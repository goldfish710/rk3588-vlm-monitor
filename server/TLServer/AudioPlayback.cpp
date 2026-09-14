// AudioPlayback.cpp
// ALSA 播放实现：解析 wav 头 → snd_pcm 配置 → writei 循环（xrun 恢复仿 AudioCapture）
#include "AudioPlayback.h"

#include <cstdio>
#include <cstring>
#include <vector>

#include <alsa/asoundlib.h>

namespace {

// 极简 WAV 头解析（PCM16）：返回采样率/声道，pcm_data 指向 PCM 样本区
bool parseWavHeader(FILE *f, unsigned int &rate, unsigned int &channels, size_t &pcm_off, size_t &pcm_bytes)
{
    char hdr[12];
    if (fread(hdr, 1, 12, f) != 12 || memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0)
        return false;
    bool have_fmt = false, have_data = false;
    uint16_t bits = 0;
    while (!have_data) {
        char ch[8];
        if (fread(ch, 1, 8, f) != 8) break;
        uint32_t sz = (uint32_t)(uint8_t)ch[4] | ((uint32_t)(uint8_t)ch[5] << 8) |
                      ((uint32_t)(uint8_t)ch[6] << 16) | ((uint32_t)(uint8_t)ch[7] << 24);
        if (memcmp(ch, "fmt ", 4) == 0) {
            std::vector<uint8_t> b(sz);
            if (sz > 64 || sz < 16 || fread(b.data(), 1, sz, f) != sz) break;
            uint16_t fmt_tag = (uint16_t)(b[0] | (b[1] << 8));
            channels = (unsigned int)(b[2] | (b[3] << 8));
            rate = (unsigned int)(b[4] | (b[5] << 8) | (b[6] << 16) | (b[7] << 24));
            bits = (uint16_t)(b[14] | (b[15] << 8));
            if (fmt_tag != 1 || bits != 16) break;
            have_fmt = true;
        } else if (memcmp(ch, "data", 4) == 0) {
            pcm_bytes = sz;
            pcm_off = (size_t)ftell(f);
            have_data = true;
        } else {
            fseek(f, sz, SEEK_CUR);
        }
    }
    return have_fmt && have_data;
}

// RK3588 ES8388 采集与播放共享时钟：采集 44.1kHz 时播放必须也是 44.1kHz
const unsigned int kPlayRate = 44100;

// 确保播放流处于运行态：underrun recover 后可能停在 PREPARED，
// 此时 writei 写入不会自动启动（sw_params 状态残留问题）——显式 start 兜底
void ensureStarted(snd_pcm_t *pcm)
{
    snd_pcm_state_t st = snd_pcm_state(pcm);
    if (st == SND_PCM_STATE_PREPARED || st == SND_PCM_STATE_SETUP)
        snd_pcm_start(pcm);
}

// 非阻塞写入：-EAGAIN 时 wait 等待消费；连续 3s 无进展 → drop+prepare+start 强制
// 重启硬件流并继续写剩余（buffer 内已写未播内容会丢，但流恢复）；
// 注意：snd_pcm_wait 在停转状态下可能返回负错误（如 -EPIPE），同样按"无进展"累计
bool writeiNonBlocking(snd_pcm_t *pcm, const int16_t *data, size_t frames)
{
    const int16_t *p = data;
    size_t remaining = frames;
    int stall_ms = 0;
    while (remaining > 0) {
        snd_pcm_sframes_t n = snd_pcm_writei(pcm, p, (snd_pcm_uframes_t)remaining);
        if (n > 0) {
            p += (size_t)n * 2;
            remaining -= (size_t)n;
            stall_ms = 0;
            continue;
        }
        if (n < 0 && n != -EAGAIN) {
            if (snd_pcm_recover(pcm, (int)n, 1) < 0) {
                fprintf(stderr, "[Playback] 写失败: %s\n", snd_strerror((int)n));
                return false;
            }
            continue;
        }
        // -EAGAIN：buffer 满（或等待状态），等 100ms 并累计停顿。
        // 阈值 600ms：正常播放时 -EAGAIN 只是短暂 buffer 满（wait 几十 ms 内恢复）；
        // 600ms 无任何消费 = 硬件停转（实测句间发生），立即 drop+prepare+start 重启
        // （代价几十 ms 人耳无感；原先 3s 阈值导致用户感知卡顿甚至等不及 ^C）
        snd_pcm_wait(pcm, 100);
        stall_ms += 100;
        if (stall_ms >= 600) {
            fprintf(stderr, "[Playback] 硬件停转 0.6s，强制重启流\n");
            snd_pcm_drop(pcm);
            snd_pcm_prepare(pcm);
            ensureStarted(pcm);
            stall_ms = 0;
        }
    }
    return true;
}

} // namespace

