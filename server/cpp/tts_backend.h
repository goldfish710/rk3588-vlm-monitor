// tts_backend.h
// TTS 后端抽象：cloud（火山引擎 HTTP）| piper（本地 RK3588 NPU，全离线）
// 可插拔设计：ChatEngine 只依赖 TtsBackend 接口，config [chat] tts_provider 切换。
//
// 线程约束：synth 是阻塞调用，仅在 ChatEngine 的 chat_llm 线程（cloud 整段路径）
// 或 tts_worker 线程（流水线逐句路径）内调用——单线程串行使用，实现无需加锁。
#ifndef TTS_BACKEND_H
#define TTS_BACKEND_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "config.h"   // ChatConfig

class TtsBackend {
public:
    virtual ~TtsBackend() = default;

    // 阻塞合成整段/整句文本 → int16 mono PCM（采样率由后端决定：
    // cloud=16000，piper=config.json 决定，通常 22050）
    virtual bool synth(const std::string &text,
                       std::vector<int16_t> &pcm, unsigned &sample_rate,
                       std::string &err) = 0;

    virtual const char *name() const = 0;
};

// 工厂：按 ChatConfig 选择后端。
// tts_provider=piper 但初始化失败时：cloud 凭据齐全则降级 cloud（err 说明），
// 否则返回 nullptr（调用方走无 TTS 兜底）。
std::unique_ptr<TtsBackend> createTtsBackend(const ChatConfig &cfg, std::string *err_out);

// PCM16 mono → 最小 WAV 文件（供 playWavFile 复用；AudioPlayback 自动按头内 rate 重采样）
bool writePcmWav(const std::string &path, const std::vector<int16_t> &pcm, unsigned sample_rate);

#endif // TTS_BACKEND_H
