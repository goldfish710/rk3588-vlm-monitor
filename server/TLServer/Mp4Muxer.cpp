#include "Mp4Muxer.h"

#include "minimp4.h"

#include <cstring>
#include <iostream>
#include <unistd.h>

// =====================================================
// 写回调（minimp4 语义：返回 0 = 成功，非 0 = 错误）
// =====================================================
int Mp4Muxer::writeCb(int64_t /*offset*/, const void* buffer, size_t size, void* token)
{
    FILE* fp = (FILE*)token;
    if (!fp) return -1;
    if (fwrite(buffer, 1, size, fp) != size) return -1;
    return 0;
}

// =====================================================
// annex-B → length-prefixed（每 NAL 前加 4 字节大端长度）
// =====================================================
int Mp4Muxer::annexBToSample(const uint8_t* data, size_t len, std::vector<uint8_t>& out)
{
    out.clear();
    size_t i = 0;
    int nal_count = 0;

    while (i + 3 <= len) {
        int sc_len = 0;
        if (i + 4 <= len && data[i] == 0x00 && data[i + 1] == 0x00 &&
            data[i + 2] == 0x00 && data[i + 3] == 0x01) {
            sc_len = 4;
        } else if (data[i] == 0x00 && data[i + 1] == 0x00 && data[i + 2] == 0x01) {
            sc_len = 3;
        }

        if (sc_len == 0) {   // 非起始码开头，跳过字节继续找
            i++;
            continue;
        }

        size_t hdr = i + sc_len;
        // 找下一个起始码作为本 NAL 结尾
        size_t next = len;
        for (size_t j = hdr; j + 3 <= len; j++) {
            if (j + 4 <= len && data[j] == 0x00 && data[j + 1] == 0x00 &&
                data[j + 2] == 0x00 && data[j + 3] == 0x01) {
                next = j;
                break;
            }
            if (data[j] == 0x00 && data[j + 1] == 0x00 && data[j + 2] == 0x01) {
                next = j;
                break;
            }
        }

        size_t nal_len = next - hdr;
        uint32_t n = (uint32_t)nal_len;
        out.push_back((uint8_t)(n >> 24));
        out.push_back((uint8_t)(n >> 16));
        out.push_back((uint8_t)(n >> 8));
        out.push_back((uint8_t)n);
        out.insert(out.end(), data + hdr, data + next);
        nal_count++;
        i = next;
    }
    return nal_count;
}

// =====================================================
// 打开：ftyp 立即落盘；轨道参数集/DSI 在首个样本前注入
// =====================================================
bool Mp4Muxer::open(const std::string& path, const Config& cfg, std::string* err_out)
{
    cfg_ = cfg;
    fp_ = fopen(path.c_str(), "wb");
    if (!fp_) {
        if (err_out) *err_out = "fopen 失败: " + path;
        return false;
    }
    setvbuf(fp_, NULL, _IOFBF, 4 * 1024 * 1024);

    mux_ = MP4E_open(1, 1, fp_, &Mp4Muxer::writeCb);   // sequential + fragmentation
    if (!mux_) {
        if (err_out) *err_out = "MP4E_open 失败";
        fclose(fp_);
        fp_ = nullptr;
        return false;
    }

    // ---------- 视频轨（hvc1） ----------
    MP4E_track_t tv;
    memset(&tv, 0, sizeof(tv));
    tv.track_media_kind = e_video;
    tv.object_type_indication = MP4_OBJECT_TYPE_HEVC;
    tv.language[0] = 'u'; tv.language[1] = 'n'; tv.language[2] = 'd';
    tv.time_scale = cfg.video_timescale;               // 90000
    tv.default_duration = 3000;                        // 90000/30fps
    tv.u.v.width = cfg.width;
    tv.u.v.height = cfg.height;
    vtrack_ = MP4E_add_track(mux_, &tv);
    if (vtrack_ < 0) {
        if (err_out) *err_out = "添加视频轨失败";
        close();
        return false;
    }
    if (!cfg.vps.empty()) MP4E_set_vps(mux_, vtrack_, cfg.vps.data(), (int)cfg.vps.size());
    if (!cfg.sps.empty()) MP4E_set_sps(mux_, vtrack_, cfg.sps.data(), (int)cfg.sps.size());
    if (!cfg.pps.empty()) MP4E_set_pps(mux_, vtrack_, cfg.pps.data(), (int)cfg.pps.size());

    // ---------- 音频轨（mp4a） ----------
    if (cfg.audio_enable) {
        MP4E_track_t ta;
        memset(&ta, 0, sizeof(ta));
        ta.track_media_kind = e_audio;
        ta.object_type_indication = MP4_OBJECT_TYPE_AUDIO_ISO_IEC_14496_3;
        ta.language[0] = 'u'; ta.language[1] = 'n'; ta.language[2] = 'd';
        ta.time_scale = cfg.audio_timescale;           // 16000
        ta.default_duration = cfg.audio_frame_samples; // 1024
        ta.u.a.channelcount = 1;
        atrack_ = MP4E_add_track(mux_, &ta);
        if (atrack_ < 0) {
            std::cerr << "[Mp4Muxer] 添加音频轨失败，继续纯视频" << std::endl;
        } else if (!cfg.asc.empty()) {
            MP4E_set_dsi(mux_, atrack_, cfg.asc.data(), (int)cfg.asc.size());
        }
    }

    last_pts90_ = -1;
    std::cout << "[Mp4Muxer] 打开 " << path
              << " (" << cfg.width << "x" << cfg.height << ")"
              << (atrack_ >= 0 ? " +AAC" : "") << std::endl;
    return true;
}

