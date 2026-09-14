// chat_engine.cpp
// 语音对话引擎实现：DeepSeek SSE 流式 + 火山 TTS + 播放 + 多轮状态机
#include "chat_engine.h"

#include <cstdio>
#include <cstring>
#include <ctime>

#include <fcntl.h>
#include <pthread.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include "AudioPlayback.h"
#include "EventManager.h"
#include "cJSON.h"
#include "frame_provider.h"
#include "https_client.h"
#include "mqtt_client.h"   // mqtt_build_event_payload
#include "mqtt_thread.h"   // mqtt_push_raw

namespace {

int64_t now_ms()
{
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

// 墙钟时间戳 "HH:MM:SS.mmm"（[Voice] 全链路耗时行的秒表对照用）
std::string fmtTimeMs(int64_t ms)
{
    time_t sec = (time_t)(ms / 1000);
    struct tm tmv;
    localtime_r(&sec, &tmv);
    char buf[32];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03d",
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec, (int)(ms % 1000));
    return buf;
}

std::string trimCr(const std::string &s)
{
    std::string r = s;
    while (!r.empty() && (r.back() == '\r' || r.back() == '\n'))
        r.pop_back();
    return r;
}

// UTF-8 安全截断（不超过 max_bytes，且不切断多字节字符）
std::string utf8Truncate(const std::string &s, size_t max_bytes)
{
    if (s.size() <= max_bytes) return s;
    size_t end = max_bytes;
    // 从截断点向前找最近一个字符边界：UTF-8 续字节(0b10xxxxxx)不能作为字符开头
    // （旧实现从尾部回退只去续字节，会在边界恰好落在引导字节时留下孤立
    // 引导字节 → 输出末尾出现 � 乱码，实测巡检摘要复现）
    while (end > 0 && ((unsigned char)s[end] & 0xC0) == 0x80)
        end--;
    return s.substr(0, end);
}

} // namespace

// ==================== 生命周期 ====================
ChatEngine *g_chat_engine = nullptr;

bool ChatEngine::init(const ChatConfig &cfg, std::string *err_out)
{
    cfg_ = cfg;
    // 本地 VLM 模式：不需要 DeepSeek key；TTS 仍走云端（或后续本地 PiperTTS）
    if (!cfg_.vlm.enabled && cfg_.llm_api_key.empty()) {
        if (err_out) *err_out = "llm_api_key 未配置（DeepSeek key）；或开 [local_llm] vlm_enable=1 走板端 VLM";
        return false;
    }
    // TTS 后端（可插拔）：piper 模式不再需要火山凭据
    tts_ = createTtsBackend(cfg_, err_out);
    if (!tts_) {
        if (err_out && err_out->empty())
            *err_out = "TTS 后端初始化失败";
        return false;
    }
    if (cfg_.vlm.enabled) {
        // {name} 占位符替换（与云端 system_prompt 同一模板）
        std::string sys_prompt = cfg_.system_prompt;
        size_t name_pos = sys_prompt.find("{name}");
        if (name_pos != std::string::npos)
            sys_prompt.replace(name_pos, 6, cfg_.assistant_name);
        companion_sys_prompt_ = sys_prompt;   // 远程问答切换人设后恢复用
        printf("[Chat] 初始化: LLM=板端 VLM(%s), TTS=%s, 唤醒词=\"%s\"\n",
               cfg_.vlm.rkllm_model.c_str(), tts_->name(),
               cfg_.wake_word.c_str());
        if (!vlm_.init(cfg_.vlm, sys_prompt, err_out))
            return false;
    } else {
        printf("[Chat] 初始化: LLM=%s model=%s, TTS=%s, 唤醒词=\"%s\"\n",
               cfg_.llm_api_host.c_str(), cfg_.llm_model.c_str(),
               tts_->name(), cfg_.wake_word.c_str());
    }
    wake_words_.clear();
    for (const auto &w : splitCsv(cfg_.wake_word))
        wake_words_.insert(w);
    for (const auto &w : splitCsv(cfg_.vision_words))
        vision_words_.push_back(w);
    for (const auto &w : splitCsv(cfg_.sos_words))
        sos_words_.push_back(w);
    return true;
}

void ChatEngine::start()
{
    if (running_.load()) return;
    running_ = true;
    thread_ = std::thread(&ChatEngine::loop, this);
    tts_synth_thread_ = std::thread(&ChatEngine::ttsSynthWorker, this);
    tts_play_thread_ = std::thread(&ChatEngine::ttsPlayWorker, this);
}

void ChatEngine::stop()
{
    if (!running_.load()) return;
    running_ = false;
    // 打断可能正在进行的 rkllm_run（巡检/对话生成中 Ctrl+C 时，
    // chat 线程不打断的话 join 要等完整生成，退出慢 8s+）
    vlm_.cancelRun();
    ev_cv_.notify_all();
    sent_cv_.notify_all();
    wav_cv_.notify_all();
    play_cv_.notify_all();
    // 退出前满足未决复核 future（事件线程可能正阻塞等待），防 future 悬置
    {
        std::lock_guard<std::mutex> lock(urgent_mtx_);
        while (!urgent_q_.empty()) {
            auto t = std::move(urgent_q_.front());
            urgent_q_.pop_front();
            urgent_count_.fetch_sub(1);
            t.prom->set_value({2, ""});
        }
    }
    if (thread_.joinable()) thread_.join();
    if (tts_synth_thread_.joinable()) tts_synth_thread_.join();
    if (tts_play_thread_.joinable()) tts_play_thread_.join();
    if (play_state_cb_) play_state_cb_(false);
    vlm_.destroy();   // 仅 loop 线程退出后调用（无并发 run）
}

void ChatEngine::onKeyword(const std::string &kw, float conf)
{
    // 唤醒词（可多个，逗号分隔配置）
    for (const auto &w : wake_words_) {
        if (kw == w) {
            std::lock_guard<std::mutex> lock(ev_mtx_);
            if (ev_queue_.size() >= kQueueMax) ev_queue_.pop_front();
            ev_queue_.push_back({EV_WAKE, kw, conf});
            ev_cv_.notify_one();
            return;
        }
    }
    // 非唤醒词（求救词）：仅 IDLE 时发给 LLM（对话模式中该词已在识别文本流里，
    // LLM 会从段落收到，无需重复）——状态判断在 loop 线程做
    std::lock_guard<std::mutex> lock(ev_mtx_);
    if (ev_queue_.size() >= kQueueMax) ev_queue_.pop_front();
    ev_queue_.push_back({EV_SYSTEM_QUERY, kw, conf});
    ev_cv_.notify_one();
}

// 跌倒静默报警设计：不再提供 onFallEvent（跌倒走 VLM 复核→分级 MQTT 报警，
// 本地不播语音）；"求救词"主动求助链路经 onKeyword→EV_SYSTEM_QUERY 保持不变

void ChatEngine::onSegment(const std::string &segment, int64_t speech_end_ms)
{
    if (segment.empty()) return;
    std::lock_guard<std::mutex> lock(ev_mtx_);
    if (ev_queue_.size() >= kQueueMax) ev_queue_.pop_front();
    Event e;
    e.type = EV_SEGMENT;
    e.text = segment;
    e.conf = 0.0f;
    e.ts = speech_end_ms;   // 话音结束墙钟（[Voice] 全链路打点）
    ev_queue_.push_back(std::move(e));
    ev_cv_.notify_one();
}

// 远程命令入口（MQTT 回调线程）：只入队原始 JSON，解析与执行在 chat 线程
void ChatEngine::onRemoteCommand(const std::string &json_payload)
{
    if (json_payload.empty()) return;
    std::lock_guard<std::mutex> lock(ev_mtx_);
    if (ev_queue_.size() >= kQueueMax) ev_queue_.pop_front();
    Event e;
    e.type = EV_REMOTE_CMD;
    e.payload = json_payload;
    ev_queue_.push_back(std::move(e));
    ev_cv_.notify_one();
}

void ChatEngine::setPlayStateCallback(std::function<void(bool)> cb)
{
    play_state_cb_ = std::move(cb);
}

// ==================== 状态机主循环 ====================
void ChatEngine::loop()
{
    pthread_setname_np(pthread_self(), "chat_llm");
    setpriority(PRIO_PROCESS, (pid_t)syscall(SYS_gettid), 10);   // 低优先级，不抢视频/AI

    // 首次巡检：启动 interval 秒后（避免开机抢 VLM 加载后的首个使用窗口）
    next_patrol_ms_ = now_ms() + (int64_t)cfg_.vlm_pipeline.status_interval_sec * 1000;

    while (running_.load()) {
        // ---- urgent 复核优先（任何时机到达：语音生成中被 abort 打断后回到这里先处理） ----
        processUrgentTasks();

        // ---- 周期状态巡检（仅 IDLE+有人+空闲达标；与复核/对话共用 VLM 天然串行） ----
        tryPatrolTick();

        // ---- 取事件（带超时：用于 AWAIT_QUERY 超时判定） ----
        Event ev;
        bool has_ev = false;
        {
            std::unique_lock<std::mutex> lock(ev_mtx_);
            int timeout_ms = 500;
            if (state_ == AWAIT_QUERY) {
                int64_t remain = await_enter_ms_ + await_timeout_ms_ - now_ms();
                if (remain <= 0) {
                    // 等待超时 → 退出对话模式
                    state_ = IDLE;
                    printf("[Chat] 等待输入超时，退出对话模式\n");
                    history_.clear();
                    vlm_.clearHistory(true);   // 本地 VLM 清 KV 缓存（保留系统提示词）
                    continue;
                }
                if (remain < timeout_ms) timeout_ms = (int)remain;
            }
            if (ev_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                [this] { return !ev_queue_.empty() || urgent_count_.load() > 0 || !running_.load(); })) {
                if (!ev_queue_.empty()) {
                    ev = std::move(ev_queue_.front());
                    ev_queue_.pop_front();
                    has_ev = true;
                }
            }
            if (!running_.load()) break;
        }
        if (!has_ev) continue;

        // ---- 状态机处理 ----
        if (state_ == IDLE) {
            if (ev.type == EV_WAKE) {
                state_ = AWAIT_QUERY;
                await_enter_ms_ = now_ms();
                await_timeout_ms_ = (int64_t)cfg_.wake_timeout_sec * 1000;
                printf("[Chat] 唤醒成功，进入对话模式（%ds 内请说话）\n",
                       cfg_.wake_timeout_sec);
            } else if (ev.type == EV_SYSTEM_QUERY) {
                // 对话外命中求救词 → 求助视觉判定链（看图二次分析再行动）；
                // 云端模式退化为关键词文本发 LLM
                if (cfg_.vlm.enabled) handleSosQuery(ev.text);
                else handleQuery(ev.text, true);
            } else if (ev.type == EV_REMOTE_CMD) {
                // 远程命令（快照/问答/时间线/状态；VLM 忙时排队，逐条执行）
                handleRemoteCommand(ev.payload);
            }
            // EV_SEGMENT 丢弃
        } else if (state_ == AWAIT_QUERY) {
            if (ev.type == EV_SEGMENT) {
                // 系统事件后的第一个段落与事件同句，丢弃防重
                if (skip_next_segment_) {
                    skip_next_segment_ = false;
                    continue;
                }
                std::string query = stripWakeWord(ev.text);
                if (query.empty()) continue;
                if (cfg_.vlm.enabled && matchSosWords(query))
                    handleSosQuery(query);
                else
                    handleQuery(query, false, false, ev.ts);
            } else if (ev.type == EV_WAKE) {
                // 已在对话模式，重置等待计时
                await_enter_ms_ = now_ms();
                await_timeout_ms_ = (int64_t)cfg_.idle_timeout_sec * 1000;
            } else if (ev.type == EV_REMOTE_CMD) {
                handleRemoteCommand(ev.payload);
            }
            // EV_SYSTEM_QUERY 丢弃：对话中求救词已在识别文本流里，LLM 会从段落收到
        } else {
            // BUSY：**实际不可达**——所有置 BUSY 的处理函数（handleQuery / handleSosQuery /
            // runFallConfirm / doPatrolRound / doRemoteChat）都在返回前恢复了状态，而 state_
            // 只由本线程读写，所以回到这里做事件分发时状态必为 IDLE/AWAIT_QUERY。
            // 保留为防御性分支：将来若有 handler 漏恢复状态，至少不会误执行到这里的逻辑。
            // 注意：远程命令在"忙"期间不会丢失——它们排在 ev_queue_ 里，当前处理函数结束后
            // 按序执行（2026-09-13 实测验证）。
            if (ev.type == EV_WAKE) {
                await_enter_ms_ = now_ms();   // 唤醒重置，供 BUSY 完成后进入等待用
            }
        }
    }
    printf("[Chat] 对话线程退出\n");
}

