// tts_backend_piper.cpp
// 本地 Piper TTS 后端（RK3588 NPU decoder，全离线）——包装 paroli 核心库的 C 接口
#include "tts_backend.h"

#include <cstdio>
#include <cstring>
#include <sys/time.h>

#ifdef PIPER_TTS
#include "3rdparty/paroli/tts_piper_c.h"
#endif

// PCM16 mono → 最小 WAV（playWavFile 自动按头内采样率重采样到 44.1k）
bool writePcmWav(const std::string &path, const std::vector<int16_t> &pcm, unsigned sample_rate)
{
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) return false;
    uint32_t data_bytes = (uint32_t)(pcm.size() * 2);
    uint32_t byte_rate = sample_rate * 2;
    auto w16 = [&](uint16_t v) { fputc(v & 0xFF, f); fputc((v >> 8) & 0xFF, f); };
    auto w32 = [&](uint32_t v) {
        fputc(v & 0xFF, f); fputc((v >> 8) & 0xFF, f);
        fputc((v >> 16) & 0xFF, f); fputc((v >> 24) & 0xFF, f);
    };
    fwrite("RIFF", 1, 4, f); w32(36 + data_bytes);
    fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); w32(16); w16(1); w16(1);           // PCM, mono
    w32(sample_rate); w32(byte_rate); w16(2); w16(16);          // block align, bits
    fwrite("data", 1, 4, f); w32(data_bytes);
    fwrite(pcm.data(), 2, pcm.size(), f);
    fclose(f);
    return true;
}

#ifdef PIPER_TTS

// 墙钟毫秒（与 asr_thread/chat_engine 同款实现）
static int64_t now_ms()
{
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

// UTF-8 字符数：一个汉字占 3 字节，直接 text.size() 会虚高
static size_t utf8Chars(const std::string &s)
{
    size_t n = 0;
    for (unsigned char c : s)
        if ((c & 0xC0) != 0x80) n++;
    return n;
}

class PiperTtsBackend : public TtsBackend {
public:
    explicit PiperTtsBackend(piper_tts_t *t) : t_(t) {}
    ~PiperTtsBackend() override { if (t_) piper_tts_destroy(t_); }
    const char *name() const override { return "piper"; }

    bool synth(const std::string &text, std::vector<int16_t> &pcm,
               unsigned &sample_rate, std::string &err) override
    {
        char ebuf[256];
        int16_t *buf = nullptr;
        size_t n = 0;
        unsigned rate = 0;
        int64_t t0 = now_ms();
        if (piper_tts_synth(t_, text.c_str(), &buf, &n, &rate, ebuf, sizeof(ebuf)) != 0) {
            err = ebuf;
            return false;
        }
        int64_t synth_ms = now_ms() - t0;
        pcm.assign(buf, buf + n);
        free(buf);
        sample_rate = rate;

        // RTF 打点（简历数据来源）：单句合成耗时 ÷ 该句生成音频时长
        // 注意：第 1 句含 NPU decoder 预热，评估稳态性能请取第 2 句之后的值
        double audio_sec = rate > 0 ? (double)pcm.size() / (double)rate : 0.0;
        if (audio_sec > 0.0) {
            printf("[TTS] 第%u句 RTF=%.3f (合成 %lldms / 音频 %.2fs / %zu字)\n",
                   ++synth_seq_, (synth_ms / 1000.0) / audio_sec,
                   (long long)synth_ms, audio_sec, utf8Chars(text));
        }
        return true;
    }

private:
    piper_tts_t *t_;
    unsigned synth_seq_ = 0;   // 句子序号（用于排除首句预热值）
};

std::unique_ptr<TtsBackend> createPiperBackend(const ChatConfig &cfg, std::string *err_out)
{
    char ebuf[256];
    piper_tts_t *t = nullptr;
    // decoder 扩展名决定后端（.rknn→NPU；paroli 包装层内部按 USE_RKNN 编译宏分派）
    int use_rknn = (cfg.piper.decoder.size() > 5 &&
                    cfg.piper.decoder.compare(cfg.piper.decoder.size() - 5, 5, ".rknn") == 0) ? 1 : 0;
    if (piper_tts_create(cfg.piper.encoder.c_str(), cfg.piper.decoder.c_str(),
                         cfg.piper.config_json.c_str(), cfg.piper.espeak_data.c_str(),
                         use_rknn, cfg.piper.npu_core,
                         &t, ebuf, sizeof(ebuf)) != 0) {
        if (err_out) *err_out = std::string("Piper 初始化失败: ") + ebuf;
        return nullptr;
    }
    printf("[TTS] Piper 后端就绪 (%s decoder): %s\n",
           use_rknn ? "NPU" : "CPU", cfg.piper.decoder.c_str());
    return std::unique_ptr<TtsBackend>(new PiperTtsBackend(t));
}

#else

std::unique_ptr<TtsBackend> createPiperBackend(const ChatConfig &cfg, std::string *err_out)
{
    (void)cfg;
    if (err_out) *err_out = "Piper TTS 未编译（CMake -DPIPER_TTS=ON）";
    return nullptr;
}

#endif  // PIPER_TTS