int playWavFile(const std::string &wav_path, const std::string &device, int lead_ms, int tail_ms)
{
    FILE *f = fopen(wav_path.c_str(), "rb");
    if (!f) {
        fprintf(stderr, "[Playback] wav 打开失败: %s\n", wav_path.c_str());
        return -1;
    }
    unsigned int rate = 0, channels = 0;
    size_t pcm_off = 0, pcm_bytes = 0;
    if (!parseWavHeader(f, rate, channels, pcm_off, pcm_bytes) || rate == 0 || channels == 0) {
        fprintf(stderr, "[Playback] wav 头解析失败（需 PCM16）: %s\n", wav_path.c_str());
        fclose(f);
        return -1;
    }
    fseek(f, (long)pcm_off, SEEK_SET);

    // 读全部 PCM 到内存（语音回复 3~10s，几十 KB~几百 KB，无压力）
    std::vector<int16_t> mono(pcm_bytes / 2 / (channels ? channels : 1));
    size_t got = fread(mono.data(), 2 * channels, mono.size(), f);
    if (got != mono.size()) {
        fprintf(stderr, "[Playback] wav PCM 读取失败\n");
        fclose(f);
        return -1;
    }
    // 声道折叠（若有 >1ch 来源，取平均）
    if (channels > 1) {
        std::vector<int16_t> m(mono.size() / channels);
        for (size_t i = 0; i < m.size(); i++)
            m[i] = (int16_t)(((int)mono[i * channels] + (int)mono[i * channels + 1]) / 2);
        mono.swap(m);
    }
    fclose(f);

    // TTS 音频任意采样率 → 线性插值上采样到 44.1k（上采样无混叠问题，质量足够）
    std::vector<int16_t> resampled;
    const int16_t *src = mono.data();
    size_t src_n = mono.size();
    if (rate != kPlayRate) {
        resampled.resize((size_t)((double)mono.size() * kPlayRate / rate) + 1);
        double step = (double)rate / kPlayRate;
        for (size_t i = 0; i < resampled.size(); i++) {
            double pos = step * i;
            size_t i0 = (size_t)pos;
            if (i0 + 1 >= src_n) {
                resampled[i] = src_n ? src[src_n - 1] : 0;
                continue;
            }
            double frac = pos - i0;
            resampled[i] = (int16_t)((double)src[i0] * (1.0 - frac) + (double)src[i0 + 1] * frac);
        }
        src = resampled.data();
        src_n = resampled.size();
    }

    snd_pcm_t *pcm = nullptr;
    int rc = snd_pcm_open(&pcm, device.c_str(), SND_PCM_STREAM_PLAYBACK, 0);
    if (rc < 0) {
        fprintf(stderr, "[Playback] snd_pcm_open(%s) 失败: %s\n", device.c_str(), snd_strerror(rc));
        return -1;
    }

    // ES8388 硬件要求最少 2 声道；mono 复制到 L/R
    rc = snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE,
                            SND_PCM_ACCESS_RW_INTERLEAVED,
                            2, kPlayRate, 0, 500000);
    if (rc < 0) {
        fprintf(stderr, "[Playback] snd_pcm_set_params 失败: %s\n", snd_strerror(rc));
        snd_pcm_close(pcm);
        return -1;
    }

    printf("[Playback] 播放 %s: %uHz→%uHz mono→2ch %.2fs\n", wav_path.c_str(), rate, kPlayRate,
           (double)src_n / kPlayRate);

    // 前导静音（ES8388 stream 启动瞬态吞首块音频——实测句首短字被吞，
    // lead-in 让硬件在静音期完成启动） + 尾部静音（默认 400ms:火山 TTS
    // 句尾静音仅 ~200ms,播完立刻安静听感"戛然而止";加静音缓冲让句尾有
    // 呼吸感,同时兜底 drain 截尾。流水线逐句播放时非末句传小值消除句间停顿）
    const size_t lead_pad_frames = kPlayRate * lead_ms / 1000;
    const size_t tail_pad_frames = kPlayRate * tail_ms / 1000;
    std::vector<int16_t> stereo((lead_pad_frames + src_n + tail_pad_frames) * 2, 0);
    for (size_t i = 0; i < src_n; i++)
        stereo[(lead_pad_frames + i) * 2] = stereo[(lead_pad_frames + i) * 2 + 1] = src[i];

    const int16_t *p = stereo.data();
    snd_pcm_uframes_t frames = (snd_pcm_uframes_t)(stereo.size() / 2);   // 含尾部静音缓冲
    while (frames > 0) {
        snd_pcm_sframes_t n = snd_pcm_writei(pcm, p, frames);
        if (n < 0) {
            // xrun 等可恢复错误：prepare 后重试（同 AudioCapture 模式）
            if (snd_pcm_recover(pcm, (int)n, 1) < 0) {
                fprintf(stderr, "[Playback] 写失败: %s\n", snd_strerror((int)n));
                snd_pcm_close(pcm);
                return -1;
            }
            continue;
        }
        p += (size_t)n * 2;
        frames -= (snd_pcm_uframes_t)n;
    }
    snd_pcm_drain(pcm);   // 播完排空
    snd_pcm_close(pcm);
    // 注意：f 已在 PCM 读取段 fclose，此处不可再关（曾致 double free 崩溃）
    return 0;
}

