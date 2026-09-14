// AudioPlayback.h
// ALSA 播放模块（TTS 语音回复用）
// 设备默认 "default"（经 dmix 自动转率，任意 wav 采样率可播；
// 不直接用 hw:0,0 避免与采集抢占且需严格匹配硬件参数）
#ifndef AUDIO_PLAYBACK_H
#define AUDIO_PLAYBACK_H

#include <cstdint>
#include <string>

// 播放 WAV 文件（PCM16），阻塞至播完。解析 wav 头按实际采样率配置 ALSA，
// mono 源自动复制为双声道（ES8388 硬件要求最少 2ch）。
// 设备默认 hw:0,0 直连硬件——default(dmix) 与 hw:0,0 采集并发时会阻塞
// （实测:采集运行期间 dmix 播放 writei 永久卡住,退出时采集停止才恢复）。
// 返回 0=成功，负值=失败（stderr 有原因）
// lead_ms: 播放前导静音（ES8388 stream 启动瞬态会吞首块音频，实测句首
// 短字"哦"被吞——前导静音让硬件在静音期完成启动，语音内容完整播出）
// tail_ms: 播放尾部追加静音时长（整段播放默认 400ms 呼吸感；
// 流水线逐句播放时非末句传小值消除句间停顿）
int playWavFile(const std::string &wav_path, const std::string &device = "hw:0,0",
                int lead_ms = 60, int tail_ms = 400);

// 连续播放会话：整轮对话一次 open，逐句写入（消除每句 open/close 的
// 启动瞬态吞音——实测"热重启"（drain+close 后立即 open）会吞后续句句首字）
class PlaybackSession {
public:
    PlaybackSession();
    ~PlaybackSession();

    // 打开设备并配置（固定 44.1kHz/2ch；lead_ms 前导静音覆盖启动瞬态）
    bool open(const std::string &device, int lead_ms = 60);

    // 写入一段 mono PCM（任意采样率，内部重采样到 44.1k 并复制双声道），阻塞至写入完成
    bool playPcm16(const int16_t *mono, size_t n, unsigned src_rate);

    // 句间静音（毫秒）
    void gap(int ms);

    // 播完收尾：drain + close
    void finish();

private:
    void *pcm_ = nullptr;   // snd_pcm_t*
    bool open_ = false;
};

#endif // AUDIO_PLAYBACK_H
