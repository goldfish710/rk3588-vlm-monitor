# 08. 项目精讲:ChatEngine 对话引擎——状态机与多轮对话

> 目标:读懂 chat_engine.h/.cpp(约 500 行)。这是项目的"业务逻辑核心":
> 唤醒 → 收句子 → 调 LLM → TTS → 播放 → 多轮循环,还有跌倒/求救事件插队。

---

## 8.1 类结构:事件驱动的状态机

```cpp
// server/cpp/chat_engine.h(节选)
class ChatEngine {
public:
    bool init(const ChatConfig &cfg, std::string *err_out);
    void start();
    void stop();
    void onKeyword(const std::string &kw, float conf);   // ASR 线程回调
    void onSegment(const std::string &segment);          // ASR 线程回调
    void onFallEvent();                                  // 跌倒事件线程回调
    void setPlayStateCallback(std::function<void(bool)> cb);

private:
    enum State { IDLE, AWAIT_QUERY, BUSY };              // 三个状态
    enum EventType { EV_WAKE, EV_SEGMENT, EV_SYSTEM_QUERY, EV_FALL_QUERY };
    struct Event { EventType type; std::string text; float conf; };

    void loop();                                          // 工作线程
    void handleQuery(const std::string &query, bool skip_next_seg);
    bool doLlm(const std::string &query, std::string &reply, std::string &err);
    bool doTts(const std::string &text, std::string &wav_path, std::string &err);
    void playReply(const std::string &text, std::string &err);

    std::atomic<bool> running_{false};
    std::mutex ev_mtx_;
    std::condition_variable ev_cv_;
    std::deque<Event> ev_queue_;
    State state_ = IDLE;                                  // 仅 loop 线程访问!
    std::vector<std::pair<std::string, std::string>> history_;  // 多轮历史
};
```

### 设计看点

1. **enum 状态机**:C 里也是这么写的(enum + switch),C++ 没变——状态机是语言无关的模式
2. **"仅 loop 线程访问"的注释约定**:state_ 不需要锁,因为只有一个线程碰它。**用注释声明数据归属**,这是项目里最重要的隐性契约
3. **pair 存对话历史**:`std::pair<角色,内容>` 轻量二元组。history_ 上限 12 条,超出删最旧:

```cpp
void ChatEngine::appendHistory(const std::string &role, const std::string &content) {
    history_.push_back({role, content});     // 花括号初始化 pair
    while (history_.size() > 12)
        history_.erase(history_.begin());    // 窗口滑出
}
```

---

## 8.2 状态机主循环:三种状态的行为表

```
        EV_WAKE          EV_SEGMENT        EV_SYSTEM_QUERY     EV_FALL_QUERY
IDLE    进入AWAIT(10s)    丢弃              发LLM(求救词)        发LLM(跌倒)
AWAIT   重置计时         发LLM(剥唤醒词)     丢弃(已在文本流)      发LLM
BUSY    记住重置         丢弃              丢弃                 丢弃
```

对应代码骨架(节选):

```cpp
if (state_ == IDLE) {
    if (ev.type == EV_WAKE) {
        state_ = AWAIT_QUERY;
        await_timeout_ms_ = (int64_t)cfg_.wake_timeout_sec * 1000;
    } else if (ev.type == EV_SYSTEM_QUERY) {
        handleQuery(ev.text, true);          // 求救词:直接当用户问话处理
    } else if (ev.type == EV_FALL_QUERY) {
        handleQuery(ev.text, true);          // 跌倒事件
    }
} else if (state_ == AWAIT_QUERY) {
    if (ev.type == EV_SEGMENT) {
        if (skip_next_segment_) { skip_next_segment_ = false; continue; }  // 防重
        std::string query = stripWakeWord(ev.text);
        if (query.empty()) continue;
        handleQuery(query, false);
    }
    // ...
}
```

### 三个值得学的细节

1. **超时用"绝对时刻+时长"存两个变量**:

```cpp
await_enter_ms_ = now_ms();
await_timeout_ms_ = 10000;
// 检查超时:
int64_t remain = await_enter_ms_ + await_timeout_ms_ - now_ms();
if (remain <= 0) { state_ = IDLE; ... }
```

为什么不只存"到期时刻"?因为计时要**重置**(每次唤醒/每轮结束都重新计时),存起点+时长让重置逻辑变成两行赋值。

2. **skip_next_segment_ 防重**(事件同句防重的巧思):求救词命中 → 发 LLM("救命")→ 1 秒后 ASR 判段把同一句话("救命")再回调 → 若再发一次 LLM 就重复了。解法:事件处理后置标志,丢弃**下一个**段落。为什么恰好是"下一个"?因为老人接下来的回应("我没事")是下下个段落,不受影响。

3. **条件变量等待与超时共舞**(再复习):

```cpp
std::unique_lock<std::mutex> lock(ev_mtx_);
int timeout_ms = 500;
if (state_ == AWAIT_QUERY) {
    int64_t remain = await_enter_ms_ + await_timeout_ms_ - now_ms();
    if (remain <= 0) { state_ = IDLE; continue; }     // 超时退出对话
    if (remain < timeout_ms) timeout_ms = (int)remain; // 动态缩短等待
}
ev_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this] {
    return !ev_queue_.empty() || !running_.load();
});
```

