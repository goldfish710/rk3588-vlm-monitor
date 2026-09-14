# 07. 项目精讲:ASR 语音识别引擎(上)——三模型推理与流式状态

> 目标:读懂 asr_thread.h/.cpp(约 700 行,项目最复杂的模块之一)。
> 本章讲"推理引擎"部分:三个 NPU 模型、状态回填、贪心搜索——
> 你会在其中看到第 1~4 章所有知识的大合奏。

---

## 7.1 模块总览:一个类干完"识别一句话"

```
采集线程 ──feedPcm()──▶ PCM队列 ──asrLoop()──▶ 重采样16k ──▶ fbank特征
   ──▶ 凑够103帧 ──▶ encoder(流式) ──▶ 贪心搜索(joiner+decoder)
   ──▶ token流 ──▶ 段落拼装 ──▶ 回调(关键词/段落/文本文件)
```

AsrEngine 的角色:把"音频字节流"变成"文字事件流"。它内部有一个线程(asr_zip)、一个队列、三个 RKNN 模型、一个 fbank 特征器、若干回调槽。

---

## 7.2 模型上下文:自定义 RknnCtx 结构

```cpp
// server/cpp/asr_thread.h
struct RknnCtx {
    rknn_context ctx = 0;                       // NPU 会话句柄
    rknn_input_output_num io_num{};             // 输入输出张量数量
    std::vector<rknn_tensor_attr> input_attrs;  // 每个张量的类型/维度/量化参数
    std::vector<rknn_tensor_attr> output_attrs;
    std::vector<rknn_input> inputs;             // 推理输入(含预分配缓冲)
    std::vector<rknn_output> outputs;           // 推理输出(预分配,避免每帧 malloc)
};
```

### C++ 视角的三个设计点

1. **向量代替定长数组**:张量数量(36 个!)因模型而异,vector 天然适配
2. **`{}` 值初始化**:`rknn_input_output_num io_num{}` 保证两个计数从 0 开始——C 里忘 memset 就读垃圾值
3. **预分配输出缓冲**:每帧推理都 malloc/free 会碎内存+慢,一次分配复用(项目里所有高频路径都这个思路)

三模型各持一套:`Models { RknnCtx encoder, decoder, joiner; } models_;`——嵌套 struct 的嵌套 struct,数据跟着用途走。

---

## 7.3 build_io:按张量类型分派初始化

```cpp
// server/cpp/asr_thread.cpp  build_io()
for (uint32_t i = 0; i < c->io_num.n_input; i++) {
    rknn_input &in = c->inputs[i];       // 引用:给数组元素起别名,代码可读
    memset(&in, 0, sizeof(in));          // C 结构照样 memset(C 遗产 API)
    in.index = i;
    const rknn_tensor_attr &a = c->input_attrs[i];
    if (a.type == RKNN_TENSOR_FLOAT16) {
        in.size = a.n_elems * sizeof(float);
        in.type = RKNN_TENSOR_FLOAT32;   // 输入统一 float32,runtime 内部转 fp16
        in.buf = malloc(in.size);        // ← malloc!为什么不用 new?
        memset(in.buf, 0, in.size);
    } else if (a.type == RKNN_TENSOR_INT64) {
        in.size = a.n_elems * sizeof(int64_t);
        in.type = RKNN_TENSOR_INT64;
        in.buf = malloc(in.size);
        memset(in.buf, 0, in.size);
    } else {
        fprintf(stderr, "[ASR] 不支持的输入类型 type=%d\n", (int)a.type);
        return false;                    // 防御:遇到未知类型报错而不是崩
    }
}
```

**值得思考的混合风格**:这里 malloc/free 而不是 vector——因为 rknn API 要裸指针,而且缓冲大小固定不需要动态扩容。**C++ 不禁止用 C 的设施,禁止的是"该自动管理时手动管理"**。项目注释里明确这个权衡。

类型分派 if-else 是嵌入式 AI 的常态:同一张量可能 int8/float16/float32,代码必须每种都处理——**我们项目"关键点 conf 被量化毁掉"的事故,根源就在这个分派没做全**(当时只处理了 int8)。

---

## 7.4 encoder 推理 + 状态回填(流式核心)

zipformer 是**流式**模型:每 0.96s 音频一 chunk,encoder 带着"记忆"跨 chunk 工作。记忆怎么传?就靠这 35 个状态张量:

