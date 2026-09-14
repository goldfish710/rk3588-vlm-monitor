#ifndef AAC_ENCODER_H
#define AAC_ENCODER_H

// =====================================================
// fdk-aac 编码器封装（AAC-LC，16kHz 单声道）
//
// 输入：PCM 块（16kHz/2ch/S16_LE 交错，与 AudioCapture 输出一致）
// 输出：裸 AAC AU 帧（无 ADTS 头）→ 广播给 sinks（录像 + RTSP）
//
// 编码在音频采集线程内联调用（16kHz mono 单帧约 0.2ms，可忽略），
// 全系统共享一个实例（编码一次，多处消费）。
//
// 关键数字：AAC-LC 每帧固定 1024 样本，16kHz 下 = 64ms/帧，
// 与 50ms 采集块不对齐 → 内部降混累加缓冲，攒满一帧才编码。
// =====================================================

#include <fdk-aac/aacenc_lib.h>

#include <functional>
#include <string>
#include <vector>
#include <cstdint>

#include "AudioCapture.h"   // AudioChunk

// 一帧编码后的 AAC AU
struct AacFrame {
    int64_t ts_ms = 0;              // 帧内首样本墙钟毫秒（与视频时间戳同源）
    std::vector<uint8_t> data;      // 裸 AU（16k mono 24kbps ≈ 192B/帧）
};

class AacEncoder {
public:
    struct Config {
        int sample_rate = 16000;    // 与采集一致
        int channels = 2;           // 输入 PCM 声道数（内部降混为 mono）
        int bitrate_bps = 24000;    // AAC-LC 目标码率
        int mono_source = 0;        // 0=(L+R)/2 降混，1=仅左声道，2=仅右声道
    };

    // 打开编码器；失败返回 false（err_out 带原因），调用方降级
    bool init(const Config& cfg, std::string* err_out = nullptr);
    void shutdown();

    bool ready() const { return h_ != nullptr; }

    // 采集线程内联调用：降混 → 累加 → 攒满 1024 样本编码 → 广播
    void feedPcm(const AudioChunk& chunk);

    // 编码输出消费者（init 前注册；采集线程调用，注册后不再变更）
    void addAacSink(std::function<void(const AacFrame&)> sink) {
        aac_sinks_.push_back(std::move(sink));
    }

    // ---- 轨道元信息（MP4 音频轨 / RTSP SDP 用） ----
    const std::vector<uint8_t>& asc() const { return aac_asc_; }   // AudioSpecificConfig
    std::string ascHex() const;                                     // 十六进制串，如 "1388"
    int outSampleRate() const { return cfg_.sample_rate; }
    int outChannels() const { return 1; }                           // 降混后 mono
    int frameSamples() const { return 1024; }                       // AAC-LC 固定

private:
    void downmixToMono(const AudioChunk& c);
    void encodeAvailable();

    HANDLE_AACENCODER h_ = nullptr;
    Config cfg_;
    std::vector<int16_t> mono_buf_;      // 降混累加缓冲
    int64_t buf_start_ms_ = 0;           // mono_buf_ 首样本时刻
    std::vector<uint8_t> aac_asc_;       // confBuf（ASC）
    std::vector<std::function<void(const AacFrame&)>> aac_sinks_;
};

#endif