// 一段用户输入（说话或系统事件）的完整处理：LLM → TTS 播放 → 历史 → 回到等待
void ChatEngine::handleQuery(const std::string &query, bool skip_next_seg, bool force_vision,
                            int64_t speech_end_ms)
{
    int64_t t_query = now_ms();   // 识别完成（段落回调）时刻——全链路耗时起点
    printf("[Chat] 用户: \"%s\"\n", query.c_str());
    state_ = BUSY;
    busy_kind_ = kBusyVoice;

    std::string reply, err;
    bool ok = false, aborted = false;
    // 统一流水线：piper/cloud 均逐句合成+播放（provider 只决定 synth 后端）；
    // 本地 VLM 走流式生成（边生成边入队），云端 LLM 整段返回后切句入队
    if (cfg_.vlm.enabled) {
        // 视觉轮：求救词链路强制带图；或问句命中视觉触发词（"看到/画面/谁在…"）
        // 抓当前帧落临时 jpg，随问句一起进 VLM（多轮 KV 保留图像上下文——
        // 模型"记得刚才看到了什么"）。抓帧失败退化为纯文本不阻塞对话
        bool need_vision = force_vision || matchVisionWords(query);
        std::string vision_jpg;
        if (need_vision) {
            cv::Mat frame = g_frame_provider.request(2000);
            if (!frame.empty()) {
                cv::imwrite("/tmp/voice_vision.jpg", frame);
                vision_jpg = "/tmp/voice_vision.jpg";
                printf("[Chat] 视觉轮：抓帧带图（%s）\n",
                       force_vision ? "求救词链路" : "命中视觉触发词");
                // 视觉轮隔离 KV：实测同一会话历史里已有图像时再跑多模态
                // 生成会输出 <|image_pad|> token 洪泛（[PAD151935] 连发，
                // 整轮变成 16s 无声义的 pad 播报）。官方 demo 每次图像调用
                // 前都 clearHistory，此处对齐（代价：视觉轮前的文字上下文
                // 丢弃，系统人设保留）
                vlm_.clearHistory(true);
            } else {
                printf("[Chat] 视觉轮：抓帧超时，退化为纯文本\n");
            }
        }
        int rc = vision_jpg.empty()
                     ? doLocalLlmStreaming(query, reply, err)
                     : doLocalLlmStreaming(query, reply, err, &vision_jpg);
        ok = (rc == 0);
        aborted = (rc == 1);
    } else {
        ok = doLlm(query, reply, err);
        if (ok && running_.load())
            playSentencesBlocking(reply);
    }
    busy_kind_ = kBusyNone;

    if (aborted) {
        // 语音轮被跌倒复核抢占（rkllm_abort）：跳过兜底播报与历史追加，
        // 直接让位——loop 顶部将先处理 urgent 复核
        printf("[Chat] 语音轮被跌倒复核抢占，跳过兜底播报\n");
        if (running_.load()) {
            state_ = AWAIT_QUERY;
            await_enter_ms_ = now_ms();
            await_timeout_ms_ = (int64_t)cfg_.idle_timeout_sec * 1000;
            skip_next_segment_ = skip_next_seg;
        }
        return;
    }
    if (!ok) {
        printf("[Chat] LLM 失败: %s\n", err.c_str());
        // 兜底播报：云端模式提示网络；本地模式不提网络（板端无网依赖）
        reply = cfg_.vlm.enabled ? "我有点累，让我休息一下"
                                 : "网络不太好，请稍后再试";
        if (running_.load())
            playSentencesBlocking(reply);
    }
    appendHistory("user", query);
    if (!reply.empty() && reply != "网络不太好，请稍后再试" &&
        reply != "我有点累，让我休息一下")
        appendHistory("assistant", reply);

    // 本轮完成 → 回到等待（多轮，家人可回应），空闲超时可配置
    if (running_.load()) {
        state_ = AWAIT_QUERY;
        await_enter_ms_ = now_ms();
        await_timeout_ms_ = (int64_t)cfg_.idle_timeout_sec * 1000;
        skip_next_segment_ = skip_next_seg;
        last_conv_end_ms_ = now_ms();   // 巡检 min_idle 判定起点（每轮结束重置）
        printf("[Chat] 本轮完成，可继续说（%ds 无说话退出）\n", cfg_.idle_timeout_sec);
        // 全链路耗时（简历关键数据）：优先"说话结束→首句播出"口径
        // （speech_end_ms 由 ASR 打点：最后一块音频的墙钟时刻）
        if (first_play_ms_ > t_query) {
            if (speech_end_ms > 0 && first_play_ms_ > speech_end_ms)
                printf("[Voice] %s 说话结束→首句播出: %lld ms\n",
                       fmtTimeMs(first_play_ms_).c_str(),
                       (long long)(first_play_ms_ - speech_end_ms));
            else
                printf("[Voice] %s 识别完成→首句播出: %lld ms\n",
                       fmtTimeMs(first_play_ms_).c_str(), (long long)(first_play_ms_ - t_query));
        }
    }
}

// 去空格 + 剥掉唤醒词（"你好 今天 天气" → "今天天气"；支持多个唤醒词）
std::string ChatEngine::stripWakeWord(const std::string &segment)
{
    std::string s;
    for (char ch : segment)
        if (ch != ' ') s.push_back(ch);
    // 剥掉第一次出现的任一唤醒词
    size_t best_pos = std::string::npos;
    size_t best_len = 0;
    for (const auto &w : wake_words_) {
        size_t pos = s.find(w);
        if (pos != std::string::npos && (best_pos == std::string::npos || pos < best_pos)) {
            best_pos = pos;
            best_len = w.size();
        }
    }
    if (best_pos != std::string::npos)
        s.erase(best_pos, best_len);
    // 首尾空白
    size_t b = s.find_first_not_of(" \t\r\n");
    size_t e = s.find_last_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    return s.substr(b, e - b + 1);
}

void ChatEngine::appendHistory(const std::string &role, const std::string &content)
{
    history_.push_back({role, content});
    while (history_.size() > 12)
        history_.erase(history_.begin());
}

// ==================== LLM 本地（板端 Qwen3-VL，RKLLM） ====================
// 多轮历史由 RKLLM 内部 KV cache 保留（keep_history=1），每轮只喂新 user 输入；
// 退出对话时 loop 已调 vlm_.clearHistory(true)
bool ChatEngine::doLocalLlm(const std::string &query, std::string &reply, std::string &err)
{
    if (!vlm_.generate(query, reply, err)) {
        printf("[Chat] VLM 失败: %s\n", err.c_str());
        return false;
    }
    printf("[Chat] AI(VLM): \"%s\"\n", reply.c_str());
    return true;
}

// ==================== LLM（DeepSeek，流式 SSE） ====================
bool ChatEngine::doLlm(const std::string &query, std::string &reply, std::string &err)
{
    for (int attempt = 0; attempt < 2; attempt++) {
        reply.clear();
        err.clear();

        // 组装请求 JSON
        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "model", cfg_.llm_model.c_str());
        cJSON_AddBoolToObject(root, "stream", true);
        cJSON_AddNumberToObject(root, "max_tokens", cfg_.max_tokens);
        cJSON_AddNumberToObject(root, "temperature", cfg_.temperature);
        // v4 默认开启思考模式（先流 reasoning_content 再流 content，首字延迟数秒），
        // 陪伴对话要快 → 显式关闭
        cJSON *thinking = cJSON_AddObjectToObject(root, "thinking");
        cJSON_AddStringToObject(thinking, "type", "disabled");
        cJSON *msgs = cJSON_AddArrayToObject(root, "messages");
        cJSON *sys = cJSON_CreateObject();
        cJSON_AddStringToObject(sys, "role", "system");
        // 提示词模板来自 config（{name} 占位符替换为助手名字）
        std::string sys_prompt = cfg_.system_prompt;
        size_t name_pos = sys_prompt.find("{name}");
        if (name_pos != std::string::npos)
            sys_prompt.replace(name_pos, 6, cfg_.assistant_name);
        cJSON_AddStringToObject(sys, "content", sys_prompt.c_str());
        cJSON_AddItemToArray(msgs, sys);
        for (const auto &m : history_) {
            cJSON *it = cJSON_CreateObject();
            cJSON_AddStringToObject(it, "role", m.first.c_str());
            cJSON_AddStringToObject(it, "content", m.second.c_str());
            cJSON_AddItemToArray(msgs, it);
        }
        cJSON *usr = cJSON_CreateObject();
        cJSON_AddStringToObject(usr, "role", "user");
        cJSON_AddStringToObject(usr, "content", query.c_str());
        cJSON_AddItemToArray(msgs, usr);
        char *json = cJSON_PrintUnformatted(root);
        std::string body(json);
        cJSON_free(json);
        cJSON_Delete(root);

        std::vector<std::pair<std::string, std::string>> headers = {
            {"Authorization", "Bearer " + cfg_.llm_api_key},
            {"Content-Type", "application/json"},
            {"Accept", "text/event-stream"},
        };
        HttpResponse resp = httpsPost(cfg_.llm_api_host, "443", "/chat/completions",
                                      headers, body, 10, 15, 60);
        if (resp.status != 200) {
            err = "HTTP " + std::to_string(resp.status) + " " + resp.error;
            // 解析错误 message（DeepSeek 错误体是 JSON error.message）
            if (!resp.body.empty()) {
                cJSON *e = cJSON_ParseWithLength((const char *)resp.body.data(), resp.body.size());
                if (e) {
                    cJSON *em = cJSON_GetObjectItem(e, "error");
                    if (em) {
                        cJSON *msg = cJSON_GetObjectItem(em, "message");
                        if (cJSON_IsString(msg)) err += " | " + std::string(msg->valuestring);
                    }
                    cJSON_Delete(e);
                }
            }
            if (attempt == 0) { usleep(500000); continue; }
            return false;
        }

        // SSE 解析：逐行 data: {json}；[DONE] 结束；忽略 : 注释行
        std::string pending((const char *)resp.body.data(), resp.body.size());
        bool done = false;
        while (!pending.empty() && !done) {
            size_t nl = pending.find('\n');
            std::string line = (nl == std::string::npos) ? pending : pending.substr(0, nl);
            if (nl == std::string::npos) pending.clear();
            else pending.erase(0, nl + 1);
            line = trimCr(line);
            if (line.empty() || line[0] == ':') continue;      // 注释/keep-alive
            if (line.compare(0, 6, "data: ") != 0) continue;
            std::string payload = line.substr(6);
            if (payload == "[DONE]") { done = true; break; }
            cJSON *ev = cJSON_Parse(payload.c_str());
            if (!ev) continue;
            cJSON *choices = cJSON_GetObjectItem(ev, "choices");
            if (cJSON_IsArray(choices) && cJSON_GetArraySize(choices) > 0) {
                cJSON *delta = cJSON_GetObjectItem(cJSON_GetArrayItem(choices, 0), "delta");
                if (delta) {
                    cJSON *content = cJSON_GetObjectItem(delta, "content");
                    if (cJSON_IsString(content))
                        reply += content->valuestring;
                }
            }
            cJSON_Delete(ev);
        }
        if (!done) {
            err = "SSE 流未正常结束";
            if (attempt == 0) { usleep(500000); continue; }
            return false;
        }
        if (reply.empty()) {
            err = "LLM 回复为空";
            if (attempt == 0) { usleep(500000); continue; }
            return false;
        }
        printf("[Chat] AI: \"%s\"\n", reply.c_str());
        return true;
    }
    return false;
}

