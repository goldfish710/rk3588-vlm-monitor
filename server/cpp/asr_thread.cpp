// asr_thread.cpp
// zipformer 流式语音识别引擎实现
// 移植自官方 rknn_model_zoo/examples/zipformer/cpp（Apache-2.0）：
//   三模型 RKNN 封装 / 状态回填 / 贪心搜索 / vocab 工具函数
// 增量流式化改造：队列消费、44.1k→16k 重采样、重叠 hop 水位排空、
//   静音门控、尾段补零 flush、关键词匹配与冷却。
#include "asr_thread.h"
#include "thread_util.h"   // bind_rt_thread（asrLoop 绑大核掩码，见 asrLoop 注释）

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <pthread.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <unistd.h>

#include "kaldi-native-fbank/csrc/online-feature.h"

// ==================== 常量（与官方 demo process.h 一致） ====================
#define VOCAB_NUM 6257
#define ASR_SAMPLE_RATE 16000
#define N_MELS 80
#define N_SEGMENT 103                 // 每 chunk 输入帧数（10ms/帧 → 1.03s 窗口）
#define ENCODER_OUTPUT_T 24           // encoder 每 chunk 输出帧数（/4 下采样）
#define DECODER_DIM 512
#define JOINER_OUTPUT_SIZE 6254       // 词表 6257 - 末尾 3 个占位 token
#define N_OFFSET 96                   // 流式 hop（0.96s，与 segment 重叠 7 帧）
#define CONTEXT_SIZE 2                // decoder 上下文 token 数
#define BLANK_ID 0
#define UNK_ID 2

static int64_t now_ms() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

// ==================== 工具函数（移植自官方 process.cc） ====================
static void convert_nchw_to_nhwc(const float *src, float *dst, int N, int C, int H, int W)
{
    for (int n = 0; n < N; ++n)
        for (int c = 0; c < C; ++c)
            for (int h = 0; h < H; ++h)
                for (int w = 0; w < W; ++w)
                    dst[n * H * W * C + h * W * C + w * C + c] =
                        src[n * C * H * W + c * H * W + h * W + w];
}

// 注：D1 fp16 对齐评估后放弃（见 build_io 注释），相关辅助函数不保留

static int get_fbank_frames(knf::OnlineFbank *fbank, int frame_index, int segment, float *frames)
{
    if (frame_index + segment > fbank->NumFramesReady())
        return -1;
    for (int i = 0; i < segment; ++i) {
        const float *frame = fbank->GetFrame(i + frame_index);
        memcpy(frames + i * N_MELS, frame, N_MELS * sizeof(float));
    }
    return 0;
}

static int argmax(const float *array)
{
    int max_index = 0;
    float max_value = array[0];
    for (int i = 1; i < JOINER_OUTPUT_SIZE; i++) {
        if (array[i] > max_value) {
            max_value = array[i];
            max_index = i;
        }
    }
    return max_index;
}

// 单帧 joiner logits 的 softmax 概率（max 归一防溢出；6254 维，微秒级）
static float softmax_prob(const float *logits, int idx)
{
    float max_logit = logits[idx];
    for (int i = 0; i < JOINER_OUTPUT_SIZE; i++)
        if (logits[i] > max_logit) max_logit = logits[i];
    double sum = 0.0;
    for (int i = 0; i < JOINER_OUTPUT_SIZE; i++)
        sum += std::exp((double)(logits[i] - max_logit));
    return (float)(std::exp((double)(logits[idx] - max_logit)) / sum);
}

static void replace_substr(std::string &str, const std::string &from, const std::string &to)
{
    if (from.empty()) return;
    size_t pos = 0;
    while ((pos = str.find(from, pos)) != std::string::npos) {
        str.replace(pos, from.length(), to);
        pos += to.length();
    }
}

// ==================== WAV 解析（自写，不引入 libsndfile） ====================
static bool parseWav(const std::string &path, std::vector<int16_t> &pcm,
                     int &rate, int &channels)
{
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return false;
    char hdr[12];
    if (fread(hdr, 1, 12, f) != 12 || memcmp(hdr, "RIFF", 4) != 0 ||
        memcmp(hdr + 8, "WAVE", 4) != 0) {
        fclose(f);
        return false;
    }
    bool have_fmt = false, have_data = false;
    uint32_t data_len = 0, data_off = 0;
    while (!have_data) {
        char ch[8];
        if (fread(ch, 1, 8, f) != 8) break;
        uint32_t sz = (uint32_t)(uint8_t)ch[4] | ((uint32_t)(uint8_t)ch[5] << 8) |
                      ((uint32_t)(uint8_t)ch[6] << 16) | ((uint32_t)(uint8_t)ch[7] << 24);
        if (memcmp(ch, "fmt ", 4) == 0) {
            std::vector<uint8_t> b(sz);
            if (sz > 64 || sz < 16 || fread(b.data(), 1, sz, f) != sz) break;
            uint16_t fmt_tag = (uint16_t)(b[0] | (b[1] << 8));
            channels = (int)(b[2] | (b[3] << 8));
            rate = (int)(b[4] | (b[5] << 8) | (b[6] << 16) | (b[7] << 24));
            uint16_t bits = (uint16_t)(b[14] | (b[15] << 8));
            if (fmt_tag != 1 || bits != 16) break;   // 仅支持 PCM16
            have_fmt = true;
        } else if (memcmp(ch, "data", 4) == 0) {
            data_len = sz;
            data_off = (uint32_t)ftell(f);
            have_data = true;
        } else {
            fseek(f, sz, SEEK_CUR);
        }
    }
    if (!have_fmt || !have_data) {
        fclose(f);
        return false;
    }
    fseek(f, data_off, SEEK_SET);
    pcm.resize(data_len / 2);
    if (fread(pcm.data(), 2, pcm.size(), f) != pcm.size()) {
        fclose(f);
        return false;
    }
    fclose(f);
    return true;
}