**动态超时**:等待时间 = min(500ms 轮询, 剩余超时)。一个条件变量同时服务"来事件"和"状态超时"两件事,不用开定时线程。

---

## 8.3 handleQuery:一次完整问答的流水线

```cpp
void ChatEngine::handleQuery(const std::string &query, bool skip_next_seg) {
    printf("[Chat] 用户: \"%s\"\n", query.c_str());
    state_ = BUSY;

    std::string reply, err;
    if (!doLlm(query, reply, err)) {
        reply = "网络不太好，请稍后再试";          // 兜底:失败也有话说
    }
    if (running_.load()) {                          // 请求期间可能被 stop!
        std::string perr;
        playReply(reply, perr);                     // TTS + 播放(内部有回声门控)
    }
    appendHistory("user", query);                   // 成功与否都记用户的话
    if (!reply.empty() && reply != "网络不太好，请稍后再试")
        appendHistory("assistant", reply);          // 兜底话不进历史(否则污染)

    if (running_.load()) {
        state_ = AWAIT_QUERY;                       // 回到等待:多轮继续
        skip_next_segment_ = skip_next_seg;
    }
}
```

### 逐点看

1. **`running_.load()` 的两次检查**:LLM 请求要 1~2 秒、TTS 播放要几秒——这期间用户可能 Ctrl+C。每次长操作后都要问"我还该继续吗?"——**长任务路径上的检查点是优雅退出的关键**(项目踩过"退出时还在发请求"的坑)
2. **兜底回复不进历史**:如果网络失败的话也进历史,下一轮 LLM 会以为"自己说过这句话",上下文被污染。这个判断很细
3. **一次问答 = 一个函数**:抽成 handleQuery 后,说话段落/求救事件/跌倒事件三条入口共用同一条流水线——**消除重复的三处调用,而不是复制粘贴三次**

---

## 8.4 doLlm:组装请求与解析流式响应

```cpp
bool ChatEngine::doLlm(const std::string &query, std::string &reply, std::string &err) {
    for (int attempt = 0; attempt < 2; attempt++) {   // 失败重试一次
        // ---- 用 cJSON 构建请求体(对比手写字符串拼接) ----
        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "model", cfg_.llm_model.c_str());
        cJSON_AddBoolToObject(root, "stream", true);
        cJSON *msgs = cJSON_AddArrayToObject(root, "messages");
        // system + 历史 + 本次问话,逐个 AddItemToArray
        char *json = cJSON_PrintUnformatted(root);    // 序列化
        std::string body(json);
        cJSON_free(json);
        cJSON_Delete(root);

        // ---- HTTPS 请求(第 09 章细讲) ----
        HttpResponse resp = httpsPost(..., body, 10, 15, 60);
        if (resp.status != 200) { /* 解析错误 JSON,重试 */ continue; }

        // ---- SSE 解析:逐行拆 data: ----
        std::string pending((const char *)resp.body.data(), resp.body.size());
        bool done = false;
        while (!pending.empty() && !done) {
            size_t nl = pending.find('\n');
            std::string line = (nl == std::string::npos) ? pending : pending.substr(0, nl);
            pending.erase(0, nl == std::string::npos ? pending.size() : nl + 1);
            line = trimCr(line);
            if (line.empty() || line[0] == ':') continue;      // keep-alive 注释行
            if (line.compare(0, 6, "data: ") != 0) continue;
            std::string payload = line.substr(6);
            if (payload == "[DONE]") { done = true; break; }
            cJSON *ev = cJSON_Parse(payload.c_str());
            // choices[0].delta.content 逐层取值,缺失即跳过
        }
    }
}
```

### C 程序员会觉得亲切的部分

- 字符串处理还是那套:find/substr/erase——只是 std::string 版本更安全
- cJSON 是 C 库,但**用 RAII 思想包着用**:出错路径要 cJSON_Delete 释放,别忘

### C++ 特有的部分

- `std::string body(json)` 把 C 字符串转成 string 后,**json 缓冲立刻 free**——所有权干净移交
- 重试循环 + 错误分级(第 6 章学过的模式,网络版)

---

## 8.5 思考题

1. BUSY 状态下所有事件被丢弃。跌倒事件恰好在播报窗口触发就丢了——怎么改造成"排队等 BUSY 结束"?写出你的方案(提示:EV_FALL_QUERY 在 BUSY 分支 push 回队列会死循环,想清楚怎么办)。
2. 为什么"兜底回复不进历史"而"用户的话失败也进历史"?反过来的后果是什么?
3. `history_` 上限 12 条:12 条中文大约多少 token?对应 DeepSeek 计费多少?(提示:中文 1 字≈0.6~1 token)
4. 状态机加一个"EV_TIMEOUT"显式事件(而不是靠 wait_for 超时隐式触发),两种设计各自的优劣?

---

## 一句话总结

ChatEngine = 状态机(三态)+ 事件队列(四类事件)+ 统一流水线(handleQuery)+ 多轮历史。业务逻辑模块的正确组织方式:状态集中在一处,行为集中在 handleQuery,入口只管投事件。