// TTS 合成 + 播放（整段路径，cloud 模式；含回声门控：播放期间采集暂停+ASR 抑制）
void ChatEngine::playReply(const std::string &text, std::string &err)
{
    std::vector<int16_t> pcm;
    unsigned rate = 0;
    if (!tts_->synth(text, pcm, rate, err))
        return;
    const char *wav = "/tmp/tts_reply.wav";
    if (!writePcmWav(wav, pcm, rate)) {
        err = "wav 写盘失败";
        return;
    }
    if (play_state_cb_) play_state_cb_(true);
    int rc = playWavFile(wav, cfg_.playback_device);
    if (rc != 0) {
        err = "播放失败";
        if (play_state_cb_) play_state_cb_(false);
        remove(wav);
        return;
    }
    // 播放结束立即恢复:采集在播放期间已暂停(drain 完成时喇叭已静音),
    // 无回声进入,不再需要尾部抑制窗口——用户播完即可说话
    if (play_state_cb_) play_state_cb_(false);
    remove(wav);
}

// ==================== 流水线（piper 模式：边生成边逐句合成播放） ====================
// 线程模型：
//   chat_llm 线程：rkllm_run 阻塞中 on_delta 同步攒句 → 句队列（背压上限 kMaxSynthQueue）
//   tts_worker 线程：句队列消费 → TtsBackend.synth → wav 写盘 → playWavFile 阻塞播放
//   同步：chat 线程置 gen_done_ 后等 play_cv_（45s 兜底）；worker 播完全部置 play_finished_

// 整段文本按攒句规则切分（云端 LLM + piper 模式复用）
std::vector<std::string> ChatEngine::splitSentences(const std::string &text)
{
    std::vector<std::string> out;
    std::string buf;
    for (size_t i = 0; i < text.size();) {
        // 取一个 UTF-8 字符（中文 3 字节、标点 3 字节、ASCII 1 字节）
        size_t clen = 1;
        unsigned char c = (unsigned char)text[i];
        if (c >= 0xE0) clen = 3;
        else if (c >= 0xC0) clen = 2;
        if (i + clen > text.size()) clen = text.size() - i;
        std::string ch = text.substr(i, clen);
        i += clen;

        bool strong = (ch == "。" || ch == "！" || ch == "？" || ch == "…" ||
                       ch == "；" || ch == ";" || ch == "!" || ch == "?" || ch == "\n");
        bool weak = (ch == "，" || ch == "、" || ch == ",");
        buf += ch;
        bool flush = strong || (weak && buf.size() >= 8 * 3 + 3) || buf.size() >= 26 * 3 + 3;
        if (flush) {
            // 去首尾空白/纯标点残留
            std::string s;
            for (size_t k = 0; k < buf.size();) {
                size_t l = 1;
                unsigned char cc = (unsigned char)buf[k];
                if (cc >= 0xE0) l = 3;
                else if (cc >= 0xC0) l = 2;
                std::string t = buf.substr(k, l);
                k += l;
                if (t == " " || t == "\n" || t == "\t") continue;
                s += t;
            }
            if (!s.empty())
                out.push_back(s);
            buf.clear();
        }
    }
    return out;
}

// 问句是否命中视觉触发词（vision_words，逗号分隔；含"好痛/疼"等求救类词）
bool ChatEngine::matchVisionWords(const std::string &query) const
{
    for (const auto &w : vision_words_)
        if (!w.empty() && query.find(w) != std::string::npos)
            return true;
    return false;
}

// 段落是否命中求救词（sos_words 逗号分隔）→ 求助视觉判定链
bool ChatEngine::matchSosWords(const std::string &query) const
{
    for (const auto &w : sos_words_)
        if (!w.empty() && query.find(w) != std::string::npos)
            return true;
    return false;
}

// 求助视觉判定链：有人喊救命/说好痛 → 抓帧 → VLM 看图二次分析（sos_prompt）
//   真(1)：MQTT urgent 上报 + 本地安慰播报；假(0)：不报 + 询问播报；
//   不确定(2)：attention 上报 + 询问播报。抓帧/VLM 失败按不确定兜底
void ChatEngine::handleSosQuery(const std::string &text)
{
    int64_t t_query = now_ms();   // 识别完成时刻——全链路耗时起点
    printf("[SOS] 求助事件进入视觉判定链：\"%s\"\n", text.c_str());
    state_ = BUSY;
    busy_kind_ = kBusyVoice;

    int verdict = 2;
    std::string reason = "画面获取失败";
    cv::Mat frame = g_frame_provider.request(2000);
    if (frame.empty()) {
        printf("[SOS] 抓帧超时，按不确定处理\n");
    } else {
        cv::imwrite("/tmp/sos_vision.jpg", frame);
        std::string prompt = cfg_.vlm_pipeline.sos_prompt;
        size_t p = prompt.find("{text}");
        if (p != std::string::npos) prompt.replace(p, 6, text);
        vlm_.clearHistory(true);   // 视觉判定隔离 KV（防历史图像触发 PAD 洪泛）
        std::string reply, err;
        if (vlm_.generateWithImage(prompt, "/tmp/sos_vision.jpg", reply, err)) {
            verdict = parseVerdict(reply, reason);
        } else {
            printf("[SOS] VLM 判定失败: %s（按不确定处理）\n", err.c_str());
        }
        vlm_.clearHistory(true);
    }
    printf("[SOS] verdict=%d 原因=\"%s\"\n", verdict, reason.c_str());

    // MQTT 上报（复用 home/fall 通道；真=urgent 假=不上报 不确定=attention）
    if (verdict != 0) {
        MQTTEvent ev;
        ev.event = "help";
        ev.device_id = g_config.event.device_id;
        ev.confidence = 1.0f;
        ev.timestamp = time(NULL);
        ev.image = "./events/image/sos_vision.jpg";
        ev.severity = (verdict == 1) ? "urgent" : "attention";
        ev.vlm_confirm = verdict;
        ev.vlm_reply = reason;
        ev.summary = text;
        mqtt_push_event(ev);
    }

    // 本地语音出口（静默链路除外）：真→安慰；假/不确定→询问
    const std::string &reply_text = (verdict == 1) ? cfg_.vlm_pipeline.sos_true_reply
                                  : (verdict == 0) ? cfg_.vlm_pipeline.sos_false_reply
                                  : cfg_.vlm_pipeline.sos_unknown_reply;
    if (running_.load())
        playSentencesBlocking(reply_text);
    if (first_play_ms_ > t_query)
        printf("[SOS] %s 识别完成→首句播出: %lld ms\n",
               fmtTimeMs(first_play_ms_).c_str(), (long long)(first_play_ms_ - t_query));

    // 收尾：顺延巡检一个完整周期（判定链刚烧完 VLM），回到 IDLE
    if (running_.load()) {
        state_ = IDLE;
        last_conv_end_ms_ = now_ms();
        next_patrol_ms_ = now_ms() + (int64_t)cfg_.vlm_pipeline.status_interval_sec * 1000;
    }
}

// 轮次开始前重置流水线状态（chat 线程）
void ChatEngine::resetRound()
{
    // 上一轮播报仍在线时等其收尾（≤3s：被抢占轮的当前句播完+finish 通常 ≤3s），
    // 防旧 play 线程的 finishRound 污染新轮标志位；超时则强制重置（罕见路径，
    // 旧 play 线程仍持 ALSA 会话，新轮首句打开会话可能 EBUSY 丢弃，有日志）
    if (round_active_.load()) {
        std::unique_lock<std::mutex> lock(play_mtx_);
        play_cv_.wait_for(lock, std::chrono::seconds(3),
                          [this] { return play_finished_.load() || !running_.load(); });
        if (!play_finished_.load())
            printf("[Chat] ★警告★ 上一轮播报收尾超时（3s），强制重置\n");
        round_active_ = false;
    }
    preempt_voice_ = false;   // 抢占位只在轮次开始时清（复核期间保持静默）
    {
        std::lock_guard<std::mutex> lock(sent_mtx_);
        sent_q_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(wav_mtx_);
        wav_q_.clear();
    }
    gen_done_ = false;
    synth_done_ = false;
    play_finished_ = false;
    playing_started_ = false;
    sent_buf_.clear();
    wav_cv_.notify_all();   // 唤醒 play 线程（可能正在等"新一轮开始"）
    sent_cv_.notify_all();  // 唤醒 synth 线程（可能在等"新一轮开始"）
    round_active_ = true;
}

// 切出一句入队（chat 线程）。背压时句子留在 sent_buf_ 等下次 flush；
// force=true 时无视背压强制入队（生成结束收尾用，保证末句不丢）
void ChatEngine::flushSentence(bool force)
{
    if (sent_buf_.empty()) return;
    std::string s;
    for (size_t k = 0; k < sent_buf_.size();) {
        size_t l = 1;
        unsigned char cc = (unsigned char)sent_buf_[k];
        if (cc >= 0xE0) l = 3;
        else if (cc >= 0xC0) l = 2;
        if (k + l > sent_buf_.size()) l = sent_buf_.size() - k;
        std::string t = sent_buf_.substr(k, l);
        k += l;
        if (t == " " || t == "\n" || t == "\t") continue;
        s += t;
    }
    if (s.empty()) { sent_buf_.clear(); return; }
    // 防御：剥离 [PAD<数字>] token（多模态 KV 异常时模型会输出图像占位
    // token 洪泛，如不剥离会被 piper 读出来——实测 16s "哎[PAD151935]" 播报）
    {
        std::string clean;
        clean.reserve(s.size());
        for (size_t i = 0; i < s.size();) {
            if (s[i] == '[' && i + 4 < s.size() && s.compare(i, 4, "[PAD") == 0) {
                size_t e = s.find(']', i);
                if (e != std::string::npos) { i = e + 1; continue; }
            }
            clean += s[i++];
        }
        if (clean.empty()) { sent_buf_.clear(); return; }   // 全 pad：丢弃
        s = std::move(clean);
    }
    {
        std::lock_guard<std::mutex> lock(sent_mtx_);
        if (sent_q_.size() < kMaxSynthQueue || force) {
            printf("[Chat] 入队句(force=%d, 队列%zu/4): \"%s\"\n",
                   force ? 1 : 0, sent_q_.size() + 1, s.c_str());
            fflush(stdout);
            sent_q_.push_back(std::move(s));
            sent_buf_.clear();          // 入队成功才清缓冲（缓冲生命周期由本函数管理）
        } else {
            printf("[Chat] 入队背压(队列满): \"%s\"\n", s.c_str());
            fflush(stdout);
            sent_buf_ = std::move(s);   // 背压：只留去空白句文本，下次 flush 重试
        }
    }
    sent_cv_.notify_one();
}