// ==================== 连续播放会话（流水线逐句播放） ====================
// 整轮对话一次 open：ES8388 每次 open→write 都有启动瞬态（吞首块音频），
// "drain+close 后立即重开"的热启动瞬态更长（实测吞后续句句首字）——
// 会话模式只在轮次首句 open 一次，句间仅写静音 gap，彻底消除重启吞音。
PlaybackSession::PlaybackSession() {}
PlaybackSession::~PlaybackSession() { finish(); }

bool PlaybackSession::open(const std::string &device, int lead_ms)
{
    snd_pcm_t *pcm = nullptr;
    int rc = snd_pcm_open(&pcm, device.c_str(), SND_PCM_STREAM_PLAYBACK, 0);
    if (rc < 0) {
        fprintf(stderr, "[Playback] snd_pcm_open(%s) 失败: %s\n", device.c_str(), snd_strerror(rc));
        return false;
    }
    rc = snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE,
                            SND_PCM_ACCESS_RW_INTERLEAVED,
                            2, kPlayRate, 0, 500000);
    if (rc < 0) {
        fprintf(stderr, "[Playback] snd_pcm_set_params 失败: %s\n", snd_strerror(rc));
        snd_pcm_close(pcm);
        return false;
    }
    // start_threshold=1：写 1 帧即启动播放。set_params 默认 start_threshold≈buffer
    // 大小（500ms）——末句后的 400ms 尾静音不足阈值导致流不启动、writei 永久阻塞
    // （实测"最后一句不出声，Ctrl+C 后 drain 才播"的根因）
    {
        snd_pcm_sw_params_t *sw = nullptr;
        snd_pcm_sw_params_malloc(&sw);
        snd_pcm_sw_params_current(pcm, sw);
        snd_pcm_sw_params_set_start_threshold(pcm, sw, 1);
        rc = snd_pcm_sw_params(pcm, sw);
        snd_pcm_sw_params_free(sw);
        if (rc < 0) {
            fprintf(stderr, "[Playback] sw_params 设置失败: %s\n", snd_strerror(rc));
            snd_pcm_close(pcm);
            return false;
        }
    }
    pcm_ = pcm;
    open_ = true;
    // 非阻塞模式：writei 在 buffer 满时立即返回 -EAGAIN（阻塞模式会永久卡死——
    // ES8388 连续会话中 I2S DMA 可能停转，阻塞 writei 无法恢复）
    snd_pcm_nonblock(pcm, 1);
    // 前导静音覆盖启动瞬态
    std::vector<int16_t> silence((size_t)kPlayRate * lead_ms / 1000 * 2, 0);
    const int16_t *p = silence.data();
    ensureStarted(pcm);
    snd_pcm_uframes_t frames = (snd_pcm_uframes_t)(silence.size() / 2);
    while (frames > 0) {
        snd_pcm_sframes_t n = snd_pcm_writei(pcm, p, frames);
        if (n < 0) {
            if (snd_pcm_recover(pcm, (int)n, 1) < 0) {
                fprintf(stderr, "[Playback] lead 写失败: %s\n", snd_strerror((int)n));
                snd_pcm_close(pcm);
                pcm_ = nullptr;
                open_ = false;
                return false;
            }
            continue;
        }
        p += (size_t)n * 2;
        frames -= (snd_pcm_uframes_t)n;
    }
    return true;
}

