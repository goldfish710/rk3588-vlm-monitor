#ifndef MP4_MUXER_H
#define MP4_MUXER_H

// =====================================================
// 基于 minimp4 的 fMP4 写封装（frag 模式）
//
// 特性：
//  - 每帧一个样本（H.265 annex-B → length-prefixed 单样本拼接，
//    规避官方 h26x writer "每 NAL 一样本"导致的多 slice 帧时长膨胀）
//  - moov（含 hvcC/esds/mvex）在首个样本时写入文件头 → 掉电只丢
//    最后一个未完成的 moof 分片，其余可播
//  - duration 制（无 PTS）：事件预录段先 mux 即 t=0，无负 PTS 问题
//
// 仅录像线程单线程使用。
// =====================================================

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

typedef struct MP4E_mux_tag MP4E_mux_t;   // minimp4 前置声明（完整类型在 minimp4.h）

class Mp4Muxer {
public:
    struct Config {
        int width = 2592, height = 1944;        // 视频轨尺寸
        uint32_t video_timescale = 90000;       // 视频 timescale（90kHz 标准）
        bool audio_enable = false;
        uint32_t audio_timescale = 16000;       // AAC 轨 timescale = 编码采样率
        uint32_t audio_frame_samples = 1024;    // 每 AAC 帧样本数（duration 单位）
        std::vector<uint8_t> vps, sps, pps;     // 不含起始码（hvcC 注入）
        std::vector<uint8_t> asc;               // AudioSpecificConfig（音频轨 esds）
    };

    // 打开文件并写 ftyp；参数集/DSI 必须在此处注入（minimp4 要求
    // 在首个 put_sample 之前完成全部轨道配置）
    bool open(const std::string& path, const Config& cfg, std::string* err_out = nullptr);
    bool active() const { return mux_ != nullptr; }

    // 视频帧：annex-B 输入（含起始码），内部转 length-prefixed 单样本
    // pts90：文件相对 PTS（90kHz 单位，单调递增）
    bool writeVideo(const uint8_t* data, size_t len, bool is_idr, int64_t pts90);

    // 音频 AU：duration 固定 = audio_frame_samples
    bool writeAudio(const uint8_t* aac_au, size_t len);

    // 落盘不关（周期 fsync 掉电保护用）
    void flush();

    // 关闭：MP4E_close + fflush + fsync + fclose
    void close();

private:
    static int writeCb(int64_t offset, const void* buffer, size_t size, void* token);

    // annex-B → length-prefixed（4B 大端长度 + NAL 数据）
    static int annexBToSample(const uint8_t* data, size_t len, std::vector<uint8_t>& out);

    MP4E_mux_t* mux_ = nullptr;
    FILE* fp_ = nullptr;
    int vtrack_ = -1;
    int atrack_ = -1;
    int64_t last_pts90_ = -1;   // 上一视频帧 PTS（duration 推算用）
    Config cfg_;
};

#endif
