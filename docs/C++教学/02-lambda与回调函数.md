# 02. C++ 的灵魂:lambda、std::function、RAII 实践

> 目标:理解项目里到处可见的 `[&audio_cap](){...}` 是什么、为什么这么写。
> 这章是整个项目的"接线艺术"——所有模块都是靠回调串起来的。

---

## 2.1 C 的回调:函数指针的局限

### C 的做法

```c
// C:注册回调 = 存一个函数指针
typedef void (*callback_t)(int value);
void register_cb(callback_t cb) { g_cb = cb; }

// 问题1:回调函数拿不到"上下文"——只能靠全局变量
static int global_ctx;
void my_cb(int v) { printf("%d %d\n", v, global_ctx); }  // 脏

// 问题2:想传两个参数?只能再发明一个带 void* 的写法
typedef void (*callback2_t)(int value, void* userdata);  // 每个库一套
```

C 的回调函数**只带参数、不带现场**。你的项目里有大量"事件发生时通知某个对象"的需求,如果只用 C 函数指针,到处都要塞全局变量——灾难。

### C++ 的解法:lambda(闭包)= 函数 + 捕获的现场

```cpp
// lambda 完整语法:[捕获列表](参数列表) -> 返回类型 { 函数体 }
auto f = [&](int v) { printf("%d\n", v + captured); };  // & = 按引用捕获所有用到的外部变量
f(10);
```

**捕获列表是精髓**:lambda 把"定义处可见的变量"打包带进函数体。C 的回调只有参数,lambda 的回调带着整个现场。

---

## 2.2 std::function:能装下任何"可调用物"的盒子

lambda 没有固定类型(每个 lambda 都是一个匿名类型),怎么存?用 `std::function`:

```cpp
#include <functional>
std::function<void(int)> cb;      // 能装任何"接收 int 返回 void"的东西
cb = [](int v) { ... };           // 装 lambda
cb = 某个普通函数;                 // 装函数
cb = 某个类的成员函数包装;         // 装成员函数
cb(5);                            // 统一调用
```

这就是项目里"回调槽位"的标准写法。

---

## 2.3 项目实例:AudioCapture 的多路分发

### 代码

```cpp
// server/TLServer/AudioCapture.h
struct AudioChunk { int64_t ts_ms; std::vector<uint8_t> data; };

class AudioCapture {
public:
    // 注册一个"消费者"：每来一段音频,就把 AudioChunk 交给它
    void addSink(std::function<void(AudioChunk&&)> sink) {
        sinks_.push_back(std::move(sink));
    }
private:
    std::vector<std::function<void(AudioChunk&&)>> sinks_;   // 一堆回调
};

// server/TLServer/AudioCapture.cpp  captureLoop() 采集线程里:
for (auto& sink : sinks_) {        // 每路分发一份
    AudioChunk chunk;
    chunk.ts_ms = chunk_ts;
    chunk.data = chunk_data;
    sink(std::move(chunk));        // 调用回调
}
```

### 逐行讲解

1. `std::function<void(AudioChunk&&)>` = "接收 AudioChunk 右值引用、返回 void"的可调用物
2. `AudioChunk&&` 是**右值引用**:语义是"这个数据给你了,你可以搬走它"(移动语义,2.5 节)
3. `std::move(chunk)` = "我不用了,你拿走"——配合右值引用避免拷贝
4. sinks_ 是 vector,意味着**可以注册任意多个消费者**——这正是项目架构的精妙处:采集线程完全不知道下游是谁,加功能只加注册

### TLmain 里的三路注册(整个音频架构的缩影)

```cpp
// server/TLServer/TLmain.cpp  音频链路初始化
audio_cap.addSink([&aac_enc](AudioChunk&& c) { aac_enc.feedPcm(c); });        // ① 编码成 AAC(录像+RTSP)
audio_cap.addSink([asr_engine](AudioChunk&& c) { asr_engine->feedPcm(c); });  // ② 喂给语音识别
// 将来加"声音事件检测":再加一行 addSink 就完事——不改任何现有代码
```

三个知识点一次看清:
- `[&aac_enc]`:按引用捕获局部变量 aac_enc
- `[asr_engine]`:按值捕获指针(指针拷贝,代价=8字节,和引用捕获效果一样)
- **观察者模式**:生产者(采集)与消费者(编码/识别)完全解耦

---

## 2.4 项目实例:AsrEngine 的事件回调(多槽位)