// VLM 流式增量回调（chat_llm 线程内同步调用）：攒句+切句
void ChatEngine::onVlmDelta(const std::string &delta)
{
    for (size_t i = 0; i < delta.size();) {
        size_t clen = 1;
        unsigned char c = (unsigned char)delta[i];
        if (c >= 0xE0) clen = 3;
        else if (c >= 0xC0) clen = 2;
        if (i + clen > delta.size()) clen = delta.size() - i;
        std::string ch = delta.substr(i, clen);
        i += clen;

        bool strong = (ch == "。" || ch == "！" || ch == "？" || ch == "…" ||
                       ch == "；" || ch == ";" || ch == "!" || ch == "?" || ch == "\n");
        bool weak = (ch == "，" || ch == "、" || ch == ",");
        sent_buf_ += ch;
        bool flush = strong || (weak && sent_buf_.size() >= 8 * 3 + 3) ||
                     sent_buf_.size() >= 26 * 3 + 3;
        if (flush)
            flushSentence();   // 缓冲生命周期由 flushSentence 管理

    }
}

// 本地 VLM 流式生成（piper 流水线）：生成完等播放完才返回
// jpg 非空 = 视觉轮（图像+文本流式生成，逐句进 TTS 流水线）
// 返回: 0=生成成功 1=被复核抢占(abort，本轮不播报不等待) -1=失败
int ChatEngine::doLocalLlmStreaming(const std::string &query, std::string &reply, std::string &err,
                                    const std::string *jpg)
{
    resetRound();
    int64_t t0 = now_ms();
    bool ok = jpg
        ? vlm_.generateStreamingWithImage(
              query, *jpg, [this](const std::string &d) { onVlmDelta(d); }, reply, err)
        : vlm_.generateStreaming(
              query, [this](const std::string &d) { onVlmDelta(d); }, reply, err);
    if (!ok && vlm_.wasAborted()) {
        // 被 urgent 复核抢占：丢弃攒句残句与队列句，让 synth/play 收尾
        // （preempt_voice_ 已由 requestFallConfirm 置位，play 线程会丢排队句、
        //   当前句自然播完即静默）；本线程不等播放完成，立即让位跑复核
        sent_buf_.clear();
        {
            std::lock_guard<std::mutex> lock(sent_mtx_);
            sent_q_.clear();
        }
        gen_done_ = true;
        sent_cv_.notify_all();
        printf("[Chat] 生成被复核抢占，丢弃未播放句子（本轮不播报）\n");
        return 1;
    }
    // 残句收尾（无标点结尾的末句；force 无视背压保证末句不丢）
    flushSentence(true);
    gen_done_ = true;
    sent_cv_.notify_all();
    printf("[Chat] 生成完毕（%lld ms），等待播放完成\n", (long long)(now_ms() - t0));
    {
        std::unique_lock<std::mutex> lock(play_mtx_);
        play_cv_.wait_for(lock, std::chrono::seconds(45),
                          [this] { return play_finished_.load() || !running_.load(); });
        if (!play_finished_.load())
            printf("[Chat] ★警告★ 等待播放完成超时（45s），强制返回\n");
    }
    return ok ? 0 : -1;
}

// 整段文本 → 切句入队 → 等播放完成（云端 LLM + piper 模式；兜底文案复用）
void ChatEngine::playSentencesBlocking(const std::string &text)
{
    resetRound();
    for (const auto &s : splitSentences(text)) {
        {
            std::unique_lock<std::mutex> lock(sent_mtx_);
            sent_cv_.wait(lock, [this] { return sent_q_.size() < kMaxSynthQueue; });
            sent_q_.push_back(s);
            sent_cv_.notify_one();
        }
    }
    gen_done_ = true;
    sent_cv_.notify_all();
    {
        std::unique_lock<std::mutex> lock(play_mtx_);
        play_cv_.wait_for(lock, std::chrono::seconds(45),
                          [this] { return play_finished_.load() || !running_.load(); });
    }
}

// 本轮播放收尾（tts_worker 内）：末句播完 + 生成完毕 → 恢复采集 + 通知 chat 线程
void ChatEngine::finishRound()
{
    if (playing_started_.load()) {
        if (play_state_cb_) play_state_cb_(false);
        playing_started_ = false;
    }
    play_finished_ = true;
    round_active_ = false;
    play_cv_.notify_all();
    printf("[Chat] 本轮播报完成\n");
}

// 句队列 → 合成 wav（预取深度 kMaxWavQueue：合成藏进播放时间里，消除句间合成停顿）
void ChatEngine::ttsSynthWorker()
{
    pthread_setname_np(pthread_self(), "tts_synth");
    setpriority(PRIO_PROCESS, (pid_t)syscall(SYS_gettid), 15);

    int fail_streak = 0;
    while (running_.load()) {
        std::string sent;
        {
            std::unique_lock<std::mutex> lock(sent_mtx_);
            sent_cv_.wait(lock, [this] {
                return !sent_q_.empty() || gen_done_.load() || !running_.load();
            });
            if (!running_.load() && sent_q_.empty()) break;
            if (sent_q_.empty()) {
                if (gen_done_.load()) {
                    // 本轮全部句已合成：置 synth_done_ 通知 play 线程收尾
                    // （finishRound → 恢复采集/解除 ASR 抑制）。★历史 bug★：
                    // 此分支漏置 synth_done_，play 线程永远等不到"本轮完成"，
                    // finishRound 不执行 → play_state_cb_(false) 不触发 →
                    // 采集保持暂停+ASR token 全被丢弃（实测首次对话后无法再
                    // 唤醒、asr_text.txt 无新字、chat 线程 45s 超时强制返回）
                    synth_done_ = true;
                    wav_cv_.notify_all();
                    // 等下一轮开始（resetRound 置 gen_done_=false 并
                    // notify sent_cv_；不可 break——本线程常驻，第二轮句队列
                    // 需要继续消费）
                    sent_cv_.wait(lock, [this] {
                        return !gen_done_.load() || !sent_q_.empty() || !running_.load();
                    });
                }
                continue;
            }
            sent = std::move(sent_q_.front());
            sent_q_.pop_front();
            sent_cv_.notify_one();   // 消费者取走后通知生产者（playSentencesBlocking
                                     // 的背压等待依赖；与 wav_q_ 同类问题一并修复）
        }

        // 抢占静默：复核要求停止语音时丢弃待合成句（生成已被 abort，省 NPU）
        if (preempt_voice_.load()) continue;

        std::vector<int16_t> pcm;
        unsigned rate = 0;
        std::string err;
        int64_t ts = now_ms();
        if (!tts_->synth(sent, pcm, rate, err)) {
            printf("[Chat] 句合成失败(%s): %s\n", tts_->name(), err.c_str());
            if (++fail_streak >= 2) {
                printf("[Chat] 连续 2 句失败，弃本轮播报\n");
                {
                    std::lock_guard<std::mutex> lock(sent_mtx_);
                    sent_q_.clear();
                }
                break;
            }
            continue;
        }
        fail_streak = 0;
        printf("[Chat] 句合成 %.1fms (%d 样本): \"%s\"\n",
               (double)(now_ms() - ts), (int)pcm.size(), sent.c_str());

        {
            std::unique_lock<std::mutex> lock(wav_mtx_);
            wav_cv_.wait(lock, [this] { return wav_q_.size() < kMaxWavQueue || !running_.load(); });
            SynthItem it;
            it.pcm = std::move(pcm);
            it.rate = rate;
            wav_q_.push_back(std::move(it));
        }
        wav_cv_.notify_one();
    }
    {
        std::lock_guard<std::mutex> lock(wav_mtx_);
    }
    synth_done_ = true;
    wav_cv_.notify_all();
    printf("[Chat] tts_synth 退出\n");
}

// PCM 队列 → 连续播放会话（整轮一次 open，句间 gap；门控：首句播前 true 一次，末句播完 false）
void ChatEngine::ttsPlayWorker()
{
    pthread_setname_np(pthread_self(), "tts_play");
    setpriority(PRIO_PROCESS, (pid_t)syscall(SYS_gettid), 15);

    PlaybackSession session;
    bool session_open = false;

    while (running_.load()) {
        SynthItem item;
        {
            std::unique_lock<std::mutex> lock(wav_mtx_);
            wav_cv_.wait(lock, [this] {
                return !wav_q_.empty() || synth_done_.load() || !running_.load();
            });
            if (!running_.load() && wav_q_.empty()) break;
            if (wav_q_.empty()) {
                if (synth_done_.load()) {
                    finishRound();
                    // 等新一轮开始（resetRound 置 synth_done_=false 并 notify）
                    wav_cv_.wait(lock, [this] {
                        return !synth_done_.load() || !wav_q_.empty() || !running_.load();
                    });
                }
                continue;
            }
            item = std::move(wav_q_.front());
            wav_q_.pop_front();
            wav_cv_.notify_one();   // ★历史 bug★：消费者取走后必须通知生产者——
                                    // 合成端背压等待（wav_q_ 满）依赖此通知释放，
                                    // 漏掉后长轮播放时合成端永久冻结（实测"长笑话"
                                    // 播 8 句后三方僵持，按退出才把余句放出）
        }

        // 抢占静默：丢弃未开播句子（当前句自然播完，≤3s；复核优先于播报）
        if (preempt_voice_.load()) {
            printf("[Chat] 抢占静默：丢弃待播句\n");
            continue;
        }

        // 首句：门控 + 打开连续会话（lead 60ms 覆盖启动瞬态）
        if (!playing_started_.exchange(true)) {
            first_play_ms_ = now_ms();   // 首句开播时刻（[Voice] 全链路耗时打点）
            if (play_state_cb_) play_state_cb_(true);
        }
        if (!session_open) {
            if (!session.open(cfg_.playback_device, 60)) {
                printf("[Chat] 播放会话打开失败\n");
                continue;
            }
            session_open = true;
        }
        printf("[Playback] 句播 %uHz %.2fs\n", item.rate, (double)item.pcm.size() / item.rate);
        fflush(stdout);
        session.playPcm16(item.pcm.data(), item.pcm.size(), item.rate);
        printf("[Playback] 句播完成\n");
        fflush(stdout);

        // 句间 80ms 自然过渡；末句直接 finish（尾静音写入是历史卡死点之一）
        bool is_last;
        {
            std::lock_guard<std::mutex> lock(wav_mtx_);
            is_last = wav_q_.empty() && synth_done_.load();
        }
        if (is_last) {
            printf("[Playback] 末句收尾 finish\n");
            fflush(stdout);
            session.finish();
            session_open = false;
        } else {
            session.gap(80);
        }
    }
    if (session_open) session.finish();
    printf("[Chat] tts_play 退出\n");
}

// ==================== 跌倒复核（urgent 通道，可抢占语音轮） ====================
// 两级检测：YOLO pose 高召回候选 → 现场图 VLM 语义复核 → 分级报警。
// 复核是最高优先级任务：语音生成中 → rkllm_abort 打断；播放中 → 句间丢句静默；
// 复核期间 play/synth 线程与 VLM 无资源冲突（ALSA vs NPU），可并行收尾。

