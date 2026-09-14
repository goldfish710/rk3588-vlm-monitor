// chat_engine.h
// 语音对话引擎：唤醒词 → 段落文本 → DeepSeek LLM(流式 SSE) → 火山 TTS → ALSA 播放
//
// 状态机（内部线程，nice 10）：
//   IDLE --唤醒词--> AWAIT_QUERY --段落--> BUSY(LLM→TTS→PLAYING) --完成--> AWAIT_QUERY(多轮)
//   AWAIT_QUERY：唤醒进入后 10s 无输入 → IDLE；轮完成后 60s 无输入 → IDLE
//
// 回声处理：PLAYING 及播放尾 2s 通过 play_state_cb_ 通知 ASR 抑制 token 输入；
// BUSY 期间收到的段落直接丢弃（不支持抢话）。
// 多轮历史：messages 上限 12 条；system 提示词固定为简短口语化陪伴助手。
#ifndef CHAT_ENGINE_H
#define CHAT_ENGINE_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "config.h"   // ChatConfig（TLServer/config.h，与 mqtt_thread 同模式）
#include "vlm_engine.h"   // 板端 VLM（RK3588 可选替代云端 DeepSeek）
#include "tts_backend.h"  // TTS 可插拔后端（cloud 火山 / piper 本地 NPU）

struct cJSON;   // 前向声明（实现文件 include cJSON.h）

class ChatEngine {
public:
    bool init(const ChatConfig &cfg, std::string *err_out);
    void start();
    void stop();

    // ---- ASR 线程回调（必须快速返回，仅入队） ----
    void onKeyword(const std::string &kw, float conf);      // 关键词命中（唤醒词→对话；求救词→IDLE 时发 LLM）
    void onSegment(const std::string &segment, int64_t speech_end_ms);  // 说话段落完成（静音 2s；speech_end_ms=话音结束墙钟）
    // ---- 跌倒静默报警设计：跌倒检测不再进语音通道（报警接收方是监护人，
    //      语音通道只服务在场者主动发起的交互）；求救词链路（onKeyword→
    //      EV_SYSTEM_QUERY）保持不变 ----

    // ---- 跌倒复核（事件线程调用，线程安全；urgent 通道可抢占语音轮） ----
    struct FallConfirmResult { int verdict; std::string reply; };   // verdict 1=真 0=排除 2=无法确认
    // 入 urgent 队列并（语音生成中时）打断 rkllm_run；返回 future 供事件线程
    // 等待结论。引擎未运行/无 VLM 时立即返回已就绪 verdict=2 的 future（兜底安全）
    std::future<FallConfirmResult> requestFallConfirm(const std::string &image_path);

    // ---- 远程命令（MQTT 回调线程调用，必须快速返回：仅入队，解析在 chat 线程） ----
    void onRemoteCommand(const std::string &json_payload);

    // 播放状态回调（PLAYING 开始时 true / 播放尾 2s 后 false）
    // TLmain 接线到 AsrEngine::setSuppressInput 做回声门控
    void setPlayStateCallback(std::function<void(bool)> cb);

private:
    enum EventType { EV_WAKE, EV_SEGMENT, EV_SYSTEM_QUERY, EV_REMOTE_CMD };
    struct Event {
        EventType type;
        std::string text;
        float conf;
        std::string payload;      // 原始 JSON（EV_REMOTE_CMD，C 阶段）
        uint64_t req_seq;         // 命令序号（C 阶段）
        int64_t ts;               // 话音结束墙钟毫秒（EV_SEGMENT，[Voice] 打点；聚合初始化省略时为 0）
    };

    enum State { IDLE, AWAIT_QUERY, BUSY };

    void loop();
    // 处理一段用户输入（说话段落或系统事件文本）：LLM→TTS→历史→回到等待
    // skip_next_seg=true 时丢弃事件后的第一个说话段落（同句防重）
    // force_vision=true（求救词链路）或问句命中 vision_words 时抓当前帧带图给 VLM
    // speech_end_ms>0 时 [Voice] 行打"说话结束→首句播出"（全链路口径）
    void handleQuery(const std::string &query, bool skip_next_seg, bool force_vision = false,
                     int64_t speech_end_ms = 0);