// ==================== 重采样器（任意整数率 → 16k，线性插值，流式相位状态） ====================
// fbank 要求 16kHz；本项目采集为 44.1kHz，故需降采样。线性插值对语音
// 关键词识别足够（44.1k→16k 的混叠可忽略，实测验证）。
class Resampler {
public:
    explicit Resampler(int src_rate) : step_((double)src_rate / ASR_SAMPLE_RATE) {}

    // 喂入一段 mono S16，输出重采样后的 float 波形（-1~1）
    void feed(const int16_t *in, size_t n, std::vector<float> &out)
    {
        buf_.insert(buf_.end(), in, in + n);
        while (pos_ + 1.0 < in_base_ + (double)buf_.size()) {
            size_t i = (size_t)(pos_ - in_base_);
            double frac = pos_ - (in_base_ + (double)i);
            double s = (double)buf_[i] * (1.0 - frac) + (double)buf_[i + 1] * frac;
            out.push_back((float)(s / 32768.0));
            pos_ += step_;
        }
        size_t consumed = (size_t)(pos_ - in_base_);
        if (consumed > 0) {
            buf_.erase(buf_.begin(), buf_.begin() + consumed);
            in_base_ += (double)consumed;
        }
        if (buf_.size() > 4096) {   // 防御：正常情况下只残留 1~2 个样本
            size_t excess = buf_.size() - 2;
            buf_.erase(buf_.begin(), buf_.begin() + excess);
            in_base_ += (double)excess;
        }
    }

    void reset()
    {
        buf_.clear();
        pos_ = 0.0;
        in_base_ = 0.0;
    }

private:
    double step_;
    double pos_ = 0.0;        // 下一输出对应的输入流位置（相对 in_base_）
    double in_base_ = 0.0;    // buf_[0] 对应的全局位置
    std::vector<int16_t> buf_;
};

