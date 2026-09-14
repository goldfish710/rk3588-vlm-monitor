// frame_provider.cpp
// 按需零拷贝快照通道实现（线程模型见头文件注释）
#include "frame_provider.h"

FrameProvider g_frame_provider;

void FrameProvider::offer(const cv::Mat &rgb888)
{
    if (!want_.load(std::memory_order_relaxed))
        return;   // 无请求：零转换零拷贝（平时 30fps 全量走这条早退）

    cv::Mat bgr;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        // cvtColor 输出为独立新缓冲（采集缓冲下一帧即被 RGA 覆盖，不能持有视图）
        cv::cvtColor(rgb888, bgr, cv::COLOR_RGB2BGR);
        slot_ = bgr;
        seq_++;
    }
    cv_.notify_one();
}

cv::Mat FrameProvider::request(int timeout_ms)
{
    cv::Mat out;
    std::unique_lock<std::mutex> lock(mtx_);
    want_ = true;
    uint64_t base = seq_;
    cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                 [this, base] { return seq_ != base; });
    if (seq_ != base && !slot_.empty())
        out = slot_.clone();   // 拷贝交给调用者（chat 线程），槽位归采集线程复用
    want_ = false;
    return out;
}
