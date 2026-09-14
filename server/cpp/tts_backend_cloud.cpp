// tts_backend_cloud.cpp
// 云端 TTS 后端：火山引擎 HTTP 非流式（原 chat_engine.cpp doTts 逻辑迁入，行为一致）
#include "tts_backend.h"

#include <cstdio>
#include <cstring>

#include <unistd.h>

#include "cJSON.h"
#include "https_client.h"

namespace {

// 自写 base64 解码（火山 TTS 音频字段；cJSON 只给字符串）
// 与原 chat_engine.cpp 实现一致，随 doTts 迁入
std::vector<uint8_t> base64Decode(const std::string &in)
{
    static const int8_t rev[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
        52,53,54,55,56,57,58,59,60,61,-1,-1,-1, 0,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    };
    std::vector<uint8_t> out;
    out.reserve(in.size() * 3 / 4);
    int val = 0, bits = -8;
    for (unsigned char c : in) {
        int v = (c < 256) ? rev[c] : -1;
        if (v < 0) continue;
        val = (val << 6) | v;
        bits += 6;
        if (bits >= 0) {
            out.push_back((uint8_t)((val >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return out;
}

// 极简 WAV 解析（PCM16 mono，火山返回固定格式）：跳过 RIFF 头，定位 data 块
// 返回 false 且置 pcm_off/pcm_bytes 不变表示解析失败
bool parseWavPcm(const std::vector<uint8_t> &wav, size_t &pcm_off, size_t &pcm_bytes)
{
    if (wav.size() < 44 || memcmp(wav.data(), "RIFF", 4) != 0 ||
        memcmp(wav.data() + 8, "WAVE", 4) != 0)
        return false;
    size_t pos = 12;
    while (pos + 8 <= wav.size()) {
        uint32_t sz = (uint32_t)wav[pos + 4] | ((uint32_t)wav[pos + 5] << 8) |
                      ((uint32_t)wav[pos + 6] << 16) | ((uint32_t)wav[pos + 7] << 24);
        if (memcmp(wav.data() + pos, "data", 4) == 0) {
            pcm_off = pos + 8;
            pcm_bytes = sz;
            if (pcm_off + pcm_bytes > wav.size())
                pcm_bytes = wav.size() - pcm_off;
            return true;
        }
        pos += 8 + sz;
        if (sz & 1) pos++;   // 奇数块补齐
    }
    return false;
}

} // namespace

class CloudTtsBackend : public TtsBackend {
public:
    explicit CloudTtsBackend(const ChatConfig &cfg) : cfg_(cfg) {}
    const char *name() const override { return "cloud"; }

    bool synth(const std::string &text, std::vector<int16_t> &pcm,
               unsigned &sample_rate, std::string &err) override
    {
        for (int attempt = 0; attempt < 2; attempt++) {
            err.clear();
            char reqid[64];
            snprintf(reqid, sizeof(reqid), "rk3588_%lld_%u",
                     (long long)now_ms(), ++tts_seq_);

            cJSON *root = cJSON_CreateObject();
            cJSON *app = cJSON_AddObjectToObject(root, "app");
            cJSON_AddStringToObject(app, "appid", cfg_.tts_app_id.c_str());
            cJSON_AddStringToObject(app, "token", cfg_.tts_access_token.c_str());
            cJSON_AddStringToObject(app, "cluster", cfg_.tts_cluster.c_str());
            cJSON *user = cJSON_AddObjectToObject(root, "user");
            cJSON_AddStringToObject(user, "uid", "rk3588_home_001");
            cJSON *audio = cJSON_AddObjectToObject(root, "audio");
            cJSON_AddStringToObject(audio, "voice_type", cfg_.tts_voice_type.c_str());
            cJSON_AddStringToObject(audio, "encoding", "wav");
            cJSON_AddNumberToObject(audio, "rate", 16000);
            cJSON_AddNumberToObject(audio, "speed_ratio", cfg_.tts_speed_ratio);
            cJSON_AddNumberToObject(audio, "volume_ratio", 1.0);
            cJSON_AddNumberToObject(audio, "pitch_ratio", 1.0);
            cJSON *request = cJSON_AddObjectToObject(root, "request");
            cJSON_AddStringToObject(request, "reqid", reqid);
            cJSON_AddStringToObject(request, "text", text.c_str());
            cJSON_AddStringToObject(request, "text_type", "plain");
            cJSON_AddStringToObject(request, "operation", "query");
            cJSON_AddNumberToObject(request, "with_frontend", 1);
            cJSON_AddStringToObject(request, "frontend_type", "unitTson");
            char *json = cJSON_PrintUnformatted(root);
            std::string body(json);
            cJSON_free(json);
            cJSON_Delete(root);

            // 注意：火山鉴权头是 "Bearer;<token>"（分号，不是空格/冒号）
            std::vector<std::pair<std::string, std::string>> headers = {
                {"Authorization", "Bearer;" + cfg_.tts_access_token},
                {"Content-Type", "application/json"},
            };
            HttpResponse resp = httpsPost(cfg_.tts_api_host, "443", "/api/v1/tts",
                                          headers, body, 10, 15, 60);
            if (resp.status != 200) {
                err = "HTTP " + std::to_string(resp.status) + " " + resp.error;
                if (attempt == 0) { usleep(500000); continue; }
                return false;
            }
            cJSON *r = cJSON_ParseWithLength((const char *)resp.body.data(), resp.body.size());
            if (!r) {
                err = "TTS 响应 JSON 解析失败";
                if (attempt == 0) { usleep(500000); continue; }
                return false;
            }
            cJSON *code = cJSON_GetObjectItem(r, "code");
            cJSON *msg = cJSON_GetObjectItem(r, "message");
            if (!cJSON_IsNumber(code) || code->valueint != 3000) {
                err = "TTS code!=3000: " +
                      (cJSON_IsString(msg) ? std::string(msg->valuestring) : "未知错误");
                cJSON_Delete(r);
                if (attempt == 0) { usleep(500000); continue; }
                return false;
            }
            cJSON *data = cJSON_GetObjectItem(r, "data");
            if (!cJSON_IsString(data)) {
                err = "TTS data 字段缺失";
                cJSON_Delete(r);
                if (attempt == 0) { usleep(500000); continue; }
                return false;
            }
            std::vector<uint8_t> wav = base64Decode(data->valuestring);
            cJSON_Delete(r);

            // 剥 wav 头取 PCM（火山固定 16k mono PCM16）
            size_t pcm_off = 0, pcm_bytes = 0;
            if (!parseWavPcm(wav, pcm_off, pcm_bytes) || pcm_bytes < 44) {
                err = "TTS 音频解析失败";
                if (attempt == 0) { usleep(500000); continue; }
                return false;
            }
            pcm.resize(pcm_bytes / 2);
            memcpy(pcm.data(), wav.data() + pcm_off, pcm_bytes);
            sample_rate = 16000;
            return true;
        }
        return false;
    }

private:
    static int64_t now_ms()
    {
        struct timeval tv;
        gettimeofday(&tv, nullptr);
        return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
    }
    ChatConfig cfg_;
    uint32_t tts_seq_ = 0;
};

std::unique_ptr<TtsBackend> createTtsBackend(const ChatConfig &cfg, std::string *err_out);

// piper 后端在独立文件（条件编译）；工厂分派
std::unique_ptr<TtsBackend> createPiperBackend(const ChatConfig &cfg, std::string *err_out);

std::unique_ptr<TtsBackend> createTtsBackend(const ChatConfig &cfg, std::string *err_out)
{
    if (cfg.tts_provider == "piper") {
        auto backend = createPiperBackend(cfg, err_out);
        if (backend)
            return backend;
        // 降级：cloud 凭据齐全则回退火山
        if (!cfg.tts_app_id.empty() && !cfg.tts_access_token.empty()) {
            if (err_out) *err_out += "（已降级云端 TTS）";
            return std::unique_ptr<TtsBackend>(new CloudTtsBackend(cfg));
        }
        return nullptr;
    }
    // 默认 cloud（含 provider 拼写异常值——保守走云端）
    if (cfg.tts_app_id.empty() || cfg.tts_access_token.empty()) {
        if (err_out) *err_out = "tts_app_id/tts_access_token 未配置（火山引擎）";
        return nullptr;
    }
    return std::unique_ptr<TtsBackend>(new CloudTtsBackend(cfg));
}