// ==================== 模型上下文（移植官方 build_input_output / init / release / 推理） ====================
namespace {

bool build_io(AsrEngine::RknnCtx *c)
{
    c->inputs.resize(c->io_num.n_input);
    c->outputs.resize(c->io_num.n_output);
    for (uint32_t i = 0; i < c->io_num.n_input; i++) {
        rknn_input &in = c->inputs[i];
        memset(&in, 0, sizeof(in));
        in.index = i;
        const rknn_tensor_attr &a = c->input_attrs[i];
        if (a.type == RKNN_TENSOR_FLOAT16) {
            // D1 评估结论：4 维状态 fp16 直喂需自研 NC1HWC2 解包（交错访问可能更慢），
            // 非 4 维张量收益仅 ~1%；RTF 0.102 余量 10 倍 → 维持 float32 喂（runtime 内部转换）
            in.size = a.n_elems * sizeof(float);
            in.type = RKNN_TENSOR_FLOAT32;
            in.fmt = a.fmt;
            in.buf = malloc(in.size);
            memset(in.buf, 0, in.size);
        } else if (a.type == RKNN_TENSOR_INT64) {
            in.size = a.n_elems * sizeof(int64_t);
            in.type = RKNN_TENSOR_INT64;
            in.fmt = a.fmt;
            in.buf = malloc(in.size);
            memset(in.buf, 0, in.size);
        } else {
            fprintf(stderr, "[ASR] 不支持的输入类型 type=%d\n", (int)a.type);
            return false;
        }
    }
    for (uint32_t i = 0; i < c->io_num.n_output; i++) {
        rknn_output &o = c->outputs[i];
        memset(&o, 0, sizeof(o));
        o.index = i;
        const rknn_tensor_attr &a = c->output_attrs[i];
        if (a.type == RKNN_TENSOR_FLOAT16) {
            // D1 评估结论：维持 want_float（见 build_io 输入侧注释）
            o.size = a.n_elems * sizeof(float);
            o.is_prealloc = true;
            o.want_float = 1;
            o.buf = malloc(o.size);
        } else if (a.type == RKNN_TENSOR_INT64) {
            o.size = a.n_elems * sizeof(int64_t);
            o.is_prealloc = true;
            o.want_float = 0;
            o.buf = malloc(o.size);
        } else {
            fprintf(stderr, "[ASR] 不支持的输出类型 type=%d\n", (int)a.type);
            return false;
        }
    }
    return true;
}

bool init_model(const std::string &path, AsrEngine::RknnCtx *c, int npu_core_num, std::string *err_out)
{
    int ret = rknn_init(&c->ctx, (void *)path.c_str(), 0, 0, NULL);
    if (ret != RKNN_SUCC) {
        if (err_out) *err_out = "rknn_init 失败: " + path + " ret=" + std::to_string(ret);
        return false;
    }
    // NPU 核数实验键（性能调优 A/B）：0=不设置（AUTO，旧行为）
    if (npu_core_num > 0) {
        rknn_core_mask mask = RKNN_NPU_CORE_0;
        if (npu_core_num == 2) mask = RKNN_NPU_CORE_0_1;
        else if (npu_core_num == 3) mask = RKNN_NPU_CORE_0_1_2;
        rknn_set_core_mask(c->ctx, mask);
        printf("[ASR-Perf] %s NPU 核数=%d\n", path.c_str(), npu_core_num);
    }
    ret = rknn_query(c->ctx, RKNN_QUERY_IN_OUT_NUM, &c->io_num, sizeof(c->io_num));
    if (ret != RKNN_SUCC) {
        if (err_out) *err_out = "查询输入输出数量失败: " + path;
        return false;
    }
    c->input_attrs.resize(c->io_num.n_input);
    c->output_attrs.resize(c->io_num.n_output);
    for (uint32_t i = 0; i < c->io_num.n_input; i++) {
        c->input_attrs[i].index = i;
        rknn_query(c->ctx, RKNN_QUERY_INPUT_ATTR, &c->input_attrs[i], sizeof(rknn_tensor_attr));
    }
    for (uint32_t i = 0; i < c->io_num.n_output; i++) {
        c->output_attrs[i].index = i;
        rknn_query(c->ctx, RKNN_QUERY_OUTPUT_ATTR, &c->output_attrs[i], sizeof(rknn_tensor_attr));
    }
    if (!build_io(c)) {
        if (err_out) *err_out = "build_input_output 失败: " + path;
        return false;
    }
    return true;
}

void release_model(AsrEngine::RknnCtx *c)
{
    for (auto &in : c->inputs)
        if (in.buf) { free(in.buf); in.buf = NULL; }
    for (auto &o : c->outputs)
        if (o.buf) { free(o.buf); o.buf = NULL; }
    if (c->ctx != 0) {
        rknn_destroy(c->ctx);
        c->ctx = 0;
    }
    c->inputs.clear();
    c->outputs.clear();
}

int run_model(AsrEngine::RknnCtx *c)
{
    int ret = rknn_inputs_set(c->ctx, c->io_num.n_input, c->inputs.data());
    if (ret != RKNN_SUCC) return ret;
    ret = rknn_run(c->ctx, NULL);
    if (ret != RKNN_SUCC) return ret;
    ret = rknn_outputs_get(c->ctx, c->io_num.n_output, c->outputs.data(), NULL);
    return ret;
}

// encoder 推理 + 流式状态回填（输出 1..35 → 输入 1..35；4D 状态 NCHW→NHWC）
int run_encoder(AsrEngine::RknnCtx *c, double *refill_ms_out)
{
    int ret = run_model(c);
    if (ret != RKNN_SUCC) return ret;
    int64_t t_refill = now_ms();
    for (uint32_t i = 1; i < c->io_num.n_input && i < c->io_num.n_output; i++) {
        const rknn_tensor_attr &a = c->input_attrs[i];
        if (a.fmt == RKNN_TENSOR_NHWC && a.n_dims == 4) {
            int N = a.dims[0], H = a.dims[1], W = a.dims[2], C = a.dims[3];
            convert_nchw_to_nhwc((float *)c->outputs[i].buf, (float *)c->inputs[i].buf,
                                 N, C, H, W);
        } else {
            memcpy(c->inputs[i].buf, c->outputs[i].buf, c->inputs[i].size);
        }
    }
    if (refill_ms_out) *refill_ms_out = (double)(now_ms() - t_refill);
    return ret;
}

int run_decoder(AsrEngine::RknnCtx *c) { return run_model(c); }

int run_joiner(AsrEngine::RknnCtx *c, const float *enc_frame, const float *dec_out)
{
    memcpy(c->inputs[0].buf, enc_frame, c->input_attrs[0].n_elems * sizeof(float));
    memcpy(c->inputs[1].buf, dec_out, c->input_attrs[1].n_elems * sizeof(float));
    return run_model(c);
}

} // namespace