    // 问句是否命中视觉触发词（vision_words 逗号分隔；含"好痛/疼"等求救类词）
    bool matchVisionWords(const std::string &query) const;

    // 段落是否命中求救词（sos_words 逗号分隔）→ 走求助视觉判定链
    bool matchSosWords(const std::string &query) const;

    // 求助视觉判定链：抓帧 → VLM 看图二次分析（sos_prompt）→ 真痛苦 MQTT 上报
    // + 安慰播报 / 假则询问播报 / 不确定 attention 上报 + 询问播报
    void handleSosQuery(const std::string &text);
    bool doLlm(const std::string &query, std::string &reply, std::string &err);      // 云端 DeepSeek
    bool doLocalLlm(const std::string &query, std::string &reply, std::string &err); // 板端 VLM
    void playReply(const std::string &text, std::string &err);   // 整段合成+播放（cloud 模式）

    // ---- 流水线（piper 模式：VLM 边生成边逐句合成播放） ----
    // 返回: 0=生成成功 1=被复核抢占(abort，本轮不播报) -1=失败
    int doLocalLlmStreaming(const std::string &query, std::string &reply, std::string &err,
                            const std::string *jpg = nullptr);   // jpg 非空 = 视觉轮（图像+流式）
    void onVlmDelta(const std::string &delta);   // chat 线程内：攒句+入队
    void playSentencesBlocking(const std::string &text); // 整段切句入队 + 等播放完成（chat 线程）
    void resetRound();                           // 轮次开始前重置流水线状态（chat 线程）
    void flushSentence(bool force = false);      // 切出一句入队（chat 线程；force=生成收尾强制入队）
    void ttsSynthWorker();                       // 句队列→合成 wav（预取深度 2）
    void ttsPlayWorker();                        // wav 队列→阻塞播放
    void finishRound();                          // 本轮播放收尾（play 线程内）
    static std::vector<std::string> splitSentences(const std::string &text);
    std::string stripWakeWord(const std::string &segment);
    void appendHistory(const std::string &role, const std::string &content);

    // ---- 跌倒复核（urgent 通道：可抢占语音轮） ----
    struct FallConfirmTask {
        std::string image_path;
        std::shared_ptr<std::promise<FallConfirmResult>> prom;
    };
    void processUrgentTasks();                    // loop 线程内：优先消费复核队列
    bool runFallConfirm(const FallConfirmTask &task);
    // 解析结构化结论（结果/原因/慰问 三行；console 出参=慰问行文本，未输出为空）
    static int parseVerdict(const std::string &raw, std::string &reason,
                            std::string *console = nullptr);

    // ---- 周期状态巡检（阶段B：VLM 一句话场景摘要 → SQLite 时间线 + MQTT） ----
    void tryPatrolTick();                         // loop 线程内：检查巡检条件并调度
    bool doPatrolRound();                         // 执行一轮巡检（抓帧→VLM→落库→推送）

    // ---- 远程命令（阶段C：chat 线程内执行，全部 VLM 调用前后 clearHistory 隔离） ----
    void handleRemoteCommand(const std::string &json_payload);
    void doRemoteSnapshot(const std::string &req_id);
    void doRemoteLastEventImage(const std::string &req_id);   // 历史异常快照（最近一次事件的现场图）
    void doRemoteChat(const std::string &text, const std::string &req_id);
    void doRemoteTimeline(int limit, const std::string &req_id);
    void doRemoteEvents(int limit, const std::string &req_id);   // 事件查询（与时间线区分）
    void doRemoteDailySummary(const std::string &req_id, bool events_only);   // 今日总结/异常回顾
    void doRemotePing(const std::string &req_id);
    void doRemoteStartStream(const std::string &req_id);   // 起 ffmpeg 转推码流到云端
    void doRemoteStopStream(const std::string &req_id);    // 停推流并回收子进程
    void stopStreamInternal();                             // 内部停流（不发应答，供 start 复用）
    void sendRemoteResp(const std::string &cmd, const std::string &req_id,
                        int code, cJSON *data, const std::string &msg);

