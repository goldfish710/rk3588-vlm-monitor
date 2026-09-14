// vlm_engine.cpp
// 板端 VLM 引擎实现：RKLLM 文本/多模态推理 + vision rknn 图像编码
#include "vlm_engine.h"

#include <cstdio>
#include <cstring>
#include <sys/time.h>

#include "rknn_api.h"
#include <opencv2/opencv.hpp>

namespace {

int64_t now_ms()
{
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

// vision 编码器上下文（移植官方 image_enc.h 的 rknn_app_context_t）
struct VisCtx {
    rknn_context rknn_ctx = 0;
    rknn_input_output_num io_num;
    rknn_tensor_attr *input_attrs = nullptr;
    rknn_tensor_attr *output_attrs = nullptr;
    int model_channel = 0;
    int model_width = 0;
    int model_height = 0;
    int model_image_token = 0;
    int model_embed_size = 0;
    int npu_cores = 0;        // 生效的 NPU 核数（0=AUTO，诊断日志用）
};

// 图像补正方形（灰底 127.5 居中，与官方 preprocessing 一致）
cv::Mat expand2square(const cv::Mat &img, const cv::Scalar &bg)
{
    int w = img.cols, h = img.rows;
    if (w == h) return img.clone();
    int size = std::max(w, h);
    cv::Mat result(size, size, img.type(), bg);
    cv::Rect roi((size - w) / 2, (size - h) / 2, w, h);
    img.copyTo(result(roi));
    return result;
}

} // namespace

// ==================== 流式回调 ====================
int VlmEngine::resultCallback(RKLLMResult *r, void *userdata, LLMCallState state)
{
    VlmEngine *self = (VlmEngine *)userdata;
    if (state == RKLLM_RUN_NORMAL && r->text) {
        self->reply_buf_ += r->text;
        if (self->on_delta_)      // 流式转发（同线程同步，见 generateStreaming）
            self->on_delta_(r->text);
    }
    return 0;   // 0=继续推理
}

// ==================== 生命周期 ====================
bool VlmEngine::init(const VlmConfig &cfg, const std::string &system_prompt, std::string *err_out)
{
    cfg_ = cfg;

    RKLLMParam param = rkllm_createDefaultParam();
    param.model_path = cfg_.rkllm_model.c_str();
    param.max_context_len = cfg_.max_context_len;
    param.max_new_tokens = cfg_.max_new_tokens;
    param.top_k = cfg_.top_k;
    param.temperature = cfg_.temperature;
    param.skip_special_token = true;
    param.extend_param.base_domain_id = 1;   // rk3588 专用 NPU 域

    RKLLMCallback cb = {};
    cb.result_callback = &VlmEngine::resultCallback;

    printf("[VLM] 加载 RKLLM 模型（约 2.4GB，需 10~30s）: %s\n", cfg_.rkllm_model.c_str());
    int64_t t0 = now_ms();
    int ret = rkllm_init(&llm_, &param, &cb);
    if (ret != 0) {
        if (err_out) *err_out = "rkllm_init 失败 ret=" + std::to_string(ret) +
                                "（检查模型路径与 NPU 驱动）";
        return false;
    }
    printf("[VLM] rkllm_init 成功，耗时 %lld ms\n", (long long)(now_ms() - t0));

    // 系统提示词：显式设置 chat template（Qwen im_start/im_end 格式，demo 同款）
    // 模型转换时已烧入模板，这里覆盖保证 config.ini 的提示词可控
    sys_prompt_ = "<|im_start|>system\n" + system_prompt + "<|im_end|>\n";
    user_prefix_ = "<|im_start|>user\n";
    assistant_suffix_ = "<|im_end|>\n<|im_start|>assistant\n";
    ret = rkllm_set_chat_template(llm_, sys_prompt_.c_str(),
                                  user_prefix_.c_str(),
                                  assistant_suffix_.c_str());
    if (ret != 0)
        printf("[VLM] set_chat_template 失败 ret=%d（使用模型内置模板）\n", ret);

    return true;
}

bool VlmEngine::setSystemPrompt(const std::string &system_prompt)
{
    if (!llm_) { return false; }
    sys_prompt_ = "<|im_start|>system\n" + system_prompt + "<|im_end|>\n";
    int ret = rkllm_set_chat_template(llm_, sys_prompt_.c_str(),
                                      user_prefix_.c_str(),
                                      assistant_suffix_.c_str());
    if (ret != 0)
        printf("[VLM] set_chat_template(切换人设) 失败 ret=%d\n", ret);
    return ret == 0;
}

void VlmEngine::destroy()
{
    visionRelease();
    if (llm_) {
        rkllm_destroy(llm_);
        llm_ = nullptr;
    }
}

// ==================== 文本生成 ====================
bool VlmEngine::generate(const std::string &user_text, std::string &reply, std::string &err)
{
    return generateStreaming(user_text, nullptr, reply, err);
}

bool VlmEngine::generateStreaming(const std::string &user_text, DeltaCb on_delta,
                                  std::string &reply, std::string &err)
{
    if (!llm_) { err = "VLM 未初始化"; return false; }

    RKLLMInput input = {};
    input.input_type = RKLLM_INPUT_PROMPT;
    input.role = "user";
    input.prompt_input = user_text.c_str();

    RKLLMInferParam infer = {};
    infer.mode = RKLLM_INFER_GENERATE;
    infer.keep_history = 1;   // KV cache 保留历史（多轮）

    reply_buf_.clear();
    aborted_ = false;
    abort_requested_ = false;   // 每轮开始清打断标记
    on_delta_ = std::move(on_delta);   // 流式转发目标（run 返回后清空防跨轮残留）
    int64_t t0 = now_ms();
    int ret = rkllm_run(llm_, &input, &infer, this);
    int64_t cost = now_ms() - t0;
    on_delta_ = nullptr;
    reply = reply_buf_;
    if (ret != 0 && abort_requested_.load()) {
        // 被 cancelRun 打断（run 非零返回 + 本周期内有过 abort 请求）
        aborted_ = true;
        err = "rkllm_run 被 abort 打断";
        return false;
    }
    if (ret != 0) { err = "rkllm_run 失败 ret=" + std::to_string(ret); return false; }
    if (reply.empty()) { err = "VLM 回复为空"; return false; }
    printf("[VLM] 生成 %zu 字，耗时 %lld ms\n", reply.size(), (long long)cost);
    return true;
}

// ==================== 多模态（图像+文本） ====================
// 视觉编码公共段（generateWithImage / generateStreamingWithImage 共用）
bool VlmEngine::visionEncode(const std::string &jpg_path, std::string &err)
{
    if (!vis_ctx_ && !visionInit(&err)) return false;
    VisCtx *vis = (VisCtx *)vis_ctx_;
    // 打断标记在 vision 编码前复位：复核抢占可能发生在编码期间（rknn 无 abort API，
    // 只能编码完成后让路），若在 rkllm_run 前才复位会把编码期间的 abort 请求覆盖掉
    aborted_ = false;
    abort_requested_ = false;

    // 预处理：BGR→RGB → 补正方形(灰底) → 448x448
    cv::Mat img = cv::imread(jpg_path);
    if (img.empty()) { err = "图片读取失败: " + jpg_path; return false; }
    cv::cvtColor(img, img, cv::COLOR_BGR2RGB);
    cv::Mat sq = expand2square(img, cv::Scalar(127.5, 127.5, 127.5));
    cv::Mat resized;
    cv::resize(sq, resized, cv::Size(image_width_, image_height_), 0, 0, cv::INTER_LINEAR);

    // vision rknn 编码
    int64_t t_vis0 = now_ms();
    size_t n_out = vis->io_num.n_output;
    img_embed_.resize((size_t)n_image_tokens_ * vis->model_embed_size * n_out);

    rknn_input inputs[1];
    rknn_output outputs[vis->io_num.n_output];
    memset(inputs, 0, sizeof(inputs));
    memset(outputs, 0, sizeof(outputs));
    inputs[0].index = 0;
    inputs[0].type = RKNN_TENSOR_UINT8;
    inputs[0].fmt = RKNN_TENSOR_NHWC;
    inputs[0].size = (uint32_t)(image_width_ * image_height_ * vis->model_channel);
    inputs[0].buf = resized.data;

    int ret = rknn_inputs_set(vis->rknn_ctx, 1, inputs);
    if (ret < 0) { err = "rknn_inputs_set 失败 ret=" + std::to_string(ret); return false; }
    ret = rknn_run(vis->rknn_ctx, nullptr);
    if (ret < 0) { err = "rknn_run 失败 ret=" + std::to_string(ret); return false; }
    for (uint32_t j = 0; j < vis->io_num.n_output; j++)
        outputs[j].want_float = 1;
    ret = rknn_outputs_get(vis->rknn_ctx, vis->io_num.n_output, outputs, nullptr);
    if (ret < 0) { err = "rknn_outputs_get 失败 ret=" + std::to_string(ret); return false; }

    // 多输出拼接：官方按 token-major 交错（deepstack 分段输出 + input_embed）
    float *dst = img_embed_.data();
    size_t emb = (size_t)vis->model_embed_size;
    if (vis->io_num.n_output == 1) {
        memcpy(dst, outputs[0].buf, outputs[0].size);
    } else {
        for (int i = 0; i < vis->model_image_token; i++) {
            for (uint32_t j = 0; j < vis->io_num.n_output; j++) {
                memcpy(dst + i * n_out * emb + j * emb,
                       (float *)outputs[j].buf + i * emb, sizeof(float) * emb);
            }
        }
    }
    rknn_outputs_release(vis->rknn_ctx, vis->io_num.n_output, outputs);
    printf("[VLM] vision 编码耗时 %lld ms（%u 核，NPU 与 YOLO/ASR 并发争抢会拉长）\n",
           (long long)(now_ms() - t_vis0), vis->npu_cores);

    // 复核抢占让路点：vision 编码期间无法中断（rknn_run 阻塞无 abort API），
    // 编码完成后若已有 abort 请求（跌倒复核在等 VLM），跳过 LLM 生成直接让路——
    // 节省的生成段 ~3-4s 就是复核链在 12s 预算内的关键
    if (abort_requested_.load()) {
        aborted_ = true;
        err = "多模态生成被 abort 打断（vision 编码后让路）";
        return false;
    }
    return true;
}

bool VlmEngine::generateWithImage(const std::string &user_text, const std::string &jpg_path,
                                  std::string &reply, std::string &err)
{
    if (!llm_) { err = "VLM 未初始化"; return false; }
    if (!visionEncode(jpg_path, err)) return false;

    // 文本：自动前置 <image> 占位符（Qwen3-VL chat template 约定）
    std::string prompt = user_text;
    if (prompt.find("<image>") == std::string::npos)
        prompt = "<image>" + prompt;

    RKLLMInput input = {};
    input.input_type = RKLLM_INPUT_MULTIMODAL;
    input.role = "user";
    input.multimodal_input.prompt = (char *)prompt.c_str();
    input.multimodal_input.image.image_embed = img_embed_.data();
    input.multimodal_input.image.n_image_tokens = (size_t)n_image_tokens_;
    input.multimodal_input.image.n_image = 1;
    input.multimodal_input.image.image_start = "<|vision_start|>";
    input.multimodal_input.image.image_end = "<|vision_end|>";
    input.multimodal_input.image.image_content = "<|image_pad|>";
    input.multimodal_input.image.image_width = (size_t)image_width_;
    input.multimodal_input.image.image_height = (size_t)image_height_;

    RKLLMInferParam infer = {};
    infer.mode = RKLLM_INFER_GENERATE;
    infer.keep_history = 1;

    reply_buf_.clear();
    int64_t t_llm0 = now_ms();
    int ret = rkllm_run(llm_, &input, &infer, this);
    reply = reply_buf_;
    printf("[VLM] 多模态生成耗时 %lld ms\n", (long long)(now_ms() - t_llm0));
    if (ret != 0 && abort_requested_.load()) {
        aborted_ = true;
        err = "rkllm_run(多模态) 被 abort 打断";
        return false;
    }
    if (ret != 0) { err = "rkllm_run(多模态) 失败 ret=" + std::to_string(ret); return false; }
    if (reply.empty()) { err = "VLM 回复为空"; return false; }
    return true;
}

// 图像+文本流式生成（语音对话视觉轮：vision 编码 + 流式转发，逐句进 TTS 流水线）
bool VlmEngine::generateStreamingWithImage(const std::string &user_text, const std::string &jpg_path,
                                           DeltaCb on_delta, std::string &reply, std::string &err)
{
    if (!llm_) { err = "VLM 未初始化"; return false; }
    if (!visionEncode(jpg_path, err)) return false;

    // 文本：自动前置 <image> 占位符（Qwen3-VL chat template 约定）
    std::string prompt = user_text;
    if (prompt.find("<image>") == std::string::npos)
        prompt = "<image>" + prompt;

    RKLLMInput input = {};
    input.input_type = RKLLM_INPUT_MULTIMODAL;
    input.role = "user";
    input.multimodal_input.prompt = (char *)prompt.c_str();
    input.multimodal_input.image.image_embed = img_embed_.data();
    input.multimodal_input.image.n_image_tokens = (size_t)n_image_tokens_;
    input.multimodal_input.image.n_image = 1;
    input.multimodal_input.image.image_start = "<|vision_start|>";
    input.multimodal_input.image.image_end = "<|vision_end|>";
    input.multimodal_input.image.image_content = "<|image_pad|>";
    input.multimodal_input.image.image_width = (size_t)image_width_;
    input.multimodal_input.image.image_height = (size_t)image_height_;

    RKLLMInferParam infer = {};
    infer.mode = RKLLM_INFER_GENERATE;
    infer.keep_history = 1;

    reply_buf_.clear();
    on_delta_ = std::move(on_delta);   // 流式转发（同 generateStreaming 语义）
    int64_t t0 = now_ms();
    int ret = rkllm_run(llm_, &input, &infer, this);
    on_delta_ = nullptr;
    reply = reply_buf_;
    printf("[VLM] 多模态流式生成耗时 %lld ms\n", (long long)(now_ms() - t0));
    if (ret != 0 && abort_requested_.load()) {
        aborted_ = true;
        err = "rkllm_run(多模态) 被 abort 打断";
        return false;
    }
    if (ret != 0) { err = "rkllm_run(多模态) 失败 ret=" + std::to_string(ret); return false; }
    if (reply.empty()) { err = "VLM 回复为空"; return false; }
    return true;
}

// ==================== 跨线程打断 ====================
void VlmEngine::cancelRun()
{
    abort_requested_ = true;
    // 仅打断进行中的 run（rkllm_is_running 为 SDK 提供的运行态查询，
    // 避免对已完成的 run 调 abort 产生副作用）
    if (llm_ && rkllm_is_running(llm_))
        rkllm_abort(llm_);
}

// ==================== 历史清理 ====================
void VlmEngine::clearHistory(bool keep_system)
{
    if (!llm_) return;
    // keep_system=1：保留系统提示词 KV，等价"新会话"（系统人设不变）
    int ret = rkllm_clear_kv_cache(llm_, keep_system ? 1 : 0, nullptr, nullptr);
    if (ret != 0)
        printf("[VLM] clear_kv_cache 失败 ret=%d\n", ret);
}

// ==================== vision 编码器（延迟初始化） ====================
bool VlmEngine::visionInit(std::string *err_out)
{
    VisCtx *vis = new VisCtx();
    int ret = rknn_init(&vis->rknn_ctx, (void *)cfg_.vision_model.c_str(), 0, 0, nullptr);
    if (ret < 0) {
        delete vis;
        if (err_out) *err_out = "vision rknn_init 失败 ret=" + std::to_string(ret) +
                                "（模型: " + cfg_.vision_model + "）";
        return false;
    }
    // NPU 核数：0=auto，2/3 与官方 demo 一致
    vis->npu_cores = cfg_.npu_core_num;
    if (cfg_.npu_core_num == 2)
        rknn_set_core_mask(vis->rknn_ctx, RKNN_NPU_CORE_0_1);
    else if (cfg_.npu_core_num == 3)
        rknn_set_core_mask(vis->rknn_ctx, RKNN_NPU_CORE_0_1_2);
    else
        rknn_set_core_mask(vis->rknn_ctx, RKNN_NPU_CORE_AUTO);

    ret = rknn_query(vis->rknn_ctx, RKNN_QUERY_IN_OUT_NUM, &vis->io_num, sizeof(vis->io_num));
    if (ret != RKNN_SUCC) {
        rknn_destroy(vis->rknn_ctx);
        delete vis;
        if (err_out) *err_out = "vision rknn_query 失败";
        return false;
    }
    rknn_tensor_attr in_attr = {};
    in_attr.index = 0;
    ret = rknn_query(vis->rknn_ctx, RKNN_QUERY_INPUT_ATTR, &in_attr, sizeof(in_attr));
    if (ret != RKNN_SUCC) {
        rknn_destroy(vis->rknn_ctx);
        delete vis;
        if (err_out) *err_out = "vision 输入查询失败";
        return false;
    }
    rknn_tensor_attr out_attr = {};
    out_attr.index = 0;
    ret = rknn_query(vis->rknn_ctx, RKNN_QUERY_OUTPUT_ATTR, &out_attr, sizeof(out_attr));
    if (ret != RKNN_SUCC) {
        rknn_destroy(vis->rknn_ctx);
        delete vis;
        if (err_out) *err_out = "vision 输出查询失败";
        return false;
    }
    // 输入 dims：NCHW [1,C,H,W] 或 NHWC [1,H,W,C]（与官方同逻辑）
    if (in_attr.fmt == RKNN_TENSOR_NCHW) {
        vis->model_channel = in_attr.dims[1];
        vis->model_height = in_attr.dims[2];
        vis->model_width = in_attr.dims[3];
    } else {
        vis->model_height = in_attr.dims[1];
        vis->model_width = in_attr.dims[2];
        vis->model_channel = in_attr.dims[3];
    }
    // 输出 dims：[tokens, embed] 或 [1, tokens, embed, 1]——取第一个 >1 的维为 token 数
    for (int i = 0; i < 4; i++) {
        if (out_attr.dims[i] > 1) {
            vis->model_image_token = out_attr.dims[i];
            vis->model_embed_size = out_attr.dims[i + 1];
            break;
        }
    }
    n_image_tokens_ = vis->model_image_token;
    image_width_ = vis->model_width;
    image_height_ = vis->model_height;
    vis_ctx_ = vis;
    printf("[VLM] vision 编码器就绪: %dx%dx%d, %d tokens x %d embed x %u 输出\n",
           image_width_, image_height_, vis->model_channel,
           n_image_tokens_, vis->model_embed_size, vis->io_num.n_output);
    return true;
}

void VlmEngine::visionRelease()
{
    if (vis_ctx_) {
        VisCtx *vis = (VisCtx *)vis_ctx_;
        if (vis->rknn_ctx) rknn_destroy(vis->rknn_ctx);
        delete vis;
        vis_ctx_ = nullptr;
    }
}
