# 10. 项目精讲:视频编码管线 EncodeThread——项目最复杂的线程协作

> 目标:读懂 EncodeThread.cpp(约 900 行,项目最大文件)。它同时管:
> V4L2 采集、MPP 硬编码、AI 推理、跌倒检测、事件上报、绘图。
> 学完这章,你对"多线程嵌入式程序怎么组织"就有完整图景。

---

## 10.1 文件结构:先看全局再看局部

```
EncodeThread.cpp
├─ 子码流主循环 start_encode_h264()      ← 最大函数,采集/编码/AI 都在这
│   ├─ AI 推理线程(独立 std::thread)
│   │   └─ 等待队列 → rknn 推理 → 追踪器/姿态分类 → 置 pending_fall
│   ├─ 事件处理线程(独立 std::thread)
│   │   └─ 等事件队列 → 存图/录像/MQTT/ChatEngine 通知
│   └─ 主循环:采集 → 旋转送AI → 绘图 → 编码 → 推流
├─ 主码流 start_encode_h265()           ← 纯编码,无 AI
├─ 绘制工具(draw_skeleton 等)
└─ 全局状态(exit_flag、pending_fall、队列)
```

**读大文件的策略**:先画线程图,再逐线程读。直接从头啃会迷路。

---

## 10.2 静态局部变量:文件内的"私有全局"

```cpp
// EncodeThread.cpp 开头
std::atomic<bool> exit_flag(false);           // 全局退出
std::atomic<bool> g_h265_thread_alive;        // 线程存活标志
static std::deque<int> ai_queue;              // static:仅本文件可见!
static std::mutex ai_mtx;
static std::condition_variable ai_cv;
```

C 里 static 全局变量 = 文件作用域;C++ 里语义相同。**为什么用文件级 static 而不是类成员**:这些状态本来就不属于任何对象——它们是"这个编译单元的私有状态"。对比 config.h 的全局 g_config(跨文件),这里刻意收窄可见性。**可见性收窄是防 bug 的第一手段**:跨文件谁都能碰的全局变量,出问题时排查范围是全项目。

---

## 10.3 主循环的帧流水线(子码流为例)

```cpp
while (1) {
    if (exit_flag) break;
    int index = v4l2.Dqbuf();               // ① 从摄像头取一帧(阻塞)
    if (index == -1) continue;
    cnt_capture++;

    MppBuffer mpp_buf = mpp.GetMppBuffer(index);
    bool is_ai_frame = (frame_counter % ai.frame_interval == 0);   // ② 抽帧送 AI(每2帧1次)

    if (is_ai_frame) { /* ③ RGA 旋转 → 送 AI 线程队列 */ }

    std::vector<TrackedPerson> draw_persons = person_tracker.getPersons();
    bool has_draw = !draw_persons.empty();
    if (has_draw) { /* ④ RGA 旋转 → RGB → 画框画骨架 → 转回 */ }

    if (pending_fall.load()) { /* ⑤ 跌倒事件 → 事件线程队列 */ }

    mpp.mpi->encode_put_frame(...);         // ⑥ 硬件编码
    mpp.mpi->encode_get_packet(...);
    shared_queue->push_packet(enc_pkt);     // ⑦ 推流队列
    g_mainRecorder.onVideoPacket(enc_pkt);  // ⑧ 录像
    v4l2.Qbuf(...);                         // ⑨ 帧还给摄像头(循环缓冲)
}
```

### 这条流水线体现了什么

1. **零拷贝思想贯穿**:V4L2 的 buffer 直接给 MPP 编码(同一块 DMA 内存),AI 用 RGA 硬件旋转不落 CPU——C 里你写 memcpy 的地方,这里全是"传 fd"(文件描述符=内存句柄)
2. **frame_counter % 2 抽帧**:AI 跑 15fps 而视频 30fps——推理贵,检测不需要每帧做。**按需降频是嵌入式 AI 的基本功**
3. **⑨ Qbuf 还帧**:漏还一次,循环缓冲就少一块,几十秒后采集饿死。资源借还的闭环是硬件编程的生命线

---

## 10.4 AI 线程:独立的推理循环

```cpp
std::thread ai_thread([&]() {
    while (!exit_flag) {
        int ai_idx = -1;
        {
            std::unique_lock<std::mutex> lock(ai_mtx);
            ai_cv.wait(lock, [&] { return !ai_queue.empty() || exit_flag.load(); });
            if (exit_flag.load() && ai_queue.empty()) break;
            ai_idx = ai_queue.front();
            ai_queue.pop_front();
        }
        // 推理 + 后处理 + 追踪 + 姿态分类(几十 ms,全在锁外)
        // 跌倒确认 → pending_fall 置位
    }
});
```