```cpp
// server/cpp/asr_thread.h
std::vector<std::function<void(const std::string&, float)>> event_cbs_;
void addEventCallback(std::function<void(const std::string&, float)> cb) {
    event_cbs_.push_back(std::move(cb));
}

// server/cpp/asr_thread.cpp  checkKeywords() 命中关键词时:
for (auto &cb : event_cbs_)
    if (cb) cb(kw, min_prob);      // 逐个通知

// TLmain 注册两路:
asr_engine->addEventCallback([](const std::string& kw, float conf) {
    // ① 呼救词 → MQTT 报警
    MQTTEvent ev; ev.event = "help_call"; ...; mqtt_push_event(ev);
});
asr_engine->addEventCallback([chat_engine](const std::string& kw, float conf) {
    // ② 唤醒词 → 进入对话
    chat_engine->onKeyword(kw, conf);
});
```

**注意回调里的纪律**:这些回调在 ASR 线程被调用。如果回调里做网络请求(几百毫秒),ASR 识别就卡住。所以项目规矩是:**回调里只入队,重活丢给自己的线程**——ChatEngine::onKeyword 就是往队列 push 一下。这个纪律在第 3 章会展开。

---

## 2.5 移动语义入门:std::move 是干什么的

### 拷贝的浪费

```cpp
std::vector<uint8_t> big(1MB);
std::vector<uint8_t> copy = big;   // 拷贝:1MB 内存复制,慢
```

### 移动:所有权转移,不碰数据

```cpp
std::vector<uint8_t> stolen = std::move(big);  // 偷走内部指针,big 变空
// 本质:把 big 的内部指针拷给 stolen,再把 big 的指针置空
// 成本:一个指针的赋值,与 1MB 无关
```

**右值引用 `T&&`** 就是标记"这是个可以被偷的临时对象"。项目里所有队列传递都靠它零拷贝:

```cpp
// server/cpp/asr_thread.cpp  feedPcm()
queue_.push_back(std::move(mono));   // mono 的 vector 数据"搬"进队列,不再拷贝

// server/cpp/chat_engine.cpp  loop()
ev = std::move(ev_queue_.front());    // 事件从队列"搬"到局部
```

C 程序员最容易的误解:`std::move` 不移动任何东西!它只是**类型转换**(把左值标成右值),真正的"偷"是接收方的移动构造函数做的。所以 std::move 后原对象处于"有效但未定义"状态——项目代码里 move 之后从不使用原对象,这是纪律。

---

## 2.6 RAII 实战:ThreadAliveGuard(教科书级写法)

```cpp
// server/TLServer/EncodeThread.cpp(结构在对应头文件)
class ThreadAliveGuard {
public:
    explicit ThreadAliveGuard(std::atomic<bool>& flag) : flag_(flag) {
        flag_ = true;                 // 构造:标记"我活着"
    }
    ~ThreadAliveGuard() {
        flag_ = false;                // 析构:标记"我死了"
    }
private:
    std::atomic<bool>& flag_;
};

void start_encode_h265(...) {
    ThreadAliveGuard alive_guard(g_h265_thread_alive);   // 函数开头声明
    // 无论这个函数怎么退出(正常、return、异常),析构都会把 flag 置 false
}
```

TLmain 的监控线程每秒检查 `g_h265_thread_alive`:线程崩了 → flag 变 false → 重启进程。**这个"监控线程存活"的功能,如果没有 RAII,每个 return 分支都要手动置 flag——漏一个就是监控失效。**

---

## 2.7 思考题

1. `[&]` 和 `[=]` 捕获有什么区别?项目中 `[chat_engine]` 为什么不需要 `&`?
2. 在 TLmain 的 help_call 回调里,如果做 `mqtt_push_event` 需要 100ms,会发生什么?为什么这样设计反而安全?(提示:看 mqtt_push_event 内部是入队还是直接发)
3. `AudioChunk&&` 参数与 `AudioChunk` 按值参数:为什么分发循环里用 move 而不是直接 push 拷贝?找出 copy 版会多花多少内存带宽(音频 44100×2ch×2B≈176KB/s)。

---

## 一句话总结

lambda 把"现场"装进回调,std::function 统一所有回调类型,移动语义让数据传递零拷贝,RAII 让清理自动发生。项目里所有模块接线(TLmain 几百行)就是这四个概念的排列组合。