// ==================== AsrEngine 实现 ====================
bool AsrEngine::init(const AsrConfig &cfg, std::function<bool()> silence_probe, std::string *err_out)
{
    cfg_ = cfg;
    silence_probe_ = std::move(silence_probe);

    // 1. 词表
    FILE *fp = fopen(cfg_.vocab_path.c_str(), "r");
    if (!fp) {
        if (err_out) *err_out = "vocab 打开失败: " + cfg_.vocab_path;
        return false;
    }
    vocab_.assign(VOCAB_NUM, "");
    char line[512];
    int loaded = 0;
    while (fgets(line, sizeof(line), fp)) {
        char *sp = strchr(line, ' ');
        if (!sp) continue;
        *sp = '\0';
        int idx = atoi(sp + 1);
        if (idx >= 0 && idx < VOCAB_NUM) {
            vocab_[idx] = line;
            loaded++;
        }
    }
    fclose(fp);
    if (loaded < VOCAB_NUM - 3) {
        if (err_out) *err_out = "vocab 条目不足: " + std::to_string(loaded);
        return false;
    }
    printf("[ASR] 词表加载完成: %d 条\n", loaded);

    // 2. 三模型
    std::string enc = cfg_.model_dir + "/encoder-epoch-99-avg-1.rknn";
    std::string dec = cfg_.model_dir + "/decoder-epoch-99-avg-1.rknn";
    std::string joi = cfg_.model_dir + "/joiner-epoch-99-avg-1.rknn";
    if (!init_model(enc, &models_.encoder, cfg_.npu_core_num, err_out) ||
        !init_model(dec, &models_.decoder, cfg_.npu_core_num, err_out) ||
        !init_model(joi, &models_.joiner, cfg_.npu_core_num, err_out)) {
        return false;
    }
    printf("[ASR] 三模型加载完成 (encoder in/out=%u/%u, decoder in/out=%u/%u, joiner in/out=%u/%u)\n",
           models_.encoder.io_num.n_input, models_.encoder.io_num.n_output,
           models_.decoder.io_num.n_input, models_.decoder.io_num.n_output,
           models_.joiner.io_num.n_input, models_.joiner.io_num.n_output);

    // 3. 特征/输出缓冲
    encoder_in_.assign(N_MELS * N_SEGMENT, 0.0f);
    encoder_out_.assign(ENCODER_OUTPUT_T * DECODER_DIM, 0.0f);

    // 4. fbank（16k、80 维 mel，与官方 demo 参数一致）
    knf::FbankOptions fbank_opts;
    fbank_opts.frame_opts.samp_freq = ASR_SAMPLE_RATE;
    fbank_opts.mel_opts.num_bins = N_MELS;
    fbank_opts.mel_opts.high_freq = -400;
    fbank_opts.frame_opts.dither = 0;
    fbank_opts.frame_opts.snip_edges = false;
    fbank_ = new knf::OnlineFbank(fbank_opts);

    // 5. 关键词
    {
        std::string kws = cfg_.keywords;
        size_t pos = 0;
        while (pos < kws.size()) {
            size_t comma = kws.find(',', pos);
            std::string kw = kws.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
            if (!kw.empty()) {
                // 去掉空白后压入
                kw.erase(std::remove_if(kw.begin(), kw.end(), ::isspace), kw.end());
                keywords_.push_back(kw);
            }
            if (comma == std::string::npos) break;
            pos = comma + 1;
        }
        printf("[ASR] 关键词 %zu 个: %s\n", keywords_.size(), cfg_.keywords.c_str());
        // 唤醒词集合（冷却判断用；唤醒词 10s 独立冷却）
        for (const auto &w : splitCsv(g_config.chat.wake_word))
            wake_words_.insert(w);
    }

    // 6. 说话段落记录文件（追加模式；停顿 2s 写一段）
    if (!cfg_.text_log_file.empty()) {
        text_fp_ = fopen(cfg_.text_log_file.c_str(), "a");
        if (text_fp_)
            printf("[ASR] 识别文本记录: %s\n", cfg_.text_log_file.c_str());
        else
            fprintf(stderr, "[ASR] 文本记录文件打开失败: %s\n", cfg_.text_log_file.c_str());
    }

    // 7. 可选离线自检
    if (!cfg_.debug_wav_path.empty()) {
        printf("[ASR] 离线自检: %s\n", cfg_.debug_wav_path.c_str());
        float rtf = 0.0f;
        std::string text = runOfflineWav(cfg_.debug_wav_path, &rtf);
        printf("[ASR] 离线自检结果: \"%s\" (RTF=%.3f)\n", text.c_str(), rtf);
        clearStreamState();
    }

    return true;
}

void AsrEngine::start()
{
    if (running_.load()) return;
    running_ = true;
    thread_ = std::thread(&AsrEngine::asrLoop, this);
}

void AsrEngine::stop()
{
    if (!running_.load()) return;
    running_ = false;
    q_cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    // 尾段 flush（<103 帧时补零+InputFinished+最后一轮）
    flushTail();
    // 收尾：最后一段话落盘 + 关文件
    flushSegment();
    if (text_fp_) {
        fclose(text_fp_);
        text_fp_ = nullptr;
    }
}

void AsrEngine::feedPcm(const AudioChunk &chunk)
{
    if (cfg_.capture_channels <= 0) return;          // 防除零(配置异常时)
    const int16_t *p = reinterpret_cast<const int16_t *>(chunk.data.data());
    size_t frames = chunk.data.size() / 2;          // 2 字节一样本（交错）
    if (frames == 0) return;
    if (chunk.data.size() > 400000) {               // 防御:正常块 ~35KB(200ms),超 10 倍视为异常
        printf("[ASR] ★异常★ 音频块尺寸异常 %zu B,丢弃\n", chunk.data.size());
        return;
    }

    // 降混（与 AacEncoder::downmixToMono 语义一致），仅此轻量活
    PcmChunk mono;
    mono.ts_ms = chunk.ts_ms;
    last_audio_ts_ms_ = chunk.ts_ms;   // 话音结束打点：最新音频的墙钟时刻
    mono.mono.reserve(frames / cfg_.capture_channels);
    for (size_t i = 0; i + cfg_.capture_channels <= frames; i += cfg_.capture_channels) {
        int16_t m;
        switch (cfg_.mono_source) {
            case 2:  m = p[i + 1]; break;
            case 0:  m = (int16_t)(((int)p[i] + (int)p[i + 1]) / 2); break;
            default: m = p[i]; break;
        }
        mono.mono.push_back(m);
    }

    {
        std::lock_guard<std::mutex> lock(q_mtx_);
        if (queue_.size() >= kQueueMax) {
            queue_.pop_front();                      // 有界：丢最旧
        }
        if (queue_.size() >= 200) {                // 防御:200 块=40s 积压上限(正常消费下不可能达到)
            static int64_t last_qlog = 0;
            if (now_ms() - last_qlog > 30000) {
                printf("[ASR] ★异常★ 队列积压 %zu 块,丢弃最旧(ASR 线程疑似卡住)\n", queue_.size());
                last_qlog = now_ms();
            }
            queue_.pop_front();
        }
        queue_.push_back(std::move(mono));
    }
    q_cv_.notify_one();
}