和第 4 章 ASR 线程对比——**完全相同的骨架**:条件变量+队列+锁外重活。项目里所有线程都是这个模板,读一个就全会了。

**为什么 AI 不在主循环里内联跑**:推理 48ms,内联会让视频帧率从 30 掉到 20。独立线程后,主循环只"扔帧"不等待——但注意**扔帧用非阻塞投递**:

```cpp
{
    std::lock_guard<std::mutex> lock(ai_mtx);
    if (!free_ai_bufs.empty()) {          // 有空闲旋转缓冲才投
        ai_idx = free_ai_bufs.front();
        free_ai_bufs.pop_front();
    }
}
if (ai_idx >= 0) { /* 投递 */ }
// 没有空闲缓冲?这一帧就不送 AI(丢帧保实时,老策略)
```

---

## 10.5 跨线程状态:atomic 的 pending_fall

```cpp
static std::atomic<bool> pending_fall{false};       // AI 线程写
static std::atomic<float> pending_fall_conf{0.0f};  // 主循环读

// AI 线程:
pending_fall_conf.store(fall_events[0].confidence);
pending_fall.store(true);

// 主循环:
if (pending_fall.load()) {
    pending_fall.store(false);       // 消费掉(clear)
    ...构造事件入事件队列...
}
```

**单槽事件**:AI 线程只置位,主循环只清位——两个 atomic,零锁。多线程通知最简单可靠的形式。局限是"只有一位"——如果连续两次跌倒,第二次可能被覆盖,所以这个信号只用于"触发一次处理",真正的数据(图像)走事件队列。

---

## 10.6 事件线程:跌倒的善后工作

```cpp
std::thread event_thread([&]() {
    while (true) {
        AIEventData ev_data;
        {
            std::unique_lock<std::mutex> lock(event_mtx);
            event_cv.wait(lock, [&] { return !event_queue.empty() || exit_flag.load(); });
            ...
            ev_data = std::move(event_queue.front());
            event_queue.pop_front();
        }
        // 锁外重活:保存全尺寸图 → 触发录像切片 → 存 SQLite
        // → 缩略图 → MQTT 上报 → ChatEngine 通知(跌倒→LLM)
        mqtt_push_event(mqtt_ev);
        if (g_chat_engine) g_chat_engine->onFallEvent();
    }
});
```

**为什么善后要独立线程**:存图(JPEG 编码几百 ms)+ 数据库写入,都算重活。放主循环会让视频卡顿。**"重活下沉"是架构主线**:主循环只管实时链路,慢活全部旁路。

`g_chat_engine->onFallEvent()` 是跨模块回调——注意它仍然遵守纪律:**只入队**(onFallEvent 内部就是 push 事件+notify)。

---

## 10.7 零拷贝的硬件编程模式:fd 即内存

```cpp
// 旋转:摄像头帧 → AI 方向(硬件 RGA,CPU 零参与)
rga_rotate_fd(mpp_buf_fd, orig_w, orig_h,
              ai_rotate_mems[ai_idx]->fd, rot_w, rot_h,
              IM_HAL_TRANSFORM_ROT_270);
```

**fd = 文件描述符 = DMA 缓冲区的"远程句柄"**:RGA 硬件通过 fd 直接访问内存,CPU 只传一个 int。C 里你习惯 `memcpy(dst, src, size)`,嵌入式 C++ 里是 `hardware_op(src_fd, dst_fd)`——**数据从不过 CPU**。你的整个项目就是围绕这个思想建的(V4L2 DMABUF → MPP → RGA → RKNN 全链路 fd)。

---

## 10.8 思考题

1. AI 线程推理 48ms/帧,主循环 30fps 每 2 帧投 1 次(15fps)。free_ai_bufs 需要多大才能不丢 AI 帧?数一下 ai_rotate_mems 的数量,推理:缓冲池为什么是那个数。
2. pending_fall 单槽被覆盖的概率多大?如果跌倒确认很密(视频里反复跌倒),会发生什么?怎么改成多槽?
3. 主循环里 has_draw 为 false 时,画管线整体跳过——这是哪个性能优化的例子?(提示:对比"每帧都转 RGB 画空气")
4. 事件线程的 move(ev_data):图像是 cv::Mat,移动 vs 拷贝差多少内存?为什么这里必须 move?

---

## 一句话总结

EncodeThread = 实时主循环(采集→送AI→绘图→编码→还帧)+ 两个旁路线程(AI 推理、事件善后)+ fd 零拷贝硬件管线。读它的诀窍:先画线程图,再找"重活下沉"和"资源借还"两对关系。