```cpp
// server/cpp/asr_thread.cpp  run_encoder()
int ret = run_model(c);                    // 推理一次
if (ret != RKNN_SUCC) return ret;
// ★关键:输出 1..35 是"更新后的状态",必须拷回输入 1..35,下一 chunk 才能接上
for (uint32_t i = 1; i < c->io_num.n_input && i < c->io_num.n_output; i++) {
    const rknn_tensor_attr &a = c->input_attrs[i];
    if (a.fmt == RKNN_TENSOR_NHWC && a.n_dims == 4) {
        // 4D 张量:NPU 输出是 NCHW 布局,输入要求 NHWC → 手动转置
        int N = a.dims[0], H = a.dims[1], W = a.dims[2], C = a.dims[3];
        convert_nchw_to_nhwc((float*)c->outputs[i].buf,
                             (float*)c->inputs[i].buf, N, C, H, W);
    } else {
        memcpy(c->inputs[i].buf, c->outputs[i].buf, c->inputs[i].size);  // 1D/2D 直接拷
    }
}
```

### 逐点讲透

1. **状态回填是流式的本质**:推理状态保存在"输入缓冲"里,推理后把新状态写回同一缓冲——下次推理直接带着上次的记忆。**这个概念和 LLM 的 KV Cache 同源**(你板子跑不了 LLM,但你亲手实现了它的核心机制!)
2. **NCHW→NHWC 转置**:硬件 NPU 偏爱 NCHW(通道在前),显示/传输偏爱 NHWC(通道在后)。同一份数据两种布局,转换函数就是四层循环重排:

```cpp
static void convert_nchw_to_nhwc(const float *src, float *dst, int N, int C, int H, int W)
{
    for (int n = 0; n < N; ++n)
        for (int c = 0; c < C; ++c)
            for (int h = 0; h < H; ++h)
                for (int w = 0; w < W; ++w)
                    dst[((n * H + h) * W + w) * C + c] = src[((n * C + c) * H + h) * W + w];
}
// C 程序员看这个应该毫无压力:纯下标重排。难点只在"哪个索引乘什么",
// 方法:从最内层 w 向外推——NHWC 里 w 步进 1 个元素,对应 NCHW 里 c 步进 1。
```

3. `i < n_input && i < n_output` 的防御:两者必须等数,防御性检查成本为零、收益是崩溃变报错

---

## 7.5 贪心搜索:你写过的代码,和 LLM 生成一模一样

```cpp
// server/cpp/asr_thread.cpp  runOneChunk() 核心循环
for (int i = 0; i < ENCODER_OUTPUT_T; i++) {        // 24 帧输出
    // joiner:声学帧 + 解码上下文 → 6254 维词表 logits
    run_joiner(&models_.joiner, enc_out + i * DECODER_DIM, dec_out);
    int next_token = argmax(join_out);              // 取概率最大的词
    if (next_token != BLANK_ID && next_token != UNK_ID) {   // blank=停顿,unk=未识别
        float prob = softmax_prob(join_out, next_token);
        hyp_buf[0] = hyp_buf[1];                    // 上下文移位(最多记 2 个词)
        hyp_buf[1] = next_token;
        std::string tok = vocab_[next_token];       // 词表查字
        replace_substr(tok, "▁", " ");              // ▁=词边界符号 → 空格
        onToken(tok, prob, now_ms());               // 送到文本流水线
        run_decoder(&models_.decoder);              // 出新词 → 重跑 decoder
    }
}
```

### 这段代码的含金量

- **这就是 LLM 生成的同款算法**:逐 token 贪心(等价 temperature=0),只是 LLM 的上下文是几万个 token,这里是 2 个。你已经会写"大模型的生成核心"了
- `hyp_buf[0] = hyp_buf[1]` 的移位:不用循环,两个元素直接搬——C 里常见写法,可读性优先
- softmax_prob:logits → 概率,除以防溢出(先减最大值再 exp):

```cpp
float max_logit = logits[idx];
for (int i = 0; i < JOINER_OUTPUT_SIZE; i++)
    if (logits[i] > max_logit) max_logit = logits[i];
double sum = 0.0;
for (int i = 0; i < JOINER_OUTPUT_SIZE; i++)
    sum += std::exp((double)(logits[i] - max_logit));   // 减最大值防 exp 溢出
return (float)(std::exp((double)(logits[idx] - max_logit)) / sum);
```

**数值稳定的 softmax**——C 里直接 exp(logits[i]) 会遇到 exp(700)=inf。这个"减最大值"技巧面试爱考。

---

## 7.6 思考题

1. 状态回填里如果漏掉某个 4D 张量的转置(直接 memcpy),会发生什么现象?(提示:我们项目早期"关键点坐标错乱"就是这个类问题)
2. 为什么 argmax 要跳过 BLANK 和 UNK?如果 BLANK 也当词输出,文本会变成什么样?
3. 把贪心搜索改成 beam search(保留 top-2 候选),hyp_buf 的数据结构要做什么改动?
4. 找一处可以用范围 for 替代的下标循环,改了之后可读性真的变好吗?什么时候下标循环反而更清晰?

---

## 一句话总结

推理引擎 = 上下文结构(RknnCtx)+ 类型分派初始化 + 状态回填(流式)+ 贪心搜索(生成)。看懂这章,你同时看懂了"板端 AI 推理"和"LLM 生成"两个世界。
