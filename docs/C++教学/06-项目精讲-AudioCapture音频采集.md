# 06. 项目精讲:AudioCapture 音频采集

> 目标:以 AudioCapture(约 230 行,项目里最"小而美"的类)为教材,
> 学会读一个完整 C++ 类:接口设计、状态管理、错误恢复、优雅退出。

---

## 6.1 先看类的"门面":公开接口即设计文档

```cpp
// server/TLServer/AudioCapture.h
struct AudioChunk {
    int64_t ts_ms;                    // 块起始墙钟毫秒
    std::vector<uint8_t> data;        // S16_LE 交错 PCM
};

class AudioCapture {
public:
    struct Config {                   // 嵌套配置结构:配置跟着类走
        std::string device = "hw:0,0";
        int rate = 44100;
        int channels = 2;
        int pre_buffer_sec = 5;
    };

    bool init(const Config& cfg, std::string* err_out = nullptr);
    void shutdown();
    void addSink(std::function<void(AudioChunk&&)> sink);   // 多路分发
    bool enabled() const { return running_.load(); }
    bool silent() const { return silent_.load(); }          // 静音检测结果

private:
    snd_pcm_t* pcm_ = nullptr;                    // ALSA 句柄
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> silent_{true};
    std::vector<std::function<void(AudioChunk&&)>> sinks_;
};
```

### 接口设计的三个 C++ 习惯

1. **错误返回用"布尔 + 出参"而不是错误码**:`bool init(..., std::string* err_out)`——成功返回 true;失败返回 false 并填人类可读的错误串。比 C 的 `int ret` + `strerror(ret)` 组合更直观,错误信息可以带上下文("打开 hw:0,0 失败: Device busy")
2. **查询用 const 成员函数**:`bool silent() const`——调用者放心:查询不会改状态,可以多线程并发读
3. **成员默认初始化**:`snd_pcm_t* pcm_ = nullptr`——C 里 struct 未初始化指针是野指针,C++ 在声明处给 nullptr,永远安全

---

## 6.2 设备打开:多候选回退链

```cpp
// server/TLServer/AudioCapture.cpp  openDevice()
const char* candidates[3] = {cfg_.device.c_str(), "default", nullptr};
// 配置设备失败 → 试 "default" → 都没有则报错
for (int i = 0; candidates[i]; i++) {
    int rc = snd_pcm_open(&pcm_, candidates[i], SND_PCM_STREAM_CAPTURE, 0);
    if (rc < 0) {
        printf("[AudioCapture] 打开 %s 失败: %s\n", candidates[i], snd_strerror(rc));
        continue;                    // 失败继续下一个候选
    }
    // 设置格式:16bit 有符号、交错读写、44100Hz/2ch、200ms 缓冲
    rc = snd_pcm_set_params(pcm_, SND_PCM_FORMAT_S16_LE,
                            SND_PCM_ACCESS_RW_INTERLEAVED,
                            cfg_.channels, cfg_.rate, 0, 200000);
    if (rc < 0) {
        snd_pcm_close(pcm_);
        pcm_ = nullptr;              // 失败要复位!否则残留半开句柄
        continue;
    }
    return true;                     // 成功即返回
}
```

**C++ 视角的要点**:这段和 C 写 ALSA 一模一样——C++ 不改变系统调用,改变的是**组织方式**(封装进类、错误串出参、半途失败的状态清理)。看硬件类代码时记住:**系统 API 部分当 C 读,对象生命周期部分按 C++ 读**。

**踩坑点(我们项目真踩过)**:`snd_pcm_start` 必须在 poll/read 之前显式调用——注释写着"否则 poll 在未启动的流上永远等不到事件"。板端调试大坑,已沉淀在注释里。

---

## 6.3 采集线程:阻塞读 + 错误分级恢复