// =====================================================
// 视频帧写入（duration = 相邻 PTS 差，首帧 3000）
// =====================================================
bool Mp4Muxer::writeVideo(const uint8_t* data, size_t len, bool is_idr, int64_t pts90)
{
    if (!mux_ || vtrack_ < 0) return false;

    std::vector<uint8_t> sample;
    if (annexBToSample(data, len, sample) <= 0) return false;

    int64_t duration = (last_pts90_ >= 0) ? (pts90 - last_pts90_) : 3000;
    if (duration <= 0) duration = 3000;
    last_pts90_ = pts90;

    int rc = MP4E_put_sample(mux_, vtrack_, sample.data(), (int)sample.size(),
                             (int)duration,
                             is_idr ? MP4E_SAMPLE_RANDOM_ACCESS : MP4E_SAMPLE_DEFAULT);
    if (rc != MP4E_STATUS_OK) {
        std::cerr << "[Mp4Muxer] 写视频样本失败 rc=" << rc << std::endl;
        return false;
    }
    return true;
}

// =====================================================
// 音频帧写入（duration 固定 1024，每帧独立可解）
// =====================================================
bool Mp4Muxer::writeAudio(const uint8_t* aac_au, size_t len)
{
    if (!mux_ || atrack_ < 0 || len == 0) return false;

    int rc = MP4E_put_sample(mux_, atrack_, aac_au, (int)len,
                             (int)cfg_.audio_frame_samples,
                             MP4E_SAMPLE_RANDOM_ACCESS);
    if (rc != MP4E_STATUS_OK) {
        std::cerr << "[Mp4Muxer] 写音频样本失败 rc=" << rc << std::endl;
        return false;
    }
    return true;
}

// =====================================================
// 落盘（周期 fsync 掉电保护，不关 mux）
// =====================================================
void Mp4Muxer::flush()
{
    if (!fp_) return;
    fflush(fp_);
    fsync(fileno(fp_));
}

// =====================================================
// 关闭（frag 模式下 MP4E_close 无待写索引，落盘由本层做）
// =====================================================
void Mp4Muxer::close()
{
    if (mux_) {
        MP4E_close(mux_);
        mux_ = nullptr;
    }
    if (fp_) {
        fflush(fp_);
        fsync(fileno(fp_));
        fclose(fp_);
        fp_ = nullptr;
    }
    vtrack_ = atrack_ = -1;
    last_pts90_ = -1;
}