bool PlaybackSession::playPcm16(const int16_t *mono, size_t n, unsigned src_rate)
{
    if (!open_) return false;
    // 重采样到 44.1k（线性插值，与 playWavFile 同款）
    std::vector<int16_t> resampled;
    const int16_t *src = mono;
    size_t src_n = n;
    if (src_rate != kPlayRate && src_n > 0) {
        resampled.resize((size_t)((double)n * kPlayRate / src_rate) + 1);
        double step = (double)src_rate / kPlayRate;
        for (size_t i = 0; i < resampled.size(); i++) {
            double pos = step * i;
            size_t i0 = (size_t)pos;
            if (i0 + 1 >= src_n) { resampled[i] = src[src_n - 1]; continue; }
            double frac = pos - i0;
            resampled[i] = (int16_t)((double)src[i0] * (1.0 - frac) + (double)src[i0 + 1] * frac);
        }
        src = resampled.data();
        src_n = resampled.size();
    }
    // mono → stereo 复制
    std::vector<int16_t> stereo(src_n * 2);
    for (size_t i = 0; i < src_n; i++)
        stereo[i * 2] = stereo[i * 2 + 1] = src[i];

    snd_pcm_t *pcm = (snd_pcm_t *)pcm_;
    ensureStarted(pcm);
    return writeiNonBlocking(pcm, stereo.data(), src_n);
}

void PlaybackSession::gap(int ms)
{
    if (!open_) return;
    std::vector<int16_t> silence((size_t)kPlayRate * ms / 1000 * 2, 0);
    const int16_t *p = silence.data();
    snd_pcm_t *pcm = (snd_pcm_t *)pcm_;
    ensureStarted(pcm);
    writeiNonBlocking(pcm, silence.data(), silence.size() / 2);
}

void PlaybackSession::finish()
{
    if (open_) {
        snd_pcm_t *pcm = (snd_pcm_t *)pcm_;
        // 显式 start 兜底：underrun recover 后流可能停在 PREPARED，
        // buffer 内残余内容需要 start 才会播出
        snd_pcm_state_t st = snd_pcm_state(pcm);
        if (st == SND_PCM_STATE_PREPARED)
            snd_pcm_start(pcm);
        // 非阻塞句柄的 drain 会立即返回 -EAGAIN 不排空——循环等排空（上限 10s）
        int rc;
        int retries = 0;
        while ((rc = snd_pcm_drain(pcm)) == -EAGAIN && retries++ < 100)
            snd_pcm_wait(pcm, 100);
        snd_pcm_close(pcm);
        pcm_ = nullptr;
        open_ = false;
    }
}