void AsrEngine::addEventCallback(std::function<void(const std::string &, float)> cb)
{
    event_cbs_.push_back(std::move(cb));
}

void AsrEngine::setSegmentCallback(std::function<void(const std::string &, int64_t)> cb)
{
    segment_cb_ = std::move(cb);
}

void AsrEngine::setSuppressInput(bool suppress)
{
    suppress_input_ = suppress;
    if (suppress) {
        // 门控开启瞬间清掉已在管道里的文字（含回声 token），防播放后误触发
        text_segs_.clear();
        cur_segment_.clear();
    }
}

// ==================== 流式主循环 ====================
void AsrEngine::asrLoop()
{
    pthread_setname_np(pthread_self(), "asr_zip");
    setpriority(PRIO_PROCESS, (pid_t)syscall(SYS_gettid), 10);   // 低优先级，不抢视频/AI
    // 绑大核掩码（与 RT 线程同 4-6，nice 10 保证让位）：实测不绑时被内核挤到 A55 小核，
    // CPU 侧回填/类型转换耗时翻倍（回填 10.5→21.6ms）
    bind_rt_thread();

    Resampler resampler(cfg_.capture_rate);
    int64_t last_water_log_ms = 0;
    int64_t chunk_count = 0;
    int64_t chunk_ts_start = now_ms();
    double chunk_infer_ms_sum = 0.0;
    int64_t last_health_ms = now_ms();

    while (running_.load()) {
        PcmChunk chunk;
        {
            std::unique_lock<std::mutex> lock(q_mtx_);
            q_cv_.wait_for(lock, std::chrono::milliseconds(200), [this] {
                return !queue_.empty() || !running_.load();
            });
            if (queue_.empty()) {
                if (!running_.load()) break;
                // 无数据时也检查段落超时（停顿 2s 落盘一段）
                if (!cur_segment_.empty() && now_ms() - last_token_ms_ > cfg_.segment_timeout_ms)
                    flushSegment();
                continue;
            }
            chunk = std::move(queue_.front());
            queue_.pop_front();
        }

        // 段落超时落盘（说话停顿 2s 视为一段结束）
        if (!cur_segment_.empty() && now_ms() - last_token_ms_ > cfg_.segment_timeout_ms)
            flushSegment();

        // 周期健康遥测(每 5 分钟):长稳排障用——RSS 趋势可定位慢性泄漏,
        // fbank 水位/队列长度可定位累积型异常(2026-09-12 8h bad_alloc 事故后加)
        if (now_ms() - last_health_ms > 300000) {
            long rss_pages = 0;
            FILE *sf = fopen("/proc/self/statm", "r");
            if (sf) { fscanf(sf, "%ld", &rss_pages); fclose(sf); }
            // fbank_rem = 未消费剩余帧(真实水位)。注意 NumFramesReady() 语义是
            // "总追加帧数"(RecyclingVector::Size),已消费帧由 popped_frames_ 记;
            // 2026-09-12 长稳泄漏事故的遥测指纹即两者之差应保持有界
            printf("[ASR] 健康: rss=%.1fMB queue=%zu fbank_rem=%d(total=%d processed=%d popped=%d)\n",
                   rss_pages * 4096.0 / 1048576.0, queue_.size(),
                   fbank_->NumFramesReady() - popped_frames_,
                   fbank_->NumFramesReady(), processed_frames_, popped_frames_);
            last_health_ms = now_ms();
        }

        // 重采样 → fbank（无论是否静音都喂入：silent_ 判定有 ≤1s 滞后，
        // 若静音时丢弃会吃掉每句话的开头，短句几乎全丢）
        if (chunk.mono.size() > 100000) {            // 防御:单块 200ms≈8820 样本,超 11 倍视为损坏
            printf("[ASR] ★异常★ 队列块样本数异常 %zu,丢弃\n", chunk.mono.size());
            continue;
        }
        std::vector<float> w16k;
        resampler.feed(chunk.mono.data(), chunk.mono.size(), w16k);
        if (w16k.size() > 100000) {                  // 防御:单块重采样输出最多 ~3.2k 样本
            printf("[ASR] ★异常★ 重采样输出膨胀 %zu 样本,重置重采样器\n", w16k.size());
            resampler.reset();
            w16k.clear();
        }
        if (!w16k.empty()) {
            fbank_->AcceptWaveform(ASR_SAMPLE_RATE, w16k.data(), (int32_t)w16k.size());
        }

        // 帧回收:删除已消费的旧帧内存(保留 200+103 帧余量)。
        // 注意:Pop(n) 语义是"再删 n 帧"(非"删到剩 n")——必须传与上次的
        // 增量,传累计值会把帧反复删光,GetFrame 下标下溢崩溃(实测踩过)。
        if (processed_frames_ > 200 + N_SEGMENT) {
            int target = processed_frames_ - 200 - N_SEGMENT;
            if (target > popped_frames_) {
                fbank_->Pop(target - popped_frames_);
                popped_frames_ = target;
            }
        }

        // 静音门控：静音时不推理（省 NPU）；积压的静音帧用"跳帧+状态清零"
        // 排空——不再重建 fbank 对象（实测重建路径有 ~70KB/次的固定内存增长,
        // 每 2s 一次 ≈ 4.2MB/2min 的线性泄漏;跳帧方案零内存分配,效果等效）。
        // 双重条件：距上次识别出词 2s 内绝不清空（防恰好撞上说话开头）。
        if (cfg_.silence_gate && silence_probe_ && silence_probe_()) {
            int64_t n = now_ms();
            if (n - last_token_ms_ > 2000 && n - last_reset_ms_ > 2000) {
                int ready = fbank_->NumFramesReady();
                if (ready - processed_frames_ > 200) {
                    // 丢弃积压静音帧(保留最近 ~1s),状态归零,等效重建
                    processed_frames_ = ready - N_SEGMENT;
                    text_segs_.clear();   // 与重建一致:静音隔断关键词窗口
                    for (uint32_t i = 1; i < models_.encoder.io_num.n_input; i++)
                        memset(models_.encoder.inputs[i].buf, 0,
                               models_.encoder.inputs[i].size);
                    memset(models_.decoder.inputs[0].buf, 0,
                           CONTEXT_SIZE * sizeof(int64_t));
                    last_reset_ms_ = n;
                }
            }
            continue;
        }

        // fbank 水位熔断:正常稳态 ≤~500 帧;若长时运行累积异常(2026-09-12 板端
        // 实测连续运行 8h 后 ASR 线程巨型分配失败 bad_alloc),超阈值强制排空并记录
        {
            int ready0 = fbank_->NumFramesReady();
            if (ready0 - processed_frames_ > 3000) {
                static int64_t last_fblog = 0;
                if (now_ms() - last_fblog > 30000) {
                    printf("[ASR] ★异常★ fbank 水位 %d 帧(>3000),强制排空\n", ready0 - processed_frames_);
                    last_fblog = now_ms();
                }
                fbank_->Pop(ready0 - processed_frames_ - N_SEGMENT);
                popped_frames_ = processed_frames_;
                processed_frames_ = fbank_->NumFramesReady() - N_SEGMENT;
            }
        }

        // 增量 chunk 循环（官方 demo 是喂完整段后循环，这里随喂随跑）
        while (true) {
            int ready = fbank_->NumFramesReady();
            int avail = ready - processed_frames_;
            if (avail < N_SEGMENT) break;
            if (avail >= N_SEGMENT + N_OFFSET) {
                // 重叠 hop 水位积累（每 chunk 净增 7 帧）→ 跳一轮排空（丢 0.96s）
                processed_frames_ += N_OFFSET;
                int64_t n = now_ms();
                if (n - last_water_log_ms > 30000) {
                    last_water_log_ms = n;
                    printf("[ASR] 水位=%d 帧，跳一轮排空（重叠 hop 积累，属正常现象）\n", avail);
                }
                continue;
            }
            int64_t t0 = now_ms();
            runOneChunk();
            chunk_infer_ms_sum += (double)(now_ms() - t0);
            chunk_count++;
            if (chunk_count >= 50) {   // 每 50 chunk 打印一次性能
                double sec = (double)(now_ms() - chunk_ts_start) / 1000.0;
                printf("[ASR] 50 chunk: 推理耗时均值=%.1fms (RTF≈%.3f) 相位: encoder=%.1fms 回填=%.1fms 贪心=%.1fms\n",
                       chunk_infer_ms_sum / 50.0,
                       sec > 0 ? (chunk_infer_ms_sum / 1000.0) / sec : 0.0,
                       dbg_enc_ms_ / 50.0, dbg_refill_ms_ / 50.0, dbg_greedy_ms_ / 50.0);
                dbg_enc_ms_ = dbg_refill_ms_ = dbg_greedy_ms_ = 0.0;
                chunk_count = 0;
                chunk_infer_ms_sum = 0.0;
                chunk_ts_start = now_ms();
            }
        }
    }
    printf("[ASR] 识别线程退出\n");
}