std::future<ChatEngine::FallConfirmResult> ChatEngine::requestFallConfirm(const std::string &image_path)
{
    auto prom = std::make_shared<std::promise<FallConfirmResult>>();
    std::future<FallConfirmResult> fut = prom->get_future();
    // 无 VLM/引擎未运行：立即返回"无法确认"（调用方按旧行为兜底，天然安全）
    if (!running_.load() || !cfg_.vlm.enabled || !vlm_.isReady()) {
        prom->set_value({2, ""});
        return fut;
    }
    // 置抢占位：play/synth 线程句间丢句（正在播的句子自然播完即静默）；
    // 下一语音轮 resetRound 时清除
    preempt_voice_.store(true);
    {
        std::lock_guard<std::mutex> lock(urgent_mtx_);
        if (urgent_q_.size() >= kUrgentQueueMax) {
            printf("[Chat] 复核队列满，丢弃最旧请求（其 future 提前满足=无法确认）\n");
            auto old = std::move(urgent_q_.front());
            urgent_q_.pop_front();
            urgent_count_.fetch_sub(1);
            old.prom->set_value({2, ""});
        }
        urgent_q_.push_back({image_path, prom});
        urgent_count_.fetch_add(1);
    }
    // 非复核生成中（语音/巡检/远程问答任一）：跨线程打断 rkllm_run 让复核尽快执行——
    // 否则复核要等该轮生成自然结束（实测一轮 ~8s），叠加自身 8s 必超 10s 预算。
    // ★实测项★ rkllm_abort 跨线程并发安全；异常时注释此段回退"等生成自然
    // 结束"（event_proc 侧 confirm_timeout_ms 超时兜底直报，功能仍安全）
    if (busy_kind_.load() != kBusyNone && busy_kind_.load() != kBusyConfirm)
        vlm_.cancelRun();
    ev_cv_.notify_all();   // 唤醒 chat 线程（可能正在 wait 事件）
    return fut;
}

// loop 线程内：优先消费复核队列（复核期间保存/恢复原状态）
void ChatEngine::processUrgentTasks()
{
    for (;;) {
        FallConfirmTask task;
        {
            std::lock_guard<std::mutex> lock(urgent_mtx_);
            if (urgent_q_.empty()) break;
            task = std::move(urgent_q_.front());
            urgent_q_.pop_front();
            urgent_count_.fetch_sub(1);
        }
        State prev = state_;
        runFallConfirm(task);
        if (!running_.load()) break;
        // 恢复复核前状态（AWAIT_QUERY 重置等待计时：不打断"已建立对话"体验）
        if (prev == AWAIT_QUERY) {
            state_ = AWAIT_QUERY;
            await_enter_ms_ = now_ms();
            await_timeout_ms_ = (int64_t)cfg_.idle_timeout_sec * 1000;
        } else {
            state_ = IDLE;
        }
    }
}

// 执行一次复核（chat 线程内）：vision 编码 ~2s + 短答复生成 ~1.5s
bool ChatEngine::runFallConfirm(const FallConfirmTask &task)
{
    FallConfirmResult res{2, ""};
    int64_t t0 = now_ms();
    state_ = BUSY;
    busy_kind_ = kBusyConfirm;
    // 隔离 KV：复核不污染陪伴 persona 历史（保留系统提示词免重 prefill）。
    // 代价：对话中的多轮 KV 上下文被清（紧急事件优先于会话连续性，可接受）
    vlm_.clearHistory(true);
    std::string reply, err, console;   // console=慰问行（confirm_prompt 要求"结果:是"时附一行）
    bool ok = vlm_.generateWithImage(cfg_.vlm_pipeline.confirm_prompt, task.image_path, reply, err);
    if (ok) {
        std::string reason;
        res.verdict = parseVerdict(reply, reason, &console);
        // 原因缺失时退化为全文截断（结构化输出失败兜底）
        res.reply = reason.empty() ? utf8Truncate(reply, 120) : reason;
        printf("[Confirm] 图=%s verdict=%d 原因=\"%s\" 耗时=%lldms\n",
               task.image_path.c_str(), res.verdict, res.reply.c_str(),
               (long long)(now_ms() - t0));
    } else {
        printf("[Confirm] VLM 复核失败: %s（按无法确认处理）\n", err.c_str());
    }
    vlm_.clearHistory(true);   // 复核结束清场（下一语音轮从干净 KV 开始）
    busy_kind_ = kBusyNone;
    // 复核完成后顺延下次巡检一个完整周期：刚出警的现场不值得立刻再烧一轮
    // VLM 巡检（实测曾出现"复核刚结束→巡检立刻开跑"的连轴转）
    next_patrol_ms_ = now_ms() + (int64_t)cfg_.vlm_pipeline.status_interval_sec * 1000;
    task.prom->set_value(res);
    // 真跌倒 → 现场慰问：慰问文本来自复核同一次调用（confirm_prompt 的
    // "慰问:"行，不二次调用 VLM、不重复 vision 编码——用户拍板的一体化
    // 方案）；模型未输出慰问行时用兜底文案。报警走 MQTT 给监护人、慰问
    // 走本地语音给现场，两路各司其职。set_value 之后再慰问：慰问播放耗时
    // 不进 12s 复核预算（event_proc 等待的是 future）
    if (res.verdict == 1 && running_.load()) {
        if (console.empty()) console = cfg_.vlm_pipeline.fall_console_fallback;
        printf("[Confirm] 慰问语：\"%s\"\n", console.c_str());
        if (running_.load())
            playSentencesBlocking(console);
    }
    // 结论回填 DB（fire-and-forget 入队 worker；image_path 作键，文件名含自增序号唯一）
    EventManager::getInstance()->updateEventVerdict(task.image_path, res.verdict, res.reply);
    return ok;
}

// 解析 VLM 复核结论（容错：兼容中/英文冒号、模板 token、行/段两种格式；
// 解析失败一律→2 无法确认——绝不因解析失败误判为"排除"吞掉报警）
int ChatEngine::parseVerdict(const std::string &raw, std::string &reason,
                            std::string *console)
{
    // 1) 去模板 token（<|im_end|> 等）
    std::string s;
    for (size_t i = 0; i < raw.size();) {
        if (raw[i] == '<') {
            size_t e = raw.find('>', i);
            if (e != std::string::npos) { i = e + 1; continue; }
        }
        s += raw[i++];
    }

    // 找标记（"结果"/"原因"）后的内容，并跳过中/英文冒号与空白
    auto findMark = [](const std::string &line, const std::string &mark, std::string &rest) -> bool {
        size_t p = line.find(mark);
        if (p == std::string::npos) return false;
        std::string r = line.substr(p + mark.size());
        size_t k = 0;
        while (k < r.size() && (r[k] == ':' || r[k] == '：' || r[k] == ' ' || r[k] == '\t'))
            k++;
        rest = r.substr(k);
        return true;
    };
    // 取判定词：到首个空白/标点为止
    auto firstWord = [](const std::string &r) {
        std::string w;
        for (size_t i = 0; i < r.size();) {
            size_t clen = 1;
            unsigned char c = (unsigned char)r[i];
            if (c >= 0xE0) clen = 3;
            else if (c >= 0xC0) clen = 2;
            if (i + clen > r.size()) clen = r.size() - i;
            std::string ch = r.substr(i, clen);
            if (ch == " " || ch == "\t" || ch == "。" || ch == "，" || ch == "、" ||
                ch == "；" || ch == ";" || ch == "!" || ch == "！" || ch == "\r" || ch == "\n")
                break;
            w += ch;
            i += clen;
        }
        return w;
    };

    // 2) 逐行扫描"结果:"、"原因:"与"慰问:"
    std::string verdict_word, reason_word, console_word;
    size_t start = 0;
    while (start <= s.size()) {
        size_t nl = s.find('\n', start);
        std::string line = s.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        size_t b = line.find_first_not_of(" \t\r");
        size_t e = line.find_last_not_of(" \t\r");
        if (b != std::string::npos)
            line = line.substr(b, e - b + 1);
        else
            line.clear();
        std::string rest;
        if (!line.empty() && verdict_word.empty() && findMark(line, "结果", rest))
            verdict_word = rest;
        if (!line.empty() && reason_word.empty() && findMark(line, "原因", rest))
            reason_word = rest;
        if (console && !line.empty() && console_word.empty() && findMark(line, "慰问", rest))
            console_word = rest;
        if (nl == std::string::npos) break;
        start = nl + 1;
    }

    // 3) 判定词分类
    int verdict = 2;
    if (!verdict_word.empty()) {
        std::string w = firstWord(verdict_word);
        if (!w.empty()) {
            if (w.find("无法") != std::string::npos || w.find("未知") != std::string::npos ||
                w.find("不确定") != std::string::npos || w == "2")
                verdict = 2;
            else if (w == "真" || w == "真的" || w.find("真实") != std::string::npos)
                verdict = 1;                                   // SOS 提示词用 真|假
            else if (w == "假" || w == "假的" || w.find("伪装") != std::string::npos)
                verdict = 0;
            else if (w.find("不") != std::string::npos)          // 不是/不像/不算
                verdict = 0;
            else if (w.find("是") != std::string::npos || w == "1" ||
                     w.find("肯定") != std::string::npos)
                verdict = 1;
            else if (w.find("否") != std::string::npos || w.find("没") != std::string::npos ||
                     w == "0")
                verdict = 0;
        }
    } else {
        // 4) 兜底：未输出结构化标记时全文扫描（歧义一律按 2，宁报勿漏）
        if (s.find("不确定") != std::string::npos ||
            s.find("无法判断") != std::string::npos ||
            s.find("看不清") != std::string::npos)
            verdict = 2;
        else if (s.find("不是") != std::string::npos || s.find("没有") != std::string::npos)
            verdict = 0;
        else if (s.find("是") != std::string::npos)
            verdict = 1;
        else if (s.find("否") != std::string::npos)
            verdict = 0;
        else
            verdict = 2;
    }

    // 5) 原因：优先"原因:"后内容；缺失时兜底用全文——并剥掉开头的
    // "结果:判定词"段（实测模型常把两段写在同一行："结果:是 她躺在地板上…"）
    std::string src = reason_word;
    if (src.empty()) {
        src = s;
        size_t p = src.find("结果");
        if (p != std::string::npos) {
            std::string rest = src.substr(p + std::string("结果").size());
            size_t k = 0;
            while (k < rest.size() && (rest[k] == ':' || rest[k] == '：' || rest[k] == ' ' || rest[k] == '\t'))
                k++;
            std::string w = firstWord(rest.substr(k));
            if (!w.empty())
                src = rest.substr(k + w.size());   // 判定词之后的部分才是原因
        }
    }
    size_t b = src.find_first_not_of(" \t\r\n。，；：");
    size_t e = src.find_last_not_of(" \t\r\n");
    if (b == std::string::npos)
        reason.clear();
    else {
        src = src.substr(b, e - b + 1);
        reason = utf8Truncate(src, 120);   // UTF-8 安全截断（复用统一实现）
    }
    // 慰问行：优先"慰问:"后内容；同"原因"一样剥掉可能残留的判定词前缀
    if (console) {
        std::string csrc = console_word;
        size_t cb = csrc.find_first_not_of(" \t\r\n。，；：");
        size_t ce = csrc.find_last_not_of(" \t\r\n");
        if (cb == std::string::npos)
            console->clear();
        else
            *console = utf8Truncate(csrc.substr(cb, ce - cb + 1), 40);
    }
    return verdict;
}

