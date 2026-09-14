// asr_thread.h
// zipformer 流式语音识别引擎（三模型 RKNN + fbank + 贪心搜索）
// 职责：采集线程喂入 44.1kHz 多声道 PCM → 降混/重采样 → 流式识别
//       → 本地关键词匹配（呼救词）→ 事件回调（由 TLmain 转 MQTT）
//
// 移植自官方 rknn_model_zoo/examples/zipformer（Apache-2.0），
// 关键改造：官方 demo 是一次性喂完整段 wav 再循环推理；本引擎是
// 增量流式（持续运行），因此新增：
//   1. 重叠 hop 水位策略（每 chunk 消费 103 帧只前进 96 帧，重叠 7 帧，
//      水位必然持续增长，必须主动跳轮排空）
//   2. 静音门控（AudioCapture::silent() 为真时丢弃 PCM 不喂 fbank，省 NPU）
//   3. 尾段补零 flush（stop 时保证最后一段话音不丢）
//
// 线程模型：feedPcm 在采集线程调用（仅降混+入队，无重活）；
// 推理全部在内部 ASR 线程（低优先级）。
#ifndef ASR_THREAD_H
#define ASR_THREAD_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "rknn_api.h"

// 与 TLServer/AudioCapture.h 的 AudioChunk 保持一致
// （本文件在 cpp/ 下，include path 已含 TLServer 目录）
#include "AudioCapture.h"

// AsrConfig 定义在 TLServer/config.h（与 mqtt_thread.h 同模式），
// 采集格式字段由 TLmain 按 g_config.record 填写
#include "config.h"

// OnlineFbank 是 using 别名（非 class），无法前置声明，只能直接 include
#include "kaldi-native-fbank/csrc/online-feature.h"

class AsrEngine {
public:
    // 加载三模型 + vocab + fbank。silence_probe 返回 true=当前静音（注入 AudioCapture::silent()）
    bool init(const AsrConfig &cfg, std::function<bool()> silence_probe, std::string *err_out);
    // 启动 ASR 线程；若 cfg.debug_wav_path 非空，init 内已同步跑过离线验证
    void start();
    // 停线程 + 尾段补零 flush（阻塞至识别线程退出）
    void stop();
    // 采集线程调用：仅降混 + 入有界队列，绝无重活
    void feedPcm(const AudioChunk &chunk);
    // 命中关键词回调（多槽，ASR 线程内调用，须线程安全且快速返回）
    // conf = 命中 token 的 joiner softmax 概率
    void addEventCallback(std::function<void(const std::string &keyword, float conf)> cb);
    // 说话段落完成回调（静音 2s 判段，ASR 线程内调用，须快速返回）
    // cb 第二参：本段话音结束时刻（最后一块音频的墙钟毫秒，块粒度内精确）
    void setSegmentCallback(std::function<void(const std::string &segment, int64_t speech_end_ms)> cb);
    // 回声门控：true 时丢弃识别 token（TTS 播放期间由 ChatEngine 控制）
    void setSuppressInput(bool suppress);

    // 离线验证（同步，阻塞）：自写 WAV 解析 → 重采样 → 同一套流式/贪心路径，返回识别全文
    std::string runOfflineWav(const std::string &path, float *rtf_out = nullptr);

    // 模型上下文（三个模型各一套；自定义结构，避免与 yolov8-pose.h 的同名类型冲突）
    struct RknnCtx {
        rknn_context ctx = 0;
        rknn_input_output_num io_num{};
        std::vector<rknn_tensor_attr> input_attrs;
        std::vector<rknn_tensor_attr> output_attrs;
        std::vector<rknn_input> inputs;
        std::vector<rknn_output> outputs;
    };

private:
    // ---- 流式循环 ----
    void asrLoop();
    void runOneChunk();                       // encoder(状态回填)→贪心(24×joiner+decoder)
    void flushTail();                         // stop 时 <103 帧补零 + InputFinished + 最后一轮

    // ---- 文本缓冲与关键词 ----
    void onToken(const std::string &token, float prob, int64_t ts_ms);
    void checkKeywords(int64_t now_ms);
    void flushSegment();                      // 当前说话段落写入 txt（停顿 2s 或退出时）
    void clearStreamState();                  // 离线跑完后重置，供在线模式复用同一套模型

    AsrConfig cfg_;
    std::function<bool()> silence_probe_;
    std::vector<std::function<void(const std::string &, float)>> event_cbs_;
    std::function<void(const std::string &, int64_t)> segment_cb_;
    std::atomic<bool> suppress_input_{false};   // 回声门控（播放期间丢弃 token）

    // 三模型
    struct Models {
        RknnCtx encoder, decoder, joiner;
    } models_;

    // fbank（在线流式一份；离线验证时重建一份，用完销毁）
    knf::OnlineFbank *fbank_ = nullptr;

    // 词表（索引=token id）
    std::vector<std::string> vocab_;          // 6257 项

    // 关键词表（去空格后的紧凑形式）
    std::vector<std::string> keywords_;
    std::set<std::string> wake_words_;        // 唤醒词集合（独立冷却判定）

    // ---- 性能调优相位计时（每 chunk 累加，asrLoop 每 50 chunk 打印后清零） ----
    double dbg_enc_ms_ = 0.0;         // encoder 总耗时（含状态回填）
    double dbg_greedy_ms_ = 0.0;      // 贪心循环（24×joiner + token decoder）
    double dbg_refill_ms_ = 0.0;      // 状态回填（4D NCHW→NHWC 转置 + memcpy）

    // ---- 流式状态（仅 ASR 线程访问；离线模式独占） ----
    std::vector<float> encoder_in_;           // [103,80] 特征 chunk
    std::vector<float> encoder_out_;          // [24,512]（贪心时直接读模型输出 buf，此缓冲为容量兜底）
    int processed_frames_ = 0;                // 已消费 fbank 帧数（绝对计数）
    int popped_frames_ = 0;                   // 已从 fbank 回收的帧数（绝对计数）
    bool stream_finished_ = false;            // 尾段 flush 后不再推理

    // 文本滚动缓冲（最近 10s）
    struct TextSeg { std::string token; int64_t ts_ms; float prob; };
    std::vector<TextSeg> text_segs_;
    std::map<std::string, int64_t> last_fire_map_;   // 每关键词独立冷却

    // 说话段落记录（asr_text_log_file 非空时启用；ASR 线程独占）
    FILE *text_fp_ = nullptr;
    std::string cur_segment_;
    int64_t last_token_ms_ = 0;
    int64_t last_audio_ts_ms_ = 0;   // 最新喂入音频块的墙钟时刻（≈话音结束，feedPcm 更新）
    int64_t speech_end_ms_ = 0;       // 本段落话音结束时刻（onToken 时取 last_audio_ts_ms_）
    int64_t last_reset_ms_ = 0;               // 静音期流状态重建的节流时间戳

    // ---- 重采样相位（44.1k→16k，仅 ASR 线程访问） ----
    double resample_pos_ = 0.0;
    int16_t last_sample_ = 0;
    bool has_last_sample_ = false;

    // ---- 队列（采集线程写 / ASR 线程读） ----
    struct PcmChunk { int64_t ts_ms; std::vector<int16_t> mono; };
    std::deque<PcmChunk> queue_;
    std::mutex q_mtx_;
    std::condition_variable q_cv_;
    static const size_t kQueueMax = 2048;     // 2048×50ms≈102s，满则丢最旧

    std::atomic<bool> running_{false};
    std::thread thread_;
};

#endif // ASR_THREAD_H