// encoder(状态回填) → 贪心（24×joiner + 逐 token decoder）
void AsrEngine::runOneChunk()
{
    if (get_fbank_frames(fbank_, processed_frames_, N_SEGMENT, encoder_in_.data()) != 0)
        return;

    int64_t t_enc = now_ms();
    memcpy(models_.encoder.inputs[0].buf, encoder_in_.data(),
           N_MELS * N_SEGMENT * sizeof(float));
    double refill_ms = 0.0;
    if (run_encoder(&models_.encoder, &refill_ms) != RKNN_SUCC) {
        fprintf(stderr, "[ASR] encoder 推理失败\n");
        return;
    }
    dbg_enc_ms_ += (double)(now_ms() - t_enc);
    dbg_refill_ms_ += refill_ms;

    int64_t t_greedy = now_ms();
    if (processed_frames_ == 0) {
        memset(models_.decoder.inputs[0].buf, 0, CONTEXT_SIZE * sizeof(int64_t));
        if (run_decoder(&models_.decoder) != RKNN_SUCC) {
            fprintf(stderr, "[ASR] decoder 推理失败\n");
            return;
        }
    }

    float *enc_out = (float *)models_.encoder.outputs[0].buf;
    float *dec_out = (float *)models_.decoder.outputs[0].buf;
    float *join_out = (float *)models_.joiner.outputs[0].buf;
    int64_t *hyp_buf = (int64_t *)models_.decoder.inputs[0].buf;

    for (int i = 0; i < ENCODER_OUTPUT_T; i++) {
        if (run_joiner(&models_.joiner, enc_out + i * DECODER_DIM, dec_out) != RKNN_SUCC) {
            fprintf(stderr, "[ASR] joiner 推理失败\n");
            return;
        }
        int next_token = argmax(join_out);
        if (next_token != BLANK_ID && next_token != UNK_ID) {
            float prob = softmax_prob(join_out, next_token);
            hyp_buf[0] = hyp_buf[1];
            hyp_buf[1] = next_token;
            std::string tok = vocab_[next_token];
            replace_substr(tok, "▁", " ");
            onToken(tok, prob, now_ms());
            if (run_decoder(&models_.decoder) != RKNN_SUCC) {
                fprintf(stderr, "[ASR] decoder 推理失败\n");
                return;
            }
        }
    }

    processed_frames_ += N_OFFSET;
    dbg_greedy_ms_ += (double)(now_ms() - t_greedy);
}