// ==================== 周期状态巡检（阶段B：VLM 场景摘要 → 时间线） ====================
// 调度器+执行器都是 chat_llm loop 本身：巡检与复核/对话共用 VLM 天然串行，
// 不做新线程/新锁。巡检条件（全满足才执行）：
//   status_enable && interval>0 && vlm_enabled && state==IDLE && 无人跳过 &&
//   urgent 队列空 && 距上次对话 ≥ min_idle（防与语音轮抢 NPU）
void ChatEngine::tryPatrolTick()
{
    const VlmPipelineConfig &vp = cfg_.vlm_pipeline;
    if (!vp.status_enable || vp.status_interval_sec <= 0 || !cfg_.vlm.enabled) return;
    if (state_ != IDLE) return;                       // AWAIT_QUERY/BUSY 一律跳过本周期
    if (now_ms() < next_patrol_ms_) return;
    if (urgent_count_.load() > 0) return;             // 复核在排队，先让位
    // 火警需求（用户拍板 2026-09-12）：有人无人都巡检——无人场景起火必须能被
    // 看到（VLM 巡检是唯一无新增模型的异常发现通路，发现延迟=巡检间隔 60s）
    if (now_ms() - last_conv_end_ms_ < (int64_t)vp.status_min_idle_sec * 1000) {
        // 距上次对话太近：顺延到 min_idle 之后（防刚结束的语音轮与巡检抢 NPU）
        next_patrol_ms_ = last_conv_end_ms_ + (int64_t)vp.status_min_idle_sec * 1000;
        return;
    }

    // 执行一轮（成功/失败都重设下次时刻，不累积补班；urgent 到达时由抢占位
    // 在本轮 rkllm_run 结束后打断——B 级允许本轮作废）
    doPatrolRound();
    next_patrol_ms_ = now_ms() + (int64_t)vp.status_interval_sec * 1000;
}

bool ChatEngine::doPatrolRound()
{
    const VlmPipelineConfig &vp = cfg_.vlm_pipeline;
    int64_t t0 = now_ms();
    state_ = BUSY;
    busy_kind_ = kBusyPatrol;
    printf("[Patrol] 周期巡检开始（有人=%d）\n", g_frame_provider.lastPersons());

    // 抓帧（按需快照通道；≤1 帧延迟，超时 1.5s）
    cv::Mat frame = g_frame_provider.request(1500);
    if (frame.empty()) {
        printf("[Patrol] 取帧超时，本轮跳过\n");
        busy_kind_ = kBusyNone;
        state_ = IDLE;
        return false;
    }
    // JPEG 落盘（复用事件图通道，文件名 timeline_YYYYmmdd_HHMMSS_n.jpg）
    std::vector<unsigned char> jpeg;
    cv::imencode(".jpg", frame, jpeg, {cv::IMWRITE_JPEG_QUALITY, 85});
    AIEvent ev;
    ev.type = "timeline";
    ev.device_id = g_config.event.device_id;
    ev.confidence = 0;
    ev.timestamp = time(nullptr);
    std::string img_path = EventManager::getInstance()->saveEventImageSync(ev, jpeg);
    if (img_path.empty()) {
        printf("[Patrol] 图片落盘失败，本轮跳过\n");
        busy_kind_ = kBusyNone;
        state_ = IDLE;
        return false;
    }

    // VLM 场景理解（隔离 KV：巡检不污染陪伴 persona 历史）
    vlm_.clearHistory(true);
    std::string reply, err;
    bool ok = vlm_.generateWithImage(vp.status_prompt, img_path, reply, err);
    vlm_.clearHistory(true);
    if (!ok) {
        printf("[Patrol] VLM 失败: %s（本轮丢弃，下周期重试）\n", err.c_str());
        busy_kind_ = kBusyNone;
        state_ = IDLE;
        return false;
    }
    // 摘描述：去模板 token + trim + 截断 64 字节（UTF-8 安全）
    std::string text;
    {
        std::string s;
        for (size_t i = 0; i < reply.size();) {
            if (reply[i] == '<') {
                size_t e = reply.find('>', i);
                if (e != std::string::npos) { i = e + 1; continue; }
            }
            s += reply[i++];
        }
        size_t b = s.find_first_not_of(" \t\r\n");
        size_t e = s.find_last_not_of(" \t\r\n");
        if (b != std::string::npos)
            text = utf8Truncate(s.substr(b, e - b + 1), 64);
    }
    if (text.empty()) {
        printf("[Patrol] 摘要有误（空），本轮丢弃\n");
        busy_kind_ = kBusyNone;
        state_ = IDLE;
        return false;
    }

    int persons = g_frame_provider.lastPersons();
    // 落库（异步入队 worker，非阻塞）
    EventManager::getInstance()->recordTimeline(text, img_path, persons);
    // MQTT 时间线推送（复用报警 payload 构造器；retained=false 不保留）
    MQTTEvent mqtt_ev;
    mqtt_ev.event = "timeline";
    mqtt_ev.device_id = g_config.event.device_id;
    mqtt_ev.confidence = (float)persons;
    mqtt_ev.timestamp = time(nullptr);
    mqtt_ev.image = img_path;
    mqtt_ev.summary = text;
    std::string payload = mqtt_build_event_payload(mqtt_ev);
    mqtt_push_raw(g_config.mqtt.timeline_topic, payload, false);

    // ---- 异常词升级报警（火警等）：巡检摘要命中 patrol_alert_words →
    // urgent 上报（复用 home/fall 报警通道，retained）+ 现场播报。巡检的判定
    // 本身就是 VLM 语义级判定，不再二次复核（60s 发现延迟已是最慢环节）----
    bool alert_hit = false;
    std::string alert_word;
    for (const auto &w : splitCsv(cfg_.vlm_pipeline.patrol_alert_words)) {
        if (w.empty()) continue;
        size_t p = text.find(w);
        if (p == std::string::npos) continue;
        // 否定句式守卫:"没有火灾/无明火/未发现烟雾"等否定语境不应触发
        // (2026-09-12 实测误报:摘要"没有火灾或漏电情况"命中"火灾"→误报火警)。
        // 检查命中词前 10 字符内是否含否定词;命中词在否定语境则跳过
        size_t b = (p >= 10) ? (p - 10) : 0;
        std::string prefix = text.substr(b, p - b);
        static const char *kNeg[] = {"没有", "无", "未", "不存在", "并未", "并无", "没发现", "无异常"};
        bool negated = false;
        for (const char *ng : kNeg) {
            if (prefix.find(ng) != std::string::npos) { negated = true; break; }
        }
        if (negated) {
            printf("[Patrol] 异常词\"%s\"出现在否定语境,跳过(前缀=\"%s\")\n",
                   w.c_str(), prefix.c_str());
            continue;
        }
        alert_hit = true;
        alert_word = w;
        break;
    }
    if (alert_hit) {
        printf("[Patrol] \u2605\u5f02\u5e38\u8bcd\u547d\u4e2d\u2605 \u8bcd=\"%s\" \u6458\u8981=\"%s\" \u2192 urgent \u62a5\u8b66\n",
               alert_word.c_str(), text.c_str());
        MQTTEvent fire_ev;
        fire_ev.event = "fire";
        fire_ev.device_id = g_config.event.device_id;
        fire_ev.confidence = 1.0f;
        fire_ev.timestamp = time(nullptr);
        fire_ev.image = img_path;
        fire_ev.summary = text;
        fire_ev.severity = "urgent";
        fire_ev.vlm_confirm = 1;
        mqtt_push_event(fire_ev);
        if (running_.load())
            playSentencesBlocking(cfg_.vlm_pipeline.fire_alert_reply);
    }

    printf("[Patrol] 摘要=\"%s\" 人数=%d 图=%s 耗时=%lldms\n",
           text.c_str(), persons, img_path.c_str(), (long long)(now_ms() - t0));
    busy_kind_ = kBusyNone;
    state_ = IDLE;
    return true;
}

// ==================== 远程操控（阶段C：公网 MQTT 命令协议） ====================
// 协议（下行 home/{device_id}/cmd）：
//   {"cmd":"snapshot|chat|timeline|events|daily_summary|daily_events|last_event_image|ping|start_stream|stop_stream",
//    "req_id":"uuid","auth":"token","text":"chat 必填","limit":10}
// 应答（上行 home/{device_id}/resp）：{"cmd","req_id","code":0,"ts","data":{...}}
//   code!=0 → data.msg 错误原因（401 未授权/404 取帧失败/503 VLM 忙/500 推流失败）
// start_stream: 板端起 ffmpeg 把子码流(rtsp://127.0.0.1:8554/h264)零转码转推 RTMP 到
//   云端 MediaMTX（按需推流：GB28181 推流模型/消费级监控信令架构的个人简化版）
// stop_stream: 停推流并回收子进程
// 安全：cmd_token 不等长比较（防时序攻击）；公网部署必须设置（配置文件注释说明）

// 应答 JSON 组装（chat 线程内调用 → mqtt_push_raw 异步发送，不阻塞）
void ChatEngine::sendRemoteResp(const std::string &cmd, const std::string &req_id,
                                int code, cJSON *data, const std::string &msg)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "cmd", cmd.c_str());
    cJSON_AddStringToObject(root, "req_id", req_id.c_str());
    cJSON_AddNumberToObject(root, "code", code);
    char ts[32];
    time_t now = time(nullptr);
    struct tm tmv;
    localtime_r(&now, &tmv);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmv);
    cJSON_AddStringToObject(root, "ts", ts);
    if (code == 0 && data) {
        cJSON_AddItemToObject(root, "data", data);
    } else {
        cJSON *d = cJSON_CreateObject();
        cJSON_AddStringToObject(d, "msg", msg.c_str());
        cJSON_AddItemToObject(root, "data", d);
    }
    char *json = cJSON_PrintUnformatted(root);
    mqtt_push_raw(g_config.mqtt.resp_topic, std::string(json), false);
    cJSON_free(json);
    cJSON_Delete(root);
}

// 命令入口（chat 线程）：解析 → auth 校验 → 按 cmd 分派；执行完恢复原状态
void ChatEngine::handleRemoteCommand(const std::string &json_payload)
{
    if (!cfg_.vlm_pipeline.remote_enable) return;   // 总开关关：忽略（不回应答）

    cJSON *root = cJSON_Parse(json_payload.c_str());
    if (!root) {
        printf("[Remote] 非法 JSON，忽略\n");
        return;
    }
    std::string cmd, req_id, auth, text;
    int limit = 10;
    cJSON *it = cJSON_GetObjectItem(root, "cmd");
    if (cJSON_IsString(it)) cmd = it->valuestring;
    it = cJSON_GetObjectItem(root, "req_id");
    if (cJSON_IsString(it)) req_id = it->valuestring;
    it = cJSON_GetObjectItem(root, "auth");
    if (cJSON_IsString(it)) auth = it->valuestring;
    it = cJSON_GetObjectItem(root, "text");
    if (cJSON_IsString(it)) text = it->valuestring;
    it = cJSON_GetObjectItem(root, "limit");
    if (cJSON_IsNumber(it)) {
        limit = it->valueint;
        if (limit <= 0 || limit > 50) limit = 10;
    }

    // auth 校验（不等长比较防时序攻击；失败仅回 401，不执行）
    const std::string &token = cfg_.vlm_pipeline.remote_token;
    if (!token.empty()) {
        bool ok_auth = (auth.size() == token.size());
        if (ok_auth) {
            int acc = 0;
            for (size_t i = 0; i < token.size(); i++)
                acc |= (int)(unsigned char)auth[i] ^ (unsigned char)token[i];
            ok_auth = (acc == 0);
        }
        if (!ok_auth) {
            printf("[Remote] 命令未授权（cmd=%s req=%s）\n", cmd.c_str(), req_id.c_str());
            sendRemoteResp(cmd, req_id, 401, nullptr, "unauthorized");
            cJSON_Delete(root);
            return;
        }
    }

    printf("[Remote] 收到命令: %s (req=%s)\n", cmd.c_str(), req_id.c_str());
    State prev = state_;

    if (cmd == "ping") {
        doRemotePing(req_id);
    } else if (cmd == "snapshot") {
        doRemoteSnapshot(req_id);
    } else if (cmd == "last_event_image") {
        doRemoteLastEventImage(req_id);
    } else if (cmd == "chat") {
        doRemoteChat(text, req_id);
    } else if (cmd == "timeline") {
        doRemoteTimeline(limit, req_id);
    } else if (cmd == "events") {
        doRemoteEvents(limit, req_id);
    } else if (cmd == "daily_summary") {
        doRemoteDailySummary(req_id, false);   // 今天发生了什么（活动总结）
    } else if (cmd == "daily_events") {
        doRemoteDailySummary(req_id, true);    // 今日异常回顾（事件总结）
    } else if (cmd == "start_stream") {
        doRemoteStartStream(req_id);
    } else if (cmd == "stop_stream") {
        doRemoteStopStream(req_id);
    } else {
        sendRemoteResp(cmd, req_id, 400, nullptr, "unknown cmd");
    }

    // 恢复命令前状态（AWAIT_QUERY 重置等待计时：远程命令不打断对话等待）
    if (running_.load()) {
        if (prev == AWAIT_QUERY) {
            state_ = AWAIT_QUERY;
            await_enter_ms_ = now_ms();
            await_timeout_ms_ = (int64_t)cfg_.idle_timeout_sec * 1000;
        } else {
            state_ = IDLE;
        }
    }
    cJSON_Delete(root);
}

