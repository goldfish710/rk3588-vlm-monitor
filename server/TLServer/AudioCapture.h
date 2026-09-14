#ifndef AUDIO_CAPTURE_H
#define AUDIO_CAPTURE_H

// =====================================================
// ALSA 音频采集模块（板载 ES8388 codec）
//
// 职责：打开采集设备 → 独立采集线程 poll+readi 循环 →
//      写入预录环形缓冲（事件提取用）+ 回调消费者（VideoRecorder 入队）。
// 不做任何文件 I/O。
//
// 降级设计：打开失败/运行期连续读失败 → 模块禁用，
// 由 VideoRecorder 决定继续纯视频录像，不影响视频管线。
// =====================================================

#include <alsa/asoundlib.h>
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// 一块音频数据（一次 period 读取）
struct AudioChunk {
    int64_t ts_ms = 0;              // 块起始时刻（墙钟毫秒）
    std::vector<uint8_t> data;      // S16_LE 交错 PCM
};

class AudioCapture {
public:
    struct Config {
        std::string device = "hw:0,0";   // ALSA 采集设备名（打开失败回退 "default"）
        int rate = 16000;
        int channels = 2;                // ES8388 最小 2 声道
        int pre_buffer_sec = 5;          // 事件预录时长（决定环形缓冲容量）
        bool silence_warn = true;        // 检测到直流/静音时告警
        bool debug_dump = false;         // 调试：把原始采集 PCM 直接写 ./audio_raw.pcm
    };

    // 打开设备 + 启动采集线程；失败返回 false（err_out 带原因），调用方降级
    bool init(const Config& cfg, std::string* err_out = nullptr);

    // 停止采集线程（有界：poll 100ms 超时退出，绝不卡死在 readi）
    void shutdown();

    bool enabled() const { return running_.load(); }

    // 注册音频块消费者（多路分发：AAC 编码、KWS 等），须在 init 前设置
    void addSink(std::function<void(AudioChunk&&)> sink) {
        sinks_.push_back(std::move(sink));
    }

    // 最近统计窗口内是否为静音/直流（用于"麦克风可能未接线"告警）
    bool silent() const { return silent_.load(); }

    // 暂停/恢复采集（TTS 播放期间用）：
    // 播放与 hw:0,0 采集并发会 xrun 并损坏采集流（-EIO 循环），
    // 且播放期间录到的只是喇叭回声——暂停采集双赢。录像音轨在暂停期间静音。
    void pauseCapture();
    void resumeCapture();

    int rate() const { return rate_; }
    int channels() const { return channels_; }

private:
    void captureLoop();
    bool openDevice(std::string* err_out);
    void updateSilenceStats(const std::vector<uint8_t>& data);

    Config cfg_;
    snd_pcm_t* pcm_ = nullptr;
    std::thread thread_;
    std::atomic<bool> paused_{false};     // TTS 播放期间暂停采集
    std::atomic<bool> need_restart_{false};  // 恢复采集时请求完整重启(仅采集线程执行)
    std::atomic<bool> running_{false};
    std::atomic<bool> silent_{true};

    std::vector<std::function<void(AudioChunk&&)>> sinks_;

    // 静音统计（仅采集线程访问）
    int64_t last_silence_check_ms_ = 0;
    int64_t last_silence_warn_ms_ = 0;
    double acc_sum_ = 0;
    double acc_sumsq_ = 0;
    int64_t acc_samples_ = 0;

    int rate_ = 16000;
    int channels_ = 2;

    FILE* dump_fp_ = nullptr;            // 调试 dump（仅采集线程访问）
};

#endif