    ChatConfig cfg_;
    std::set<std::string> wake_words_;    // 唤醒词集合（init 时从 cfg 解析）
    std::vector<std::string> vision_words_;   // 视觉触发词（问句命中即抓帧带图）
    std::vector<std::string> sos_words_;       // 求救词（段落命中即走求助视觉判定链）
    std::string companion_sys_prompt_;    // 陪伴助手人设原文（{name} 已替换；远程问答后恢复用）
    std::atomic<bool> running_{false};
    std::thread thread_;

    std::mutex ev_mtx_;
    std::condition_variable ev_cv_;
    std::deque<Event> ev_queue_;
    static const size_t kQueueMax = 32;

    std::function<void(bool)> play_state_cb_;

    VlmEngine vlm_;                  // 板端 VLM（config vlm_enable=1 时替代云端）

    // ---- 复核 urgent 队列（与主事件队列分离：任何状态优先处理） ----
    std::mutex urgent_mtx_;
    std::deque<FallConfirmTask> urgent_q_;
    static const size_t kUrgentQueueMax = 3;
    std::atomic<int> urgent_count_{0};        // urgent_q_ 长度镜像（ev_cv_ 等待谓词跨锁读取）
    std::atomic<bool> preempt_voice_{false};  // 复核要求语音静默：play/synth 线程句间丢句；
                                              // 由 requestFallConfirm 置位，下一语音轮 resetRound 清除
    // BUSY 种类（loop 线程写、事件线程读——原子，决定抢占策略：仅语音轮可被 abort 打断）
    enum BusyKind { kBusyNone = 0, kBusyVoice, kBusyConfirm, kBusyRemote, kBusyPatrol };
    std::atomic<int> busy_kind_{kBusyNone};
    std::atomic<bool> round_active_{false};   // 上一轮播报是否仍在线（resetRound 等其收尾防跨轮竞争）

    // ---- TTS 后端与流水线（synth/play 双线程：合成藏进播放时间里） ----
    std::unique_ptr<TtsBackend> tts_;
    std::thread tts_synth_thread_;
    std::thread tts_play_thread_;
    std::mutex sent_mtx_;
    std::condition_variable sent_cv_;
    std::deque<std::string> sent_q_;
    std::atomic<bool> gen_done_{false};        // chat 线程置位：本轮生成完毕
    std::atomic<bool> synth_done_{false};      // synth 线程置位：全部句合成完毕
    std::atomic<bool> playing_started_{false}; // play 线程置位：首句开始播（门控 true 只发一次）
    int64_t first_play_ms_ = 0;             // play 线程写入：本轮首句开播时刻（chat 线程算全链路耗时）
    std::atomic<bool> play_finished_{false};   // play 线程置位：本轮播放完毕
    std::mutex play_mtx_;
    std::condition_variable play_cv_;
    std::mutex wav_mtx_;
    std::condition_variable wav_cv_;
    struct SynthItem {                          // 已合成句（synth→play 内存传递，免写盘）
        std::vector<int16_t> pcm;
        unsigned rate = 0;
    };
    std::deque<SynthItem> wav_q_;
    std::string sent_buf_;          // 攒句缓冲（仅 chat 线程访问）
    int sent_seq_ = 0;              // 播放文件序号（仅 synth 线程递增）
    static const size_t kMaxSynthQueue = 4;   // 句队列背压上限
    static const size_t kMaxWavQueue = 2;     // 预合成 wav 队列上限

    // 仅 loop 线程访问
    State state_ = IDLE;
    int64_t await_enter_ms_ = 0;      // 进入 AWAIT_QUERY 的时刻
    int64_t await_timeout_ms_ = 0;    // 该次等待的超时时长（唤醒 10s / 轮后 60s）
    std::vector<std::pair<std::string, std::string>> history_;   // role, content
    uint32_t tts_seq_ = 0;            // reqid 序号
    bool skip_next_segment_ = false;  // 系统事件后丢弃第一个段落（同句防重）
    int64_t next_patrol_ms_ = 0;      // 下次巡检时刻（loop 线程私有）
    int64_t last_conv_end_ms_ = 0;    // 上次对话轮完成时刻（巡检 min_idle 判定）
    pid_t stream_pid_ = -1;           // 推流 ffmpeg 子进程 pid（-1=未推流；仅 loop 线程访问）
};

// 全局指针：EncodeThread 跌倒事件线程跨模块调用（nullptr 安全）
extern ChatEngine *g_chat_engine;

#endif // CHAT_ENGINE_H