// 历史异常快照：查最近一次事件（fall 等）落盘的现场图并回传。
// 事件图为全分辨率 JPEG（1-2MB），缩到长边 320（与 snapshot 同款，控制 MQTT 消息体积）。
void ChatEngine::doRemoteLastEventImage(const std::string &req_id)
{
    std::vector<TimelineRow> rows;
    if (!EventManager::getInstance()->queryRecent(1, "nontimeline", false, rows)) {
        sendRemoteResp("last_event_image", req_id, 500, nullptr, "查询失败");
        return;
    }
    if (rows.empty() || rows[0].image_path.empty()) {
        sendRemoteResp("last_event_image", req_id, 404, nullptr, "暂无历史事件图");
        return;
    }
    cv::Mat img = cv::imread(rows[0].image_path);
    if (img.empty()) {
        sendRemoteResp("last_event_image", req_id, 404, nullptr, "图片文件不存在");
        return;
    }
    cv::Mat thumb;
    float scale = 320.0f / std::max(img.cols, img.rows);
    if (scale < 1.0f)
        cv::resize(img, thumb, cv::Size(), scale, scale, cv::INTER_AREA);
    else
        thumb = img;
    std::vector<unsigned char> jpeg;
    cv::imencode(".jpg", thumb, jpeg, {cv::IMWRITE_JPEG_QUALITY, 85});
    std::string b64 = mqtt_base64_encode(jpeg.data(), jpeg.size());

    char ts[32];
    struct tm tmv;
    localtime_r(&rows[0].ts, &tmv);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmv);

    cJSON *d = cJSON_CreateObject();
    cJSON_AddStringToObject(d, "image_base64", b64.c_str());
    cJSON_AddStringToObject(d, "type", rows[0].event_type.c_str());
    cJSON_AddStringToObject(d, "time", ts);
    cJSON_AddNumberToObject(d, "w", thumb.cols);
    cJSON_AddNumberToObject(d, "h", thumb.rows);
    sendRemoteResp("last_event_image", req_id, 0, d, "");
    printf("[Remote] 历史事件图已回传 (%s, %s, %dx%d)\n",
           rows[0].event_type.c_str(), ts, thumb.cols, thumb.rows);
}

void ChatEngine::doRemotePing(const std::string &req_id)
{
    cJSON *d = cJSON_CreateObject();
    cJSON_AddStringToObject(d, "state", "online");
    cJSON_AddNumberToObject(d, "persons", g_frame_provider.lastPersons());
    sendRemoteResp("ping", req_id, 0, d, "");
}

void ChatEngine::doRemoteSnapshot(const std::string &req_id)
{
    // 抓帧（无人也可拍：采集线程在未绘图路径检测 wanted() 时同样投递）
    cv::Mat frame = g_frame_provider.request(2000);
    if (frame.empty()) {
        sendRemoteResp("snapshot", req_id, 404, nullptr, "取帧失败（无帧/超时）");
        return;
    }
    // 长边 320 缩略（控制 MQTT 消息体积）
    cv::Mat thumb;
    float scale = 320.0f / std::max(frame.cols, frame.rows);
    if (scale < 1.0f)
        cv::resize(frame, thumb, cv::Size(), scale, scale, cv::INTER_AREA);
    else
        thumb = frame;
    std::vector<unsigned char> jpeg;
    cv::imencode(".jpg", thumb, jpeg, {cv::IMWRITE_JPEG_QUALITY, 85});
    std::string b64 = mqtt_base64_encode(jpeg.data(), jpeg.size());

    cJSON *d = cJSON_CreateObject();
    cJSON_AddStringToObject(d, "image_base64", b64.c_str());
    cJSON_AddNumberToObject(d, "w", thumb.cols);
    cJSON_AddNumberToObject(d, "h", thumb.rows);
    sendRemoteResp("snapshot", req_id, 0, d, "");
    printf("[Remote] 快照已回传 (%dx%d, %zu bytes)\n", thumb.cols, thumb.rows, jpeg.size());
}

void ChatEngine::doRemoteChat(const std::string &text, const std::string &req_id)
{
    if (text.empty()) {
        sendRemoteResp("chat", req_id, 400, nullptr, "text 为空");
        return;
    }
    // 抓帧 + VLM 视觉问答（取帧失败退化为纯文本）；KV 隔离不污染陪伴历史
    cv::Mat frame = g_frame_provider.request(2000);
    std::string reply, err;
    bool ok = false;
    state_ = BUSY;
    busy_kind_ = kBusyRemote;
    vlm_.clearHistory(true);
    // 切"场景观察员"人设：远程问答应客观描述画面，而非陪伴助手口吻
    // （实测不切换时"现在发生了什么"得到"你是不是在用手机拍啥"类闲聊）
    vlm_.setSystemPrompt(cfg_.vlm_pipeline.remote_chat_prompt);
    if (!frame.empty()) {
        // 临时图落盘（generateWithImage 接口取 jpg 路径）
        static const char *kRemoteJpg = "/tmp/remote_chat.jpg";
        cv::imwrite(kRemoteJpg, frame);
        ok = vlm_.generateWithImage(text, kRemoteJpg, reply, err);
    } else {
        ok = vlm_.generate(text, reply, err);   // 纯文本路径
    }
    vlm_.setSystemPrompt(companion_sys_prompt_);   // 恢复陪伴助手人设
    vlm_.clearHistory(true);
    busy_kind_ = kBusyNone;

    // 问答结束顺延下次巡检一个完整周期：远程问答刚烧完一轮 VLM，巡检立刻
    // 开跑会连轴占用 NPU（与复核/语音问答的顺延逻辑一致，实测用户反馈
    // "刚问答完又立马触发周期巡检"）
    next_patrol_ms_ = now_ms() + (int64_t)cfg_.vlm_pipeline.status_interval_sec * 1000;

    if (!ok) {
        printf("[Remote] chat VLM 失败: %s\n", err.c_str());
        sendRemoteResp("chat", req_id, 503, nullptr, "VLM 忙/失败");
        return;
    }
    printf("[Remote] chat 答复: \"%s\"\n", reply.c_str());
    cJSON *d = cJSON_CreateObject();
    cJSON_AddStringToObject(d, "reply", reply.c_str());
    sendRemoteResp("chat", req_id, 0, d, "");

    // 默认不播报（避免远程回复突然在房间出声）；remote_voice=1 时播
    if (cfg_.vlm_pipeline.remote_voice && running_.load())
        playSentencesBlocking(reply);
}

// 时间线查询：**巡检摘要 + 事件混排**（时间流视角，跌倒/火警插在对应时刻；
// 2026-09-13 用户确认：时间线就应该是完整的"一天发生了什么"，事件不该被隐藏）。
// 正序返回（最新的排最后，观感为"从早到晚的一天流水"）。
void ChatEngine::doRemoteTimeline(int limit, const std::string &req_id)
{
    std::vector<TimelineRow> rows;
    if (!EventManager::getInstance()->queryRecent(limit, "", true, rows)) {
        sendRemoteResp("timeline", req_id, 500, nullptr, "查询失败");
        return;
    }
    cJSON *items = cJSON_CreateArray();
    for (const auto &r : rows) {
        cJSON *it = cJSON_CreateObject();
        char ts[32];
        struct tm tmv;
        localtime_r(&r.ts, &tmv);
        strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmv);
        cJSON_AddStringToObject(it, "time", ts);
        cJSON_AddStringToObject(it, "type", r.event_type.c_str());
        cJSON_AddStringToObject(it, "text", r.text.c_str());
        cJSON_AddStringToObject(it, "image", r.image_path.c_str());
        if (r.vlm_confirm >= 0)
            cJSON_AddNumberToObject(it, "vlm_confirm", r.vlm_confirm);
        if (!r.vlm_reply.empty())
            cJSON_AddStringToObject(it, "vlm_reply", r.vlm_reply.c_str());   // 事件复核意见
        cJSON_AddItemToArray(items, it);
    }
    cJSON *d = cJSON_CreateObject();
    cJSON_AddItemToObject(d, "items", items);
    sendRemoteResp("timeline", req_id, 0, d, "");
}

// 事件查询（原 events 命令错误复用 timeline 查询导致与时间线重复——2026-09-13 修正）
void ChatEngine::doRemoteEvents(int limit, const std::string &req_id)
{
    std::vector<TimelineRow> rows;
    if (!EventManager::getInstance()->queryRecent(limit, "nontimeline", true, rows)) {
        sendRemoteResp("events", req_id, 500, nullptr, "查询失败");
        return;
    }
    cJSON *items = cJSON_CreateArray();
    for (const auto &r : rows) {
        cJSON *it = cJSON_CreateObject();
        char ts[32];
        struct tm tmv;
        localtime_r(&r.ts, &tmv);
        strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmv);
        cJSON_AddStringToObject(it, "time", ts);
        cJSON_AddStringToObject(it, "type", r.event_type.c_str());
        cJSON_AddStringToObject(it, "text", r.text.c_str());
        cJSON_AddStringToObject(it, "image", r.image_path.c_str());
        if (r.vlm_confirm >= 0)
            cJSON_AddNumberToObject(it, "vlm_confirm", r.vlm_confirm);
        if (!r.vlm_reply.empty())
            cJSON_AddStringToObject(it, "vlm_reply", r.vlm_reply.c_str());   // 事件复核意见
        cJSON_AddItemToArray(items, it);
    }
    cJSON *d = cJSON_CreateObject();
    cJSON_AddItemToObject(d, "items", items);
    sendRemoteResp("events", req_id, 0, d, "");
}

