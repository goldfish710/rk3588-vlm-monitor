#include "AacEncoder.h"

#include <cmath>
#include <cstdio>
#include <cstring>

// =====================================================
// 初始化：aacEncOpen → SetParam → 取 confBuf 作 ASC
// =====================================================
bool AacEncoder::init(const Config& cfg, std::string* err_out) {
    cfg_ = cfg;

    if (aacEncOpen(&h_, 0, (CHANNEL_MODE)2) != AACENC_OK) {   // maxChannels=2
        if (err_out) *err_out = "aacEncOpen 失败";
        h_ = nullptr;
        return false;
    }

    // TRANSMUX=0：输出裸 AAC AU（无 ADTS 头），MP4/RTSP 都用这个格式
    aacEncoder_SetParam(h_, AACENC_TRANSMUX, 0);
    aacEncoder_SetParam(h_, AACENC_AOT, 2);                      // AAC-LC
    aacEncoder_SetParam(h_, AACENC_SAMPLERATE, cfg_.sample_rate); // 16000
    aacEncoder_SetParam(h_, AACENC_CHANNELMODE, MODE_1);          // mono（输入需已降混）
    aacEncoder_SetParam(h_, AACENC_BITRATE, cfg_.bitrate_bps);
    aacEncoder_SetParam(h_, AACENC_AFTERBURNER, 1);
    aacEncoder_SetParam(h_, AACENC_CHANNELORDER, 1);              // MPEG 声道序

    if (aacEncEncode(h_, NULL, NULL, NULL, NULL) != AACENC_OK) {
        if (err_out) *err_out = "aacEncEncode(NULL) 初始化失败";
        aacEncClose(&h_);
        h_ = nullptr;
        return false;
    }

    AACENC_InfoStruct info;
    memset(&info, 0, sizeof(info));
    if (aacEncInfo(h_, &info) != AACENC_OK || info.confSize <= 0) {
        if (err_out) *err_out = "aacEncInfo 失败";
        aacEncClose(&h_);
        h_ = nullptr;
        return false;
    }

    aac_asc_.assign(info.confBuf, info.confBuf + info.confSize);
    printf("[AacEncoder] 初始化成功: %dHz mono %dbps, ASC=%s, 帧长=%d样本\n",
           cfg_.sample_rate, cfg_.bitrate_bps, ascHex().c_str(), 1024);
    return true;
}

void AacEncoder::shutdown() {
    if (h_) {
        aacEncClose(&h_);
        h_ = nullptr;
    }
    aac_asc_.clear();
    mono_buf_.clear();
}

std::string AacEncoder::ascHex() const {
    static const char hex[] = "0123456789ABCDEF";
    std::string s;
    s.reserve(aac_asc_.size() * 2);
    for (uint8_t b : aac_asc_) {
        s.push_back(hex[b >> 4]);
        s.push_back(hex[b & 0x0F]);
    }
    return s;
}

// =====================================================
// 采集线程内联：降混 → 累加 → 编码 → 广播
// =====================================================
void AacEncoder::feedPcm(const AudioChunk& chunk) {
    if (!h_ || chunk.data.empty()) return;

    downmixToMono(chunk);
    encodeAvailable();
}

void AacEncoder::downmixToMono(const AudioChunk& c) {
    const int16_t* p = reinterpret_cast<const int16_t*>(c.data.data());
    size_t samples = c.data.size() / 2;            // 2ch 交错样本对数

    if (mono_buf_.empty()) buf_start_ms_ = c.ts_ms;

    mono_buf_.reserve(mono_buf_.size() + samples / 2);
    for (size_t i = 0; i + 1 < samples; i += 2) {
        int16_t m;
        switch (cfg_.mono_source) {
            case 1:  m = p[i];                        break;   // 仅左声道
            case 2:  m = p[i + 1];                    break;   // 仅右声道
            default: m = (int16_t)(((int)p[i] + (int)p[i + 1]) / 2);   // 降混
        }
        mono_buf_.push_back(m);
    }
}


void AacEncoder::encodeAvailable() {
    while (mono_buf_.size() >= 1024) {
        uint8_t out[2048];
        AACENC_BufDesc in_buf, out_buf;
        AACENC_InArgs in_args;
        AACENC_OutArgs out_args;
        void *in_ptr[1], *out_ptr[1];
        int in_id[1], out_id[1], in_size[1], in_el[1], out_size[1], out_el[1];

        in_ptr[0] = mono_buf_.data();
        in_size[0] = 2 * 1024;
        in_el[0] = 2;
        in_id[0] = IN_AUDIO_DATA;
        out_ptr[0] = out;
        out_size[0] = (int)sizeof(out);
        out_el[0] = 1;
        out_id[0] = OUT_BITSTREAM_DATA;

        in_buf.numBufs = out_buf.numBufs = 1;
        in_buf.bufs = in_ptr;  in_buf.bufferIdentifiers = in_id;
        in_buf.bufSizes = in_size;  in_buf.bufElSizes = in_el;
        out_buf.bufs = out_ptr;  out_buf.bufferIdentifiers = out_id;
        out_buf.bufSizes = out_size;  out_buf.bufElSizes = out_el;

        memset(&in_args, 0, sizeof(in_args));
        in_args.numInSamples = 1024;
        memset(&out_args, 0, sizeof(out_args));

        int64_t frame_ts = buf_start_ms_;
        int64_t frame_dur_ms = (int64_t)1024 * 1000 / cfg_.sample_rate;   // 64ms

        if (aacEncEncode(h_, &in_buf, &out_buf, &in_args, &out_args) != AACENC_OK) {
            printf("[AacEncoder] 编码失败，丢弃本帧\n");
        } else if (out_args.numOutBytes > 0) {
            AacFrame f;
            f.ts_ms = frame_ts;
            f.data.assign(out, out + out_args.numOutBytes);
            for (auto& s : aac_sinks_) s(f);
        }

        buf_start_ms_ += frame_dur_ms;
        mono_buf_.erase(mono_buf_.begin(), mono_buf_.begin() + 1024);
    }
}