```cpp
// server/TLServer/AudioCapture.cpp  captureLoop()
while (running_.load()) {
    snd_pcm_sframes_t n = snd_pcm_readi(pcm_, buf.data(), period_size);   // 阻塞读
    if (n < 0) {
        if (!running_.load()) break;       // 关停触发 → 正常退出
        if (n == -EPIPE) {
            snd_pcm_prepare(pcm_);         // xrun(缓冲区溢出/欠载):prepare 后继续
            xrun_count++;
            if (now - last_xrun_log > 1000) { /* 限速打日志 */ }
        } else if (snd_pcm_recover(pcm_, (int)n, 1) < 0) {
            consecutive_failures++;
            if (consecutive_failures > 30) {
                printf("连续读取失败超限，采集线程退出（视频不受影响）\n");
                break;                     // 兜底:别死循环刷错误
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        continue;
    }
    consecutive_failures = 0;              // 成功一次就清零(关键!)
    ...
}
```

### 错误处理的三级策略(工程精髓)

| 级别 | 手段 | 对应代码 |
|---|---|---|
| 可恢复错误 | 立刻恢复继续跑 | -EPIPE → prepare |
| 偶发连续错误 | 计数 + 限速重试 | recover 失败 ×30 → 放弃 |
| 致命错误 | 退出线程但**不拖垮进程** | break + "视频不受影响" |

`consecutive_failures = 0` 这一行特别值得学:**成功必须重置失败计数**——否则偶尔一次抖动会累积到阈值误杀线程。

---

## 6.4 多路分发:再品 addSink 设计

```cpp
// captureLoop() 每拿到一块音频:
for (auto& sink : sinks_) {
    AudioChunk chunk;
    chunk.ts_ms = chunk_ts;
    chunk.data = chunk_data;         // 每路一份拷贝
    sink(std::move(chunk));
}
```

注释写着"每路一份拷贝;64KB/s,开销可忽略"——**工程判断**:为省 64KB/s 的拷贝引入共享内存的复杂度,不值。C 程序员最容易过度优化拷贝,这个注释就是"按收益定价"的范本。

`chunk_ts` 的计算也值得一看:

```cpp
int64_t chunk_ts = now_ms - (int64_t)((uint64_t)n * 1000 / rate_);
// 块起始时刻 = 读取完成时刻 - 块时长
```

**读完成时打的时间戳要往回推**——因为 50ms 的数据是过去 50ms 里采的,不是读到的瞬间。这种"时间戳归位"细节在音视频领域到处都是。

---

## 6.5 静音检测:每秒结算的统计量

```cpp
// updateSilenceStats() 每秒执行:
double variance = acc_sumsq_ / acc_samples_ - mean * mean;
double stddev = (variance > 0) ? sqrt(variance) : 0.0;
silent_ = (stddev < 100.0);   // ≈ -50dBFS 以下视为静音
```

- 累加器每秒清零重算:内存 O(1),不用存 44100 个样本
- 结果存 atomic `silent_`——ASR 线程跨模块读它做静音门控(第 4 章的原子变量实战)
- 静音判定的滞后(~1s)与 ASR 门控的配合问题,是第 12 章踩坑实录的素材

---

## 6.6 shutdown:再看一次"停线程三部曲"

```cpp
void AudioCapture::shutdown() {
    if (!running_.load()) return;   // 幂等:重复调用无害
    running_ = false;               // ① 标志
    if (pcm_) snd_pcm_drop(pcm_);   // ② 打断阻塞的 readi
    if (thread_.joinable()) thread_.join();   // ③ 收尸
}
```

`joinable()` 检查防止"没启动就 shutdown"或"重复 join"崩溃——**join 一个空 thread 是 C++ 的未定义行为**,防护必须自己写。

---

## 6.7 思考题

1. `addSink` 注释说"须在 init 前设置"。如果改成任意时刻都能 add,需要给 sinks_ 加什么?会引入什么性能代价?
2. 采集线程阻塞在 readi 时,`silent_` 是谁更新的?画出 silent_ 的更新时序(提示:它在 captureLoop 里每块调用)。
3. xrun 时数据丢了多少?为什么音频 xrun 不致命,视频丢帧却要设计成"丢最旧"?
4. 如果 `snd_pcm_recover` 每次都失败,循环里 sleep 100ms——这个 sleep 在锁内吗?为什么无所谓?

---

## 一句话总结

AudioCapture 是项目的"最小完整类":清晰的公开接口、错误分级恢复、RAII 式清理、多路分发解耦。把它的每一行读懂,读更大模块(ASR/ChatEngine)时就能"识别出老朋友"。
