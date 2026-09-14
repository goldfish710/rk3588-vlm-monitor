// vlm_engine.h
// 板端视觉语言模型引擎（RK3588 + RKLLM 1.3.0 部署 Qwen3-VL-2B，替代云端 DeepSeek）
//
// 封装官方 multimodal_model_demo 的推理流程为工程接口：
//   - 纯文本多轮对话：rkllm_run(keep_history=1) 由 RKLLM 内部 KV cache 保留历史，
//     每轮只喂新的 user 输入；超时退出对话时 clearHistory(keep_system=true) 清历史
//   - 多模态（跌倒复核/巡检/远程问答）：vision rknn 编码图像 → RKLLM_INPUT_MULTIMODAL
//   - 系统提示词经 rkllm_set_chat_template 设置（Qwen im_start/im_end 模板）
//
// 线程模型：所有调用都发生在 ChatEngine 的 chat 线程内（rkllm_run 同步阻塞 + 回调同线程），
// 无内部锁。destroy 仅在 stop() 时调用（loop 已退出）。唯一跨线程例外：cancelRun()
//（rkllm_abort 官方支持外部打断，仅限打断进行中的 run）。
#ifndef VLM_ENGINE_H
#define VLM_ENGINE_H

#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "config.h"   // VlmConfig
#include "rkllm.h"    // RKLLMResult/LLMResultCallback 类型（结果回调用真实签名）

class VlmEngine {
public:
    // system_prompt 已做 {name} 替换（ChatEngine init 时传入）
    bool init(const VlmConfig &cfg, const std::string &system_prompt, std::string *err_out);

    // 纯文本生成（多轮历史由 KV cache 保留，每轮只喂 user 输入）
    bool generate(const std::string &user_text, std::string &reply, std::string &err);

    // 流式生成：on_delta 在每次 rkllm 回调（字符级增量）时同步调用
    // （调用线程 = rkllm_run 阻塞线程，即 chat_llm 线程，天然串行）
    using DeltaCb = std::function<void(const std::string &delta)>;
    bool generateStreaming(const std::string &user_text, DeltaCb on_delta,
                           std::string &reply, std::string &err);

    // 图像+文本生成（jpg 文件路径；视觉编码约 3s@448x448，事件场景/远程问答用）
    bool generateWithImage(const std::string &user_text, const std::string &jpg_path,
                           std::string &reply, std::string &err);

    // 图像+文本流式生成（语音对话视觉轮用：vision 编码 + 流式转发 on_delta，
    // 逐句进 TTS 流水线；与 generateStreaming 同语义，仅输入多一张图）
    bool generateStreamingWithImage(const std::string &user_text, const std::string &jpg_path,
                                    DeltaCb on_delta, std::string &reply, std::string &err);

    // 清历史（keep_system=true 保留系统提示词 KV，等价新会话）
    void clearHistory(bool keep_system);

    // 运行时切换系统人设（远程问答切"场景观察员"，结束恢复"陪伴助手"）。
    // 仅替换 chat template 字符串（init 时烧入的模板），不触发模型重载，毫秒级；
    // 只允许在 chat 线程调用（单线程红线）
    bool setSystemPrompt(const std::string &system_prompt);

    void destroy();

    // 跨线程打断正在进行的 rkllm_run（rkllm_abort，SDK 官方支持外部控制）。
    // 唯一允许跨线程调用的接口（红线：其余调用只发生在 chat 线程）；
    // 无 run 进行时调用为无害 no-op（内部先 rkllm_is_running 判断）
    void cancelRun();

    // 上次 run 是否被 cancelRun 打断（chat 线程在 run 返回后查询，随后自动清空）
    bool wasAborted() const { return aborted_.load(); }

    bool isReady() const { return llm_ != nullptr; }

private:
    void *llm_ = nullptr;         // LLMHandle
    void *vis_ctx_ = nullptr;     // VisCtx*（vision 编码器，延迟初始化）
    std::vector<float> img_embed_;
    int n_image_tokens_ = 0;
    int image_width_ = 0;
    int image_height_ = 0;
    VlmConfig cfg_;
    std::string reply_buf_;       // rkllm_run 期间流式回调追加（仅 chat 线程访问）
    DeltaCb on_delta_;            // 流式转发（generateStreaming 临时赋值，run 返回后清空）
    // chat template 三段（init 时按配置填充，setSystemPrompt 重建 system 段复用后两段）
    std::string sys_prompt_;      // "<|im_start|>system\n" + 提示词 + "<|im_end|>\n"
    std::string user_prefix_;     // "<|im_start|>user\n"
    std::string assistant_suffix_;// "<|im_end|>\n<|im_start|>assistant\n"
    std::atomic<bool> abort_requested_{false};  // cancelRun 置位（跨线程）；run 返回非零且置位 ⇒ 被打断
    std::atomic<bool> aborted_{false};          // 上次 run 被打断标记（chat 线程读，run 开始时清空）

    // RKLLM 流式结果回调（static，userdata=this，rkllm_run 传入优先于 init 的 userdata）
    static int resultCallback(RKLLMResult *result, void *userdata, LLMCallState state);

    // 视觉编码公共段（generateWithImage / generateStreamingWithImage 共用）：
    // 读图→预处理→vision rknn 编码→img_embed_；编码完成后若已有 abort 请求
    // （跌倒复核在等 VLM）置 aborted 返回 false（让路点：rknn 编码无法中断）
    bool visionEncode(const std::string &jpg_path, std::string &err);
    bool visionInit(std::string *err_out);
    void visionRelease();
};

#endif // VLM_ENGINE_H
