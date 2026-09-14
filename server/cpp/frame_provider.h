// frame_provider.h
// 按需零拷贝快照通道：巡检/远程快照/远程问答三方共用的抓帧接口。
//
// 线程模型：
//   offer():    采集线程每帧绘制完成后调用（draw_mem 仍是有效 RGB888 时）
//   reportPersons(): AI 线程每帧推理后调用（原子写）
//   request():  chat 线程（唯一请求者；巡检/远程命令都在 chat 线程执行）
//
// 设计关键——按需转换：仅当有请求在等（want_）时才做一次 RGB→BGR 转换+clone
// 落槽；平时 offer 早退零开销（30fps 全量转换+拷贝约 60-90ms/s CPU，不可取）。
// 槽位 Mat 在采集线程内 clone 完毕才落槽，杜绝跨线程访问采集缓冲
// （采集缓冲下一帧即被 RGA 覆盖）。
#ifndef FRAME_PROVIDER_H
#define FRAME_PROVIDER_H

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <cstdint>

#include <opencv2/opencv.hpp>

class FrameProvider {
public:
    // 采集线程：干净 RGB888 帧视图（画框前快照，不带检测标注——标注会干扰
    // VLM 姿态判断；virt_addr 包装不持有数据）。仅被请求时转换落槽
    void offer(const cv::Mat &rgb888);

    // AI 线程：每帧推理后上报画面人数（原子写，零成本）
    void reportPersons(int n) { persons_.store(n, std::memory_order_relaxed); }
    int lastPersons() const { return persons_.load(std::memory_order_relaxed); }

    // chat 线程：请求一帧 BGR 快照（置 want_ 后等 seq_ 变化，请求到帧 ≤1 帧 ~33ms）。
    // 返回空 Mat = 超时/无帧。单请求者模型（巡检/远程命令同在 chat 线程串行）
    cv::Mat request(int timeout_ms);

    // 是否有请求在等（采集线程"无人不绘图"路径外检查——保证无人时快照也能拍空房间）
    bool wanted() const { return want_.load(std::memory_order_relaxed); }

private:
    std::mutex mtx_;
    std::condition_variable cv_;
    cv::Mat slot_;                // 采集线程转换+clone 后落槽
    uint64_t seq_ = 0;            // 槽位更新序号（request 以此判定新帧到达）
    std::atomic<bool> want_{false};
    std::atomic<int> persons_{0};
};

// 全局实例（g_chat_engine 同款 extern 模式；静态全局，退出顺序见 TLmain）
extern FrameProvider g_frame_provider;

#endif // FRAME_PROVIDER_H