// stop 时尾段 flush：<103 帧补零 + InputFinished + 最后一轮（保证最后话音不丢）
void AsrEngine::flushTail()
{
    if (!fbank_ || stream_finished_) return;
    int ready = fbank_->NumFramesReady();
    int avail = ready - processed_frames_;
    if (avail <= 0 || avail >= N_SEGMENT) return;

    int pad_frames = N_SEGMENT - avail;
    std::vector<float> zeros((size_t)pad_frames * ASR_SAMPLE_RATE / 100, 0.0f);
    fbank_->AcceptWaveform(ASR_SAMPLE_RATE, zeros.data(), (int32_t)zeros.size());
    fbank_->InputFinished();
    stream_finished_ = true;
    runOneChunk();
    printf("[ASR] 尾段 flush: 剩余 %d 帧补零 %d 帧完成\n", avail, pad_frames);
}

// ==================== 文本缓冲与关键词匹配 ====================
void AsrEngine::onToken(const std::string &token, float prob, int64_t ts_ms)
{
    // 回声门控：TTS 播放期间解出的 token 是喇叭回声，全部丢弃
    if (suppress_input_.load()) return;

    text_segs_.push_back({token, ts_ms, prob});
    // 裁剪最近 10 秒
    while (!text_segs_.empty() && ts_ms - text_segs_.front().ts_ms > 10000)
        text_segs_.erase(text_segs_.begin());
    // 说话段落累积（token 里的 ▁ 已在 runOneChunk 中替换为空格）
    // 每识别出一个词立即写盘：新段落开头打时间戳，词边写边 flush，
    // cat 随时可见（识别本身另有 ~1.5s 攒帧+推理的固有延迟）
    if (text_fp_) {
        if (cur_segment_.empty()) {
            time_t t = ts_ms / 1000;
            struct tm tmv;
            localtime_r(&t, &tmv);
            char tsbuf[32];
            snprintf(tsbuf, sizeof(tsbuf), "[%02d:%02d:%02d] ", tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
            fputs(tsbuf, text_fp_);
        }
        fputs(token.c_str(), text_fp_);
        fflush(text_fp_);
    }
    cur_segment_ += token;
    last_token_ms_ = ts_ms;
    speech_end_ms_ = last_audio_ts_ms_;   // 本 token 对应的话音结束时刻
    checkKeywords(ts_ms);
}

// 段落结束（停顿 2s / 退出时）：补换行（内容已实时写盘）+ 段落回调
// 注意：段回调与文本记录文件解耦——text_log_file 未配置时回调也必须工作
void AsrEngine::flushSegment()
{
    if (cur_segment_.empty()) return;
    if (text_fp_) {
        fputs("\n", text_fp_);
        fflush(text_fp_);
    }
    if (segment_cb_) segment_cb_(cur_segment_, speech_end_ms_);
    cur_segment_.clear();
    speech_end_ms_ = 0;
}

void AsrEngine::checkKeywords(int64_t ts_ms)
{
    // 拼接紧凑串（去掉空格：中文关键词子串匹配；BPE 下"救命"可能被切成两个 token）
    std::string compact;
    float min_prob = 1.0f;
    for (const auto &seg : text_segs_) {
        for (char ch : seg.token)
            if (ch != ' ') compact.push_back(ch);
        if (seg.prob < min_prob) min_prob = seg.prob;
    }
    if (compact.empty()) return;

    for (const auto &kw : keywords_) {
        if (compact.find(kw) == std::string::npos) continue;
        // 每词独立冷却：唤醒词 10s（对话中频繁使用），其余用 asr_cooldown_sec
        bool is_wake = wake_words_.count(kw) > 0;
        int64_t cd_ms = is_wake ? 10000 : (int64_t)cfg_.cooldown_sec * 1000;
        auto it = last_fire_map_.find(kw);
        if (it != last_fire_map_.end() && cd_ms > 0 && ts_ms - it->second < cd_ms)
            return;                                  // 该词冷却中
        last_fire_map_[kw] = ts_ms;
        printf("[ASR] ★命中关键词★ \"%s\" (conf=%.2f)\n", kw.c_str(), min_prob);
        if (cfg_.debug_text_log)
            printf("[ASR] 识别文本窗口: \"%s\"\n", compact.c_str());
        text_segs_.clear();                          // 一段话只报一次
        for (auto &cb : event_cbs_)
            if (cb) cb(kw, min_prob);
        return;
    }
}

void AsrEngine::clearStreamState()
{
    processed_frames_ = 0;
    popped_frames_ = 0;   // fbank 重建,回收计数归零
    stream_finished_ = false;
    memset(models_.decoder.inputs[0].buf, 0, CONTEXT_SIZE * sizeof(int64_t));
    // encoder 流式缓存状态清零（输入 1..35 全零 = 初始状态）
    for (uint32_t i = 1; i < models_.encoder.io_num.n_input; i++)
        memset(models_.encoder.inputs[i].buf, 0, models_.encoder.inputs[i].size);
    text_segs_.clear();
    last_fire_map_.clear();
    cur_segment_.clear();   // 离线自检的文本不写入记录文件
    if (fbank_) {
        delete fbank_;
        knf::FbankOptions fbank_opts;
        fbank_opts.frame_opts.samp_freq = ASR_SAMPLE_RATE;
        fbank_opts.mel_opts.num_bins = N_MELS;
        fbank_opts.mel_opts.high_freq = -400;
        fbank_opts.frame_opts.dither = 0;
        fbank_opts.frame_opts.snip_edges = false;
        fbank_ = new knf::OnlineFbank(fbank_opts);
    }
}

// ==================== 离线验证（同步，独立 fbank/状态，与在线串行使用） ====================
std::string AsrEngine::runOfflineWav(const std::string &path, float *rtf_out)
{
    std::vector<int16_t> pcm;
    int rate = 0, channels = 0;
    if (!parseWav(path, pcm, rate, channels)) {
        fprintf(stderr, "[ASR] wav 解析失败: %s\n", path.c_str());
        return "";
    }
    printf("[ASR] wav: %zu 样本 %dHz/%dch (%.2fs)\n",
           pcm.size() / (size_t)channels, rate, channels,
           (double)pcm.size() / channels / rate);

    // 转 mono float @16k（复用 Resampler；16k 源则近乎直通）
    std::vector<int16_t> mono;
    mono.reserve(pcm.size() / channels);
    for (size_t i = 0; i + channels <= pcm.size(); i += channels) {
        int16_t m = (channels == 1) ? pcm[i] : (int16_t)(((int)pcm[i] + (int)pcm[i + 1]) / 2);
        mono.push_back(m);
    }
    Resampler rs(rate);
    std::vector<float> w16k;
    rs.feed(mono.data(), mono.size(), w16k);

    // 重建独立 fbank（离线独占，跑完恢复在线 fbank）
    clearStreamState();   // 顺带把在线 fbank/状态归零
    fbank_->AcceptWaveform(ASR_SAMPLE_RATE, w16k.data(), (int32_t)w16k.size());

    std::string full_text;
    int64_t t_start = now_ms();
    int ready = fbank_->NumFramesReady();
    while (ready - processed_frames_ >= N_SEGMENT) {
        runOneChunk();
        ready = fbank_->NumFramesReady();
    }
    // 尾段（与 flushTail 同规则）
    {
        int avail = ready - processed_frames_;
        if (avail > 0 && avail < N_SEGMENT) {
            int pad_frames = N_SEGMENT - avail;
            std::vector<float> zeros((size_t)pad_frames * ASR_SAMPLE_RATE / 100, 0.0f);
            fbank_->AcceptWaveform(ASR_SAMPLE_RATE, zeros.data(), (int32_t)zeros.size());
            fbank_->InputFinished();
            stream_finished_ = true;
            runOneChunk();
        }
    }
    int64_t t_elapsed = now_ms() - t_start;

    for (const auto &seg : text_segs_) full_text += seg.token;
    // 去掉 ▁ 留下的空格
    full_text.erase(std::remove(full_text.begin(), full_text.end(), ' '), full_text.end());

    double audio_sec = (double)w16k.size() / ASR_SAMPLE_RATE;
    float rtf = audio_sec > 0 ? (float)(t_elapsed / 1000.0 / audio_sec) : 0.0f;
    if (rtf_out) *rtf_out = rtf;

    return full_text;
}