// 今日总结（VLM 把当天记录压缩成 2-3 句话）：
//   daily_summary = 今日活动总结（巡检时间线）
//   daily_events  = 今日异常回顾（fall/fire/help 等事件；无事件直接回复不烧 VLM）
// 记录采样上限 40 条防 prompt 过长；与远程问答一样用完清 KV、顺延巡检。
void ChatEngine::doRemoteDailySummary(const std::string &req_id, bool events_only)
{
    const char *resp_cmd = events_only ? "daily_events" : "daily_summary";

    // 一天最多 1440 条巡检记录（60s 一条），全量拉回再过滤"今日"
    std::vector<TimelineRow> rows;
    if (!EventManager::getInstance()->queryRecent(1440,
            events_only ? "nontimeline" : "timeline", false, rows)) {
        sendRemoteResp(resp_cmd, req_id, 500, nullptr, "查询失败");
        return;
    }
    // 今日零点时间戳（本地时区）
    time_t now = time(nullptr);
    struct tm tmv;
    localtime_r(&now, &tmv);
    tmv.tm_hour = 0; tmv.tm_min = 0; tmv.tm_sec = 0;
    time_t today0 = mktime(&tmv);

    std::vector<const TimelineRow *> today;
    for (const auto &r : rows)
        if (r.ts >= today0) today.push_back(&r);

    if (today.empty()) {
        cJSON *d = cJSON_CreateObject();
        cJSON_AddStringToObject(d, "reply",
            events_only ? "今日暂无异常事件记录。"
                        : "今天还没有任何巡检记录。");
        sendRemoteResp(resp_cmd, req_id, 0, d, "");
        return;
    }

    // 时间正序（queryRecent DESC 返回，倒序拼接观感差——2026-09-14 一并修）
    std::reverse(today.begin(), today.end());

    // ---- daily_events：程序统计 + VLM 润色 + 校验兜底 ----
    // 2026-09-14 实测翻车：输入里明写【复核:确认为真】6 条含 3 真，2B/4B 均输出
    // "均为假警报"还编造次数——"让模型从记录中统计"这个任务它做不了。
    // 修法：统计由程序完成（确定性报表），VLM 只润色；输出校验失败用代码模板兜底。
    std::string reply, err;
    if (events_only) {
        int confirmed = 0, excluded = 0, unknown = 0;
        std::string confirmed_times;
        for (const TimelineRow *r : today) {
            if (r->vlm_confirm == 1) {
                confirmed++;
                struct tm tv;
                localtime_r(&r->ts, &tv);
                char ts[8];
                strftime(ts, sizeof(ts), "%H:%M", &tv);
                if (!confirmed_times.empty()) confirmed_times += "、";
                confirmed_times += ts;
            } else if (r->vlm_confirm == 0) excluded++;
            else if (r->vlm_confirm == 2) unknown++;
        }
        // 代码模板兜底文案（VLM 不可用时直出）
        std::string fallback = "今日跌倒检测触发 " + std::to_string(today.size()) + " 次：";
        if (confirmed == 0) {
            fallback += "复核后均排除，无真实异常。";
        } else {
            fallback += "确认为真 " + std::to_string(confirmed) + " 次（" + confirmed_times +
                        "），已排除 " + std::to_string(excluded) + " 次，无法确认 " +
                        std::to_string(unknown) + " 次。";
        }

        // 结论先行的输入：统计是权威、明细仅参考
        std::string log_text =
            "【程序统计结果（权威结论，数字不得改动）】今日跌倒检测共触发 " +
            std::to_string(today.size()) + " 次；确认为真 " + std::to_string(confirmed) +
            " 次（" + (confirmed_times.empty() ? "无" : confirmed_times) + "）；已排除 " +
            std::to_string(excluded) + " 次；无法确认 " + std::to_string(unknown) + " 次。\n"
            "以下为记录明细（仅供参考）：\n";
        for (const TimelineRow *r : today) {
            struct tm tv;
            localtime_r(&r->ts, &tv);
            char ts[8];
            strftime(ts, sizeof(ts), "%H:%M", &tv);
            log_text += ts;
            log_text += " ";
            log_text += r->text;
            log_text += "\n";
        }
        log_text += "请把上述统计结果用通顺的中文写成 2-3 句话，数字必须与统计一致。";

        printf("[Remote] 今日总结输入(%s, %zu 条):\n%s\n", resp_cmd, today.size(), log_text.c_str());

        vlm_.clearHistory(true);
        vlm_.setSystemPrompt(
            "你是智能家居监控终端的报告助手。输入是设备的事件记录文本（不是画面），"
            "请严格基于记录内容用中文总结，禁止提及\"画面/图像/视频\"，禁止免责声明。");
        bool ok = vlm_.generate(log_text, reply, err);
        vlm_.setSystemPrompt(companion_sys_prompt_);
        vlm_.clearHistory(true);
        next_patrol_ms_ = now_ms() + (int64_t)cfg_.vlm_pipeline.status_interval_sec * 1000;

        if (!ok) {
            printf("[Remote] 今日总结 VLM 失败，用代码模板: %s\n", err.c_str());
            reply = fallback;
        } else {
            // 校验：确认为真 >0 时输出必须含该数字且无"均假/均排除"类否定；
            //       确认为真 ==0 时输出必须含"无/没有/均"。
            bool valid;
            if (confirmed > 0) {
                valid = reply.find(std::to_string(confirmed)) != std::string::npos &&
                        !(reply.find("均") != std::string::npos &&
                          reply.find("假") != std::string::npos);
            } else {
                valid = reply.find("无") != std::string::npos ||
                        reply.find("没有") != std::string::npos;
            }
            if (!valid) {
                printf("[Remote] 今日总结 VLM 输出校验失败，用代码模板: \"%s\"\n", reply.c_str());
                reply = fallback;
            }
        }
        printf("[Remote] 今日总结(%s): \"%s\"\n", resp_cmd, reply.c_str());
        cJSON *d = cJSON_CreateObject();
        cJSON_AddStringToObject(d, "reply", reply.c_str());
        sendRemoteResp(resp_cmd, req_id, 0, d, "");
        return;
    }

    // ---- daily_summary：活动总结（VLM 压缩巡检摘要，任务适配性已实测） ----
    size_t step = (today.size() > 40) ? today.size() / 40 : 1;
    std::string log_text = "以下是设备今天的场景巡检记录（时间+摘要）：\n";
    for (size_t i = 0; i < today.size(); i += step) {
        const TimelineRow *r = today[i];
        struct tm tv;
        localtime_r(&r->ts, &tv);
        char ts[16];
        strftime(ts, sizeof(ts), "%H:%M", &tv);
        log_text += ts;
        log_text += " ";
        log_text += r->text;
        log_text += "\n";
    }
    log_text += "请用 2-3 句话总结今天的情况：大致活动、画面里出现的人数、有无异常。";
    log_text += "直接输出总结本身，不要解释过程，不要免责声明。";

    printf("[Remote] 今日总结输入(%s, %zu 条):\n%s\n", resp_cmd, today.size(), log_text.c_str());

    vlm_.clearHistory(true);
    vlm_.setSystemPrompt(
        "你是智能家居监控终端的报告助手。输入是设备的事件记录文本（不是画面），"
        "请严格基于记录内容用中文总结，禁止提及\"画面/图像/视频\"，禁止免责声明。");
    bool ok = vlm_.generate(log_text, reply, err);
    vlm_.setSystemPrompt(companion_sys_prompt_);
    vlm_.clearHistory(true);
    next_patrol_ms_ = now_ms() + (int64_t)cfg_.vlm_pipeline.status_interval_sec * 1000;

    if (!ok) {
        printf("[Remote] 今日总结 VLM 失败: %s\n", err.c_str());
        sendRemoteResp(resp_cmd, req_id, 503, nullptr, "VLM 忙/失败");
        return;
    }
    printf("[Remote] 今日总结(%s): \"%s\"\n", resp_cmd, reply.c_str());
    cJSON *d = cJSON_CreateObject();
    cJSON_AddStringToObject(d, "reply", reply.c_str());
    sendRemoteResp(resp_cmd, req_id, 0, d, "");
}

// ==================== 远程实时视频（按需推流） ====================
// 架构：MQTT 信令触发 → 板端 ffmpeg 零转码转推子码流 → 云端 MediaMTX 转 HLS → 网页播放。
// 对齐 GB28181 推流模型/消费级监控信令架构（工业差异：P2P 省带宽，个人演示走云转发）。
//
// 简化取舍（2026-09-13 用户拍板，裁剪项与补法见开发日志）：
//   ① 无"无人观看自动停流"——忘关流会持续占带宽，需手动 stop_stream；
//      补法：loop 顶部加 tryStreamIdleTick（与 tryPatrolTick 同款模式）
//   ② 无板端退出清理——板端退出后 ffmpeg 成孤儿，源断几秒内自行退出（无害）；
//      补法：ChatEngine::stop() 加 kill
// 线程约束：本函数在 chat 线程执行（VLM 串行执行者）。fork+exec 毫秒级可就地做；
//   唯一同步等待是 stop 时的 waitpid 轮询（上限 1s），可接受。

// 内部停流：kill + 回收僵尸，不发应答。
// 返回时保证 stream_pid_ 已重置，僵尸必被回收。
// SIGTERM 宽限 1s，不退出再 SIGKILL。
void ChatEngine::stopStreamInternal()
{
    if (stream_pid_ <= 0) return;
    pid_t pid = stream_pid_;
    stream_pid_ = -1;

    kill(pid, SIGTERM);
    int status = 0;
    for (int i = 0; i < 20; i++) {          // 最多等 1s
        if (waitpid(pid, &status, WNOHANG) == pid) break;
        usleep(50 * 1000);
    }
    if (waitpid(pid, &status, WNOHANG) == 0) {
        kill(pid, SIGKILL);                 // SIGTERM 不响应 → 强制杀
        waitpid(pid, &status, 0);
    }
    printf("[Stream] 推流已停止 (pid=%d)\n", (int)pid);
}

void ChatEngine::doRemoteStopStream(const std::string &req_id)
{
    bool had_stream = (stream_pid_ > 0);
    stopStreamInternal();
    cJSON *d = cJSON_CreateObject();
    cJSON_AddStringToObject(d, "state", had_stream ? "stopped" : "idle");
    sendRemoteResp("stop_stream", req_id, 0, d, "");
}

// 起 ffmpeg 子进程：把板端码流（源可配：h264=子码流带AI框 / h265=主码流）零转码转推到云端 RTMP。
void ChatEngine::doRemoteStartStream(const std::string &req_id)
{
    if (cfg_.vlm_pipeline.stream_rtmp_url.empty()) {
        sendRemoteResp("start_stream", req_id, 500, nullptr, "stream_rtmp_url 未配置");
        return;
    }
    stopStreamInternal();   // 已在推 → 先停旧的（内部函数不发应答，避免 req_id 被复用）

    pid_t pid = fork();
    if (pid < 0) {
        sendRemoteResp("start_stream", req_id, 500, nullptr, "fork 失败");
        return;
    }
    if (pid == 0) {
        // ---- 子进程：stderr 落盘便于排障，stdout 丢弃，exec ffmpeg ----
        int fd = open("/tmp/stream_push.log",
                      O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            dup2(fd, 2);
            close(fd);
        }
        int nullfd = open("/dev/null", O_WRONLY);
        if (nullfd >= 0) { dup2(nullfd, 1); close(nullfd); }

        const char *url = cfg_.vlm_pipeline.stream_rtmp_url.c_str();
        const char *src = cfg_.vlm_pipeline.stream_src_url.c_str();
        execlp("ffmpeg", "ffmpeg",
               "-nostdin", "-loglevel", "warning",
               "-rtsp_transport", "tcp",
               "-i", src,                             // 源可配：h264=子码流带AI框 / h265=主码流高清
               "-c", "copy",                          // 零转码（只换容器）
               "-an",                                 // 纯视频
               "-f", "flv",
               url,
               (char *)nullptr);
        _exit(127);   // exec 失败（execlp 返回说明没找到 ffmpeg）
    }

    stream_pid_ = pid;
    printf("[Stream] 推流已启动 (pid=%d, url=%s)\n", (int)pid,
           cfg_.vlm_pipeline.stream_rtmp_url.c_str());
    cJSON *d = cJSON_CreateObject();
    cJSON_AddStringToObject(d, "state", "starting");
    sendRemoteResp("start_stream", req_id, 0, d, "");
}


