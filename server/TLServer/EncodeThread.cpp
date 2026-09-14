// EncodeThread.cpp（配置化 + 事件录像归入主码流）
#include "EncodeThread.h"
#include "v4l2.h"
#include "mpp.h"
#include "yolov8-pose.h"
#include "image_utils.h"
#include "image_drawing.h"
#include <opencv2/opencv.hpp>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <cmath>
#include <vector>
#include <deque>
#include <condition_variable>
#include <future>
#include <sys/time.h>
#include "pose_classifier.h"
#include "person_tracker.h"
#include "mqtt_thread.h"
#include "EventManager.h"
#include "chat_engine.h"
#include "frame_provider.h"
#include "Logger.h"
#include "VideoRecorder.h"
#include "config.h"
#include "thread_util.h"
#include <sys/syscall.h>   // SYS_gettid
#include <sys/resource.h>  // setpriority

// ==================== RGA ====================
#include "im2d.h"
#include "RgaUtils.h"
#include "rga.h"
#include <linux/dma-buf.h>   // DMA_BUF_IOCTL_SYNC（CPU↔RGA 缓存同步）

// ==================== 线程存活 RAII 标志 ====================
struct ThreadAliveGuard {
    std::atomic<bool>& flag;
    explicit ThreadAliveGuard(std::atomic<bool>& f) : flag(f) {
        flag.store(true);
    }
    ~ThreadAliveGuard() {
        flag.store(false);
    }
};
// ==================== 性能统计结构体 ====================
struct PerfStats {
    int capture_fps = 0;      // 采集帧率
    int encode_fps = 0;       // 编码帧率
    int ai_fps = 0;           // AI实际推理帧率
    double avg_infer_ms = 0;  // 单帧推理+后处理平均耗时(ms)
    size_t queue_size = 0;    // 共享队列当前长度
    int person_count = 0;     // 当前检测人数
};

// 工具函数：获取当前时间(ms)
static int64_t current_time_ms() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

// 工具函数：获取带毫秒的时间字符串（线程安全）
static void get_time_str_ms(char *buf, size_t bufsz) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm tmv;
    localtime_r(&tv.tv_sec, &tmv);
    char tmp[20];
    strftime(tmp, sizeof(tmp), "%H:%M:%S", &tmv);
    snprintf(buf, bufsz, "%s.%03ld", tmp, tv.tv_usec / 1000);
}

// ==================== RGA 辅助函数 ====================
// 瞬态失败重试：启动期 NPU 驱动初始化（VLM 2.4GB 加载/piper 解码器）与 RGA
// 映射竞争会出现零星 "Failed to map attachment ret[-5]"（实测启动 3 次后自愈）。
// 重试 3 次（间隔 2ms）吞掉瞬态抖动；持续失败仍按失败返回并打日志
static const int kRgaRetry = 3;

static int rga_rotate_fd(int src_fd, int src_w, int src_h,
                         int dst_fd, int dst_w, int dst_h,
                         int rotation)
{
    rga_buffer_t src_buf = wrapbuffer_fd(src_fd, src_w, src_h, RK_FORMAT_YCbCr_420_SP);
    rga_buffer_t dst_buf = wrapbuffer_fd(dst_fd, dst_w, dst_h, RK_FORMAT_YCbCr_420_SP);
    for (int i = 0; i < kRgaRetry; i++) {
        IM_STATUS st = imrotate(src_buf, dst_buf, rotation);
        if (st == IM_STATUS_SUCCESS) return 0;
        usleep(2000);
    }
    return -1;
}

static int rga_letterbox_fd(int src_fd, int src_w, int src_h,
                            int dst_fd, int dst_w, int dst_h,
                            letterbox_t *lb)
{
    float scale = std::min((float)dst_w / src_w, (float)dst_h / src_h);
    int new_w = (int)(src_w * scale);
    int new_h = (int)(src_h * scale);
    int dx = (dst_w - new_w) / 2;
    int dy = (dst_h - new_h) / 2;

    lb->scale = scale;
    lb->x_pad = dx;
    lb->y_pad = dy;

    rga_buffer_t src_buf = wrapbuffer_fd(src_fd, src_w, src_h, RK_FORMAT_YCbCr_420_SP);
    rga_buffer_t dst_buf = wrapbuffer_fd(dst_fd, dst_w, dst_h, RK_FORMAT_RGB_888);

    im_rect src_rect = {0, 0, src_w, src_h};
    im_rect dst_rect = {dx, dy, new_w, new_h};

    for (int i = 0; i < kRgaRetry; i++) {
        IM_STATUS st = improcess(src_buf, dst_buf, {}, src_rect, dst_rect, {}, IM_SYNC);
        if (st == IM_STATUS_SUCCESS) return 0;
        usleep(2000);
    }
    return -1;
}

static int rga_convert_fd(int src_fd, int src_w, int src_h, int src_fmt,
                          int dst_fd, int dst_w, int dst_h, int dst_fmt)
{
    rga_buffer_t src_buf = wrapbuffer_fd(src_fd, src_w, src_h, src_fmt);
    rga_buffer_t dst_buf = wrapbuffer_fd(dst_fd, dst_w, dst_h, dst_fmt);

    im_rect src_rect = {0, 0, src_w, src_h};
    im_rect dst_rect = {0, 0, dst_w, dst_h};

    for (int i = 0; i < kRgaRetry; i++) {
        IM_STATUS st = improcess(src_buf, dst_buf, {}, src_rect, dst_rect, {}, IM_SYNC);
        if (st == IM_STATUS_SUCCESS) return 0;
        usleep(2000);
    }
    return -1;
}

// NV12 → NV12 缩放（主码流 4K 采集 → 编码尺寸：全视野+小带宽）
static int rga_scale_fd(int src_fd, int src_w, int src_h, int dst_fd, int dst_w, int dst_h)
{
    rga_buffer_t src_buf = wrapbuffer_fd(src_fd, src_w, src_h, RK_FORMAT_YCbCr_420_SP);
    rga_buffer_t dst_buf = wrapbuffer_fd(dst_fd, dst_w, dst_h, RK_FORMAT_YCbCr_420_SP);
    im_rect src_rect = {0, 0, src_w, src_h};
    im_rect dst_rect = {0, 0, dst_w, dst_h};
    for (int i = 0; i < kRgaRetry; i++) {
        IM_STATUS st = improcess(src_buf, dst_buf, {}, src_rect, dst_rect, {}, IM_SYNC);
        if (st == IM_STATUS_SUCCESS) return 0;
        usleep(2000);
    }
    return -1;
}

// ==================== dma-buf 缓存同步（CPU ↔ RGA 交接） ====================
// CMA fd 的 mmap 默认带缓存：CPU 画框写进缓存行、RGA(DMA) 读到旧数据 = 框闪烁/拖影；
// dma-heap 没有"非缓存 mmap"接口，标准做法是在 CPU 访问前后用
// DMA_BUF_IOCTL_SYNC 括起来（dma-buf 生态官方机制）
static void dma_buf_sync_cpu_start(int fd)   // CPU 读前：失效缓存（RGA 刚写，CPU 要读）
{
    struct dma_buf_sync sync = {DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ};
    ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
}
static void dma_buf_sync_cpu_end(int fd)     // CPU 写后：刷出缓存（RGA 要读 CPU 刚画）
{
    struct dma_buf_sync sync = {DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE};
    ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
}

// A/B 对照实验：模拟非零拷贝管线的全帧 CPU 拷贝（仅测试用；开启时画面会花，
// 只看统计行的 LatAvg/CPU 变化）。打点无需新增——t_frame_start 在 Dqbuf 后
// 立即记录、终点在编码完成，插入的拷贝天然落在窗口内，LatAvg 直接反映增量
static void simulate_extra_copies(void *buf, size_t size, int n)
{
    if (n <= 0 || !buf) return;
    static void *tmp = nullptr;
    if (!tmp) {
        tmp = malloc(3840 * 2160 * 3 / 2);   // 4K NV12 上限（12.4MB）
        if (!tmp) return;
    }
    for (int i = 0; i < n; i++) {
        if (i % 2 == 0) memcpy(tmp, buf, size);
        else            memcpy(buf, tmp, size);
    }
    if (n % 2 == 1) memcpy(buf, tmp, size);   // 奇数时数据回到原缓冲（成本同计）
}

// ==================== 主码流 H.265 ====================
void start_encode_h265(SharedQueue *shared_queue, TaskScheduler *scheduler)
{
    ThreadAliveGuard alive_guard(g_h265_thread_alive);
    pthread_setname_np(pthread_self(), "h265_enc");
    bind_rt_thread();
    setpriority(PRIO_PROCESS, (pid_t)syscall(SYS_gettid), -5);
    V4l2class v4l2;
    Mppclass mpp;

    const MainStreamConfig& ms = g_config.main_stream;

    // 采集尺寸固定为传感器原生 4K（全视野）：正点 BSP 的 rkisp mainpath 只有
    // 1080p 与 4K 输出正常（1920x1440/2560x1440 实测"两个对称画面"），
    // 1080p 是 1:1 裁切视野小 → 4K 采集 + RGA 缩放到编码尺寸 = 全视野 + 小带宽
    const int kCapW = 3840, kCapH = 2160;
    const int kCapFrameSize = kCapW * kCapH * 3 / 2;
    const bool need_scale = (ms.width != kCapW || ms.height != kCapH);

    if (!v4l2.InitV4l2(ms.video_device.c_str(), kCapW, kCapH, kCapFrameSize, ms.buffer_count))
    {
        std::cout << "主码流 V4L2 初始化失败" << std::endl;
        return;
    }

    if (!mpp.InitMpp(MPP_VIDEO_CodingHEVC, ms.width, ms.height,
                     ms.hor_stride, ms.ver_stride, ms.frame_size,
                     ms.bitrate, ms.bitrate_max, ms.bitrate_min,
                     ms.fps, ms.gop, ms.buffer_count, shared_queue,
                     kCapFrameSize,    // 缓冲池按 4K 分配（Qbuf 进 V4L2 需匹配采集格式）
                     ms.rotate))       // 编码器旋转（转正竖屏输出）
    {
        std::cout << "主码流 MPP 初始化失败" << std::endl;
        return;
    }

    // 缩放目标缓冲（双缓冲 ping-pong：MPP 编码管线有 1-2 帧延迟，
    // 单缓冲会在下一帧 RGA 写入时覆盖 MPP 尚未读完的数据）
    // 同样用 CMA 连续内存（RGA 直接映射，避免 swiotlb 弹跳——见 mpp_alloc_cma_fd 注释）
    MppBuffer scale_bufs[2] = {nullptr, nullptr};
    if (need_scale)
    {
        size_t enc_buf_size = (size_t)ms.hor_stride * ms.ver_stride * 3 / 2;
        for (int i = 0; i < 2; i++)
        {
            int fd = mpp_alloc_cma_fd(enc_buf_size);
            if (fd < 0)
            {
                std::cout << "主码流缩放缓冲分配失败" << std::endl;
                return;
            }
            MppBufferInfo info = {};
            info.type = MPP_BUFFER_TYPE_EXT_DMA;
            info.fd = fd;
            info.size = enc_buf_size;
            if (mpp_buffer_import(&scale_bufs[i], &info) != MPP_OK)
            {
                std::cout << "主码流缩放缓冲导入失败" << std::endl;
                close(fd);
                return;
            }
            // 清零到中性灰（与 Mppclass 缓冲初始化同款）：RGA 只写 height 行，
            // 而 MPP 按 ver_stride(16 对齐)读取——padding 行若是残留垃圾，
            // 编码画面底部会出现彩色线（实测"底部绿线"根因）
            void *ptr = mpp_buffer_get_ptr(scale_bufs[i]);
            memset(ptr, 0x00, (size_t)ms.hor_stride * ms.ver_stride);
            memset((uint8_t *)ptr + (size_t)ms.hor_stride * ms.ver_stride, 0x80,
                   (size_t)ms.hor_stride * ms.ver_stride / 2);
        }
    }

    for (int i = 0; i < ms.buffer_count; i++)
    {
        v4l2.Qbuf(mpp.GetMppBufferFd(i), i);
    }
    v4l2.StreamOn();

    // ================== 性能监测变量 ==================
    PerfStats stats;
    int cnt_capture = 0;
    int cnt_encode = 0;
    int64_t last_stat_time = current_time_ms();

    double sum_latency_ms = 0.0;
    int    lat_samples = 0;
    double max_latency_ms = 0.0;
    int64_t t_frame_start = 0;

    std::cout << "主码流 H.265 编码线程启动（统计输出到控制台 + 延时打点）" << std::endl;
#if ENABLE_VIDEO_RECORDING
    // 把编码器头信息注入录像模块，保证切片文件可独立播放
    g_mainRecorder.setH265Headers(shared_queue->GetVPS(),
                                  shared_queue->GetSPS(),
                                  shared_queue->GetPPS());
#endif
    while (1)
    {
        if (exit_flag)
            break;

        int index = v4l2.Dqbuf();
        if (index == -1) continue;
        cnt_capture++;
        t_frame_start = current_time_ms();

        MppBuffer mpp_buf = mpp.GetMppBuffer(index);
        if (ms.extra_copies > 0)
            simulate_extra_copies(mpp_buffer_get_ptr(mpp_buf), kCapFrameSize, ms.extra_copies);

        // ---------- 每秒统计并打印 ----------
        int64_t now = current_time_ms();
        if (now - last_stat_time >= 1000) {
            stats.capture_fps = cnt_capture;
            stats.encode_fps = cnt_encode;
            stats.queue_size = shared_queue->size();

            double avg_lat = (lat_samples > 0) ? (sum_latency_ms / lat_samples) : 0.0;
            char ts[32];
            get_time_str_ms(ts, sizeof(ts));
            printf("[MAIN %s] Cap=%d fps | Enc=%d fps | Queue=%zu | LatAvg=%.1fms LatMax=%.1fms\n",
                   ts, stats.capture_fps, stats.encode_fps, stats.queue_size,
                   avg_lat, max_latency_ms);

            cnt_capture = 0;
            cnt_encode = 0;
            sum_latency_ms = 0.0;
            lat_samples = 0;
            max_latency_ms = 0.0;
            last_stat_time = now;
        }

        // ---------- RGA 缩放（4K 采集 → 编码尺寸，全视野） ----------
        MppBuffer enc_buf = mpp_buf;
        if (need_scale)
        {
            MppBuffer dst = scale_bufs[cnt_capture % 2];
            int ret = rga_scale_fd(mpp_buffer_get_fd(mpp_buf), kCapW, kCapH,
                                   mpp_buffer_get_fd(dst), ms.width, ms.height);
            if (ret < 0)
            {
                std::cerr << "主码流 RGA 缩放失败" << std::endl;
                v4l2.Qbuf(mpp.GetMppBufferFd(index), index);
                continue;
            }
            enc_buf = dst;
        }

        // ---------- MPP 编码 ----------
        MppFrame frame = NULL;
        mpp_frame_init(&frame);
        mpp_frame_set_width(frame, ms.width);
        mpp_frame_set_height(frame, ms.height);
        mpp_frame_set_hor_stride(frame, ms.hor_stride);
        mpp_frame_set_ver_stride(frame, ms.ver_stride);
        mpp_frame_set_fmt(frame, MPP_FMT_YUV420SP);
        mpp_frame_set_buffer(frame, enc_buf);
        mpp_frame_set_eos(frame, 0);

        if (mpp.mpi->encode_put_frame(mpp.ctx, frame) != MPP_SUCCESS)
        {
            std::cout << "主码流编码失败" << std::endl;
            mpp_frame_deinit(&frame);
            v4l2.Qbuf(mpp.GetMppBufferFd(index), index);
            continue;
        }

        MppPacket packet = NULL;
        if (mpp.mpi->encode_get_packet(mpp.ctx, &packet) != MPP_SUCCESS)
        {
            std::cout << "主码流获取packet失败" << std::endl;
            mpp_frame_deinit(&frame);
            v4l2.Qbuf(mpp.GetMppBufferFd(index), index);
            continue;
        }

        H265EncodePacket enc_pkt;
        void *p = mpp_packet_get_data(packet);
        size_t len = mpp_packet_get_length(packet);
        enc_pkt.data.assign(static_cast<uint8_t*>(p), static_cast<uint8_t*>(p) + len);
        gettimeofday(&enc_pkt.timestamp, NULL);
        RK_U32 mpp_flags = mpp_packet_get_flag(packet);
        enc_pkt.is_idr = (mpp_flags & 0x01);

        int64_t t_encoded = current_time_ms();
        double frame_latency = t_encoded - t_frame_start;
        sum_latency_ms += frame_latency;
        lat_samples++;
        if (frame_latency > max_latency_ms) max_latency_ms = frame_latency;

        shared_queue->push_packet(enc_pkt);
        cnt_encode++;
#if ENABLE_VIDEO_RECORDING
        // ========== 旁路: 主码流循环录像（含事件预录缓冲） ==========
        g_mainRecorder.onVideoPacket(enc_pkt);
#endif
        if(shared_queue->GetMppEventID()!=0 && shared_queue->GetSource() != NULL)
        {
            scheduler->triggerEvent(shared_queue->GetMppEventID(), shared_queue->GetSource());
        }

        v4l2.Qbuf(mpp.GetMppBufferFd(index), index);
        mpp_frame_deinit(&frame);
        mpp_packet_deinit(&packet);
    }

    std::cout << "主码流线程退出" << std::endl;
}

// ==================== 骨架连线定义（保持不变） ====================
static const int SKELETON_PAIRS[19][2] = {
    {15, 13}, {13, 11}, {16, 14}, {14, 12}, {11, 12},
    {5, 11},  {6, 12},  {5, 6},   {5, 7},   {6, 8},
    {7, 9},   {8, 10},  {1, 2},   {0, 1},   {0, 2},
    {1, 3},   {2, 4},   {3, 5},   {4, 6}
};

static void draw_skeleton(image_buffer_t *image, const object_detect_result &det, unsigned int color, int thickness)
{
    for (int i = 0; i < 19; ++i) {
        int idx1 = SKELETON_PAIRS[i][0];
        int idx2 = SKELETON_PAIRS[i][1];
        float x1 = det.keypoints[idx1][0];
        float y1 = det.keypoints[idx1][1];
        float conf1 = det.keypoints[idx1][2];
        float x2 = det.keypoints[idx2][0];
        float y2 = det.keypoints[idx2][1];
        float conf2 = det.keypoints[idx2][2];
        if (conf1 > 0.25 && conf2 > 0.25) {
            draw_line(image, (int)x1, (int)y1, (int)x2, (int)y2, color, thickness);
        }
    }
}

// ==================== 子码流（独立AI线程 + 事件线程 + 无检测跳过绘图） ====================
void start_encode_h264(SharedQueue *shared_queue, TaskScheduler *scheduler, rknn_app_context_t *app_ctx)
{
    ThreadAliveGuard alive_guard(g_h264_thread_alive);
    pthread_setname_np(pthread_self(), "h264_ai_enc");
    bind_rt_thread();
    setpriority(PRIO_PROCESS, (pid_t)syscall(SYS_gettid), -5);

    V4l2class v4l2;
    Mppclass mpp;

    const SubStreamConfig& ss = g_config.sub_stream;
    const AiConfig& ai = g_config.ai;

    if (!v4l2.InitV4l2(ss.video_device.c_str(), ss.width, ss.height, ss.frame_size, ss.buffer_count))
    {
        std::cout << "子码流 V4L2 初始化失败" << std::endl;
        return;
    }

    if (!mpp.InitMpp(MPP_VIDEO_CodingAVC, ss.width, ss.height,
                     ss.hor_stride, ss.ver_stride, ss.frame_size,
                     ss.bitrate, ss.bitrate_max, ss.bitrate_min,
                     ss.fps, ss.gop, ss.buffer_count, shared_queue,
                     -1,               // 缓冲尺寸默认（=frame_size）
                     ss.rotate))       // 编码器旋转（AI 框随画面一起转正）
    {
        std::cout << "子码流 MPP 初始化失败" << std::endl;
        return;
    }

    for (int i = 0; i < ss.buffer_count; i++)
    {
        v4l2.Qbuf(mpp.GetMppBufferFd(i), i);
    }
    v4l2.StreamOn();

    // ==================== 零拷贝 RKNN + RGA 缓冲区初始化 ====================
    const int orig_w = ss.width;
    const int orig_h = ss.height;
    const int rot_w  = orig_h;
    const int rot_h  = orig_w;
    const int model_w = app_ctx->model_width;
    const int model_h = app_ctx->model_height;
    const int model_c = app_ctx->model_channel;

    const int rot_nv12_size = rot_w * rot_h * 3 / 2;
    const int rot_rgb_size  = rot_w * rot_h * 3;

    // ★子码流 RGA 缓冲全 CMA 化（old28 定稿，实测零 RGA 失败）★：
    // 实验结论（三次对照）：4K 主码流运行条件下 rknn_create_mem（NPU 驱动导出
    // dmabuf）进 RGA 会周期性 "Failed to map attachment" 失败，CMA 堆缓冲从不
    // 失败（映射子系统承压差异）。CPU 访问用 DMA_BUF_IOCTL_SYNC 括起来
    // （dma-heap mmap 带缓存，画框前后必须同步，否则框闪烁/拖影）
    rknn_tensor_mem *input_mem = nullptr;          // CMA fd 包裹（RGA letterbox 写入）
    std::vector<rknn_tensor_mem*> output_mems;
    int draw_rotate_fd = -1, draw_mem_fd = -1, rotate_back_fd = -1;
    void *draw_mem_ptr = nullptr;                  // draw_mem 的 CPU 映射（画框/快照用）
    int input_cma_fd = -1;
    std::vector<int> ai_rotate_fds;

    auto cleanup_resources = [&]() {
        if (input_mem)        rknn_destroy_mem(app_ctx->rknn_ctx, input_mem);
        for (auto *m : output_mems) if (m) rknn_destroy_mem(app_ctx->rknn_ctx, m);
        if (draw_mem_ptr)     munmap(draw_mem_ptr, rot_rgb_size);
        if (draw_rotate_fd >= 0) close(draw_rotate_fd);
        if (draw_mem_fd >= 0)    close(draw_mem_fd);
        if (rotate_back_fd >= 0) close(rotate_back_fd);
        if (input_cma_fd >= 0)   close(input_cma_fd);
        for (int fd : ai_rotate_fds) if (fd >= 0) close(fd);
    };

    // ---- RKNN 输入（CMA 物理连续 + 包装成 rknn mem 供 set_io_mem） ----
    rknn_tensor_attr input_attr = app_ctx->input_attrs[0];
    int input_size = input_attr.size_with_stride;
    input_cma_fd = mpp_alloc_cma_fd(input_size);
    if (input_cma_fd < 0)
    {
        std::cerr << "CMA 分配 RKNN 输入失败" << std::endl;
        cleanup_resources();
        return;
    }
    void *input_ptr = mmap(nullptr, input_size, PROT_READ | PROT_WRITE, MAP_SHARED,
                           input_cma_fd, 0);
    if (input_ptr == MAP_FAILED)
    {
        std::cerr << "RKNN 输入 mmap 失败" << std::endl;
        cleanup_resources();
        return;
    }
    input_mem = rknn_create_mem_from_fd(app_ctx->rknn_ctx, input_cma_fd, input_ptr,
                                        (uint32_t)input_size, 0);
    if (!input_mem)
    {
        std::cerr << "rknn_create_mem_from_fd 输入失败" << std::endl;
        cleanup_resources();
        return;
    }
    memset(input_ptr, 114, input_size);

    // ---- RKNN 输出（NPU 写 + CPU 读，RGA 不接触 → 保留 rknn 内存） ----
    output_mems.resize(app_ctx->io_num.n_output, nullptr);
    for (int i = 0; i < (int)app_ctx->io_num.n_output; i++)
    {
        output_mems[i] = rknn_create_mem(app_ctx->rknn_ctx, app_ctx->output_attrs[i].size);
        if (!output_mems[i])
        {
            std::cerr << "rknn_create_mem 输出 " << i << " 失败" << std::endl;
            cleanup_resources();
            return;
        }
    }

    // ---- 绘图用缓冲区（CMA：CPU 画框 + RGA 转换，dma_buf_sync 保证一致性） ----
    draw_rotate_fd = mpp_alloc_cma_fd(rot_nv12_size);
    draw_mem_fd    = mpp_alloc_cma_fd(rot_rgb_size);
    rotate_back_fd = mpp_alloc_cma_fd(rot_nv12_size);
    if (draw_rotate_fd < 0 || draw_mem_fd < 0 || rotate_back_fd < 0)
    {
        std::cerr << "CMA 绘图中间缓冲区分配失败" << std::endl;
        cleanup_resources();
        return;
    }
    draw_mem_ptr = mmap(nullptr, rot_rgb_size, PROT_READ | PROT_WRITE, MAP_SHARED,
                        draw_mem_fd, 0);
    if (draw_mem_ptr == MAP_FAILED)
    {
        std::cerr << "draw_mem mmap 失败" << std::endl;
        cleanup_resources();
        return;
    }

    // ---- AI 旋转缓冲池（RGA 专用） ----
    constexpr int AI_BUF_COUNT = 3;
    ai_rotate_fds.resize(AI_BUF_COUNT, -1);
    for (int i = 0; i < AI_BUF_COUNT; i++)
    {
        ai_rotate_fds[i] = mpp_alloc_cma_fd(rot_nv12_size);
        if (ai_rotate_fds[i] < 0)
        {
            std::cerr << "CMA AI旋转缓冲 " << i << " 分配失败" << std::endl;
            cleanup_resources();
            return;
        }
    }

    // ---- 注册 RKNN 零拷贝 IO ----
    input_attr.type = RKNN_TENSOR_UINT8;
    input_attr.fmt  = RKNN_TENSOR_NHWC;
    int ret = rknn_set_io_mem(app_ctx->rknn_ctx, input_mem, &input_attr);
    if (ret < 0)
    {
        std::cerr << "rknn_set_io_mem 输入失败 ret=" << ret << std::endl;
        cleanup_resources();
        return;
    }

    for (int i = 0; i < (int)app_ctx->io_num.n_output; i++)
    {
        ret = rknn_set_io_mem(app_ctx->rknn_ctx, output_mems[i], &app_ctx->output_attrs[i]);
        if (ret < 0)
        {
            std::cerr << "rknn_set_io_mem 输出 " << i << " 失败 ret=" << ret << std::endl;
            cleanup_resources();
            return;
        }
    }

    // ---- 预计算 letterbox 参数 ----
    letterbox_t global_lb;
    memset(&global_lb, 0, sizeof(global_lb));
    {
        float scale = std::min((float)model_w / rot_h, (float)model_h / rot_w);
        global_lb.scale = scale;
        global_lb.x_pad = (int)((model_w - rot_h * scale) / 2);
        global_lb.y_pad = (int)((model_h - rot_w * scale) / 2);
    }

    // ==================== 共享数据结构 ====================
    // 多人追踪器：AI 线程 update，主循环 getPersons 绘图（内部加锁）
    PersonTracker person_tracker;

    std::atomic<bool> pending_fall{false};
    std::atomic<float> pending_fall_conf{0.0f};

    struct AIEventData {
        float confidence;
        time_t timestamp;
        cv::Mat image;
    };
    std::deque<AIEventData> event_queue;
    std::mutex event_mtx;
    std::condition_variable event_cv;

    // AI 输入队列与空闲缓冲池
    std::deque<int> ai_queue;
    std::deque<int> free_ai_bufs;
    std::mutex ai_mtx;
    std::condition_variable ai_cv;
    for (int i = 0; i < AI_BUF_COUNT; i++) free_ai_bufs.push_back(i);

    struct AiStats {
        int frames = 0;
        double total_ms = 0.0;
        double total_wait_ms = 0.0;
        int samples = 0;
        int persons = 0;
        // 推理耗时直方图（性能调优 A/B 用，环形 300 样本）
        enum { kHistN = 300 };   // 局部类 C++11 无静态数据成员，用 enum
        double hist[kHistN] = {0};
        int hist_idx = 0;
        int hist_count = 0;
        void addSample(double ms) {
            hist[hist_idx] = ms;
            hist_idx = (hist_idx + 1) % kHistN;
            if (hist_count < kHistN) hist_count++;
        }
        // 拷贝样本排序取分位（每秒一次，300 样本排序开销可忽略）
        void percentile(double *p50_out, double *p95_out) const {
            if (hist_count == 0) { *p50_out = *p95_out = 0.0; return; }
            double tmp[kHistN];
            for (int i = 0; i < hist_count; i++) tmp[i] = hist[i];
            std::sort(tmp, tmp + hist_count);
            *p50_out = tmp[hist_count * 50 / 100];
            *p95_out = tmp[hist_count * 95 / 100];
        }
    };
    AiStats ai_stats;
    std::mutex ai_stats_mtx;
    std::vector<int64_t> ai_push_ms(16, 0);   // 每个 AI 缓冲的入队时间戳（队列等待诊断，ai_mtx 保护）

    // ==================== AI 线程 ====================
    std::thread ai_thread([&]() {
        pthread_setname_np(pthread_self(), "h264_ai");
    bind_rt_thread();
        setpriority(PRIO_PROCESS, (pid_t)syscall(SYS_gettid), -5);

        while (!exit_flag)
        {
            int buf_idx = -1;
            int64_t push_ms = 0;
            {
                std::unique_lock<std::mutex> lock(ai_mtx);
                ai_cv.wait(lock, [&]() { return !ai_queue.empty() || exit_flag.load(); });
                if (exit_flag.load() && ai_queue.empty()) break;
                buf_idx = ai_queue.front();
                ai_queue.pop_front();
                push_ms = ai_push_ms[buf_idx];   // 入队时刻（队列等待诊断）
            }

            if (buf_idx < 0) continue;

            int64_t ai_wait_ms = current_time_ms() - push_ms;   // 队列等待（采集→AI 开始）
            int64_t ai_start = current_time_ms();

            int ret = rga_letterbox_fd(ai_rotate_fds[buf_idx], rot_w, rot_h,
                                       input_mem->fd, model_w, model_h,
                                       &global_lb);
            if (ret < 0)
            {
                std::cerr << "RGA letterbox 失败" << std::endl;
                std::lock_guard<std::mutex> lock(ai_mtx);
                free_ai_bufs.push_back(buf_idx);
                continue;
            }

            ret = rknn_run(app_ctx->rknn_ctx, NULL);
            if (ret < 0)
            {
                std::cerr << "rknn_run 失败 ret=" << ret << std::endl;
                std::lock_guard<std::mutex> lock(ai_mtx);
                free_ai_bufs.push_back(buf_idx);
                continue;
            }

            std::vector<rknn_output> rknn_out(app_ctx->io_num.n_output);
            for (int i = 0; i < (int)app_ctx->io_num.n_output; i++)
            {
                rknn_out[i].index = i;
                rknn_out[i].want_float = (!app_ctx->is_quant);
                rknn_out[i].buf = output_mems[i]->virt_addr;
                rknn_out[i].size = app_ctx->output_attrs[i].size;
            }

            object_detect_result_list od_results;
            memset(&od_results, 0, sizeof(od_results));
            post_process(app_ctx, rknn_out.data(), &global_lb,
                         ai.box_thresh, ai.nms_thresh, &od_results);

            int person_count = 0;
            for (int i = 0; i < od_results.count; i++)
            {
                if (od_results.results[i].cls_id == 0) person_count++;
            }

            // 多人追踪 + 逐人跌倒确认
            // 修正：事件置信度取"跌倒者本人"的检测置信度（原来取循环里最后一个人）
            std::vector<FallEvent> fall_events = person_tracker.update(od_results);
            if (!fall_events.empty())
            {
                pending_fall_conf.store(fall_events[0].confidence);
                pending_fall.store(true);
                for (const FallEvent& fe : fall_events)
                {
                    printf("[AI] 跌倒事件生成: person_id=%d confidence=%.2f\n",
                           fe.person_id, fe.confidence);
                }
            }

            // 人数上报（巡检判定"有人才抓帧"的输入；原子写零成本）
            g_frame_provider.reportPersons(person_count);

            int64_t ai_end = current_time_ms();
            {
                std::lock_guard<std::mutex> lock(ai_stats_mtx);
                ai_stats.frames++;
                ai_stats.total_ms += (ai_end - ai_start);
                ai_stats.total_wait_ms += ai_wait_ms;
                ai_stats.samples++;
                ai_stats.persons = person_count;
                ai_stats.addSample(ai_end - ai_start);
            }

            {
                std::lock_guard<std::mutex> lock(ai_mtx);
                free_ai_bufs.push_back(buf_idx);
            }
        }
    });

    // ==================== 事件处理线程（落盘 + DB + MQTT） ====================
    std::thread event_thread([&]() {
        pthread_setname_np(pthread_self(), "event_proc");
        while (!exit_flag)
        {
            AIEventData ev_data;
            {
                std::unique_lock<std::mutex> lock(event_mtx);
                event_cv.wait(lock, [&]() { return !event_queue.empty() || exit_flag.load(); });
                if (exit_flag.load() && event_queue.empty()) break;
                ev_data = std::move(event_queue.front());
                event_queue.pop_front();
            }

            std::cout << "[AI] 检测到新的跌倒事件" << std::endl;

            // ---------- 现场图编码（一次编码：本地全尺寸 + MQTT 缩略图） ----------
            std::vector<unsigned char> jpeg_full;
            cv::imencode(".jpg", ev_data.image, jpeg_full, {cv::IMWRITE_JPEG_QUALITY, 85});

            AIEvent ev;
            ev.type = "fall";
            ev.device_id = g_config.event.device_id;
            ev.confidence = ev_data.confidence;
            ev.timestamp = ev_data.timestamp;
            ev.image_path = "";
            ev.description = "检测到跌倒";   // 时间线混排显示用（复核意见由 updateEventVerdict 回填 vlm_reply）
#if ENABLE_VIDEO_RECORDING
            // 关键改动：跌倒事件录像改为主码流（H.265，前N秒+后M秒）
            ev.video_path = g_mainRecorder.triggerFallEvent();
#else
            ev.video_path = "";
#endif
            ev.upload_status = 0;

            // 图片同步保存（拿到路径），落库由 EventManager 工作线程异步完成
            // ★证据先行★：图/预录视频/DB 在复核前全部保全，复核失败也不丢证据
            ev.image_path = EventManager::getInstance()->saveEventImageSync(ev, jpeg_full);
            EventManager::getInstance()->recordEvent(ev, cv::Mat());

            // ---------- MQTT 缩略图（长边 320px，控制消息体积） ----------
            cv::Mat thumb;
            float thumb_scale = 320.0f / std::max(ev_data.image.cols, ev_data.image.rows);
            if (thumb_scale < 1.0f)
                cv::resize(ev_data.image, thumb, cv::Size(), thumb_scale, thumb_scale, cv::INTER_AREA);
            else
                thumb = ev_data.image;
            std::vector<unsigned char> jpeg_thumb;
            cv::imencode(".jpg", thumb, jpeg_thumb, {cv::IMWRITE_JPEG_QUALITY, 85});

            MQTTEvent mqtt_ev;
            mqtt_ev.event = "fall";
            mqtt_ev.device_id = g_config.event.device_id;
            mqtt_ev.confidence = ev_data.confidence;
            mqtt_ev.timestamp = ev_data.timestamp;
            mqtt_ev.image = ev.image_path;
            mqtt_ev.image_jpeg = jpeg_thumb;
            mqtt_ev.video = ev.video_path;

            // ---------- VLM 跌倒复核（两级检测：候选图交板端大模型确认） ----------
            // 跌倒静默报警：复核后本地不触发语音安慰（报警接收方是监护人，
            // 语音通道只服务用户主动发起的交互）——按 verdict 分级推送 MQTT
            int verdict = -1;          // -1=复核链路不可用 → 旧行为原样推送（无新字段）
            std::string vlm_reply;
            bool confirm_available = (g_chat_engine != nullptr &&
                                      g_config.chat.enable &&
                                      g_config.chat.vlm.enabled &&
                                      g_config.chat.vlm_pipeline.confirm_enable);
            if (confirm_available) {
                std::future<ChatEngine::FallConfirmResult> fut =
                    g_chat_engine->requestFallConfirm(ev.image_path);
                auto st = fut.wait_for(std::chrono::milliseconds(
                    g_config.chat.vlm_pipeline.confirm_timeout_ms));
                if (st == std::future_status::ready) {
                    ChatEngine::FallConfirmResult r = fut.get();
                    verdict = r.verdict;
                    vlm_reply = r.reply;
                } else {
                    verdict = 2;   // 超时：按未确认直报（宁报勿漏）
                    printf("[AI] 复核超时(%dms)，按未确认直报\n",
                           g_config.chat.vlm_pipeline.confirm_timeout_ms);
                }
            }

            // ---------- 分级映射与报警 ----------
            // severity: urgent=真跌倒 attention=未确认 cleared=VLM 排除
            if (verdict == 1) {
                mqtt_ev.severity = "urgent";
                mqtt_ev.vlm_confirm = 1;
                mqtt_ev.vlm_reply = vlm_reply;
                mqtt_push_event(mqtt_ev);
                printf("[AI] ★真跌倒确认★ urgent 报警已推送\n");
            } else if (verdict == 2) {
                if (g_config.chat.vlm_pipeline.confirm_fallback == "alarm") {
                    mqtt_ev.severity = "attention";
                    mqtt_ev.vlm_confirm = 2;
                    mqtt_ev.vlm_reply = vlm_reply;
                    mqtt_push_event(mqtt_ev);
                    printf("[AI] 复核未确认，attention 报警已推送\n");
                } else {
                    printf("[AI] 复核未确认且 confirm_fallback=none，按配置丢弃\n");
                }
            } else if (verdict == 0) {
                mqtt_ev.vlm_confirm = 0;
                mqtt_ev.vlm_reply = vlm_reply;
                if (g_config.chat.vlm_pipeline.ignore_report) {
                    // 低打扰通知：监护人可见"已复核排除"（双保险，防 VLM 漏判）
                    mqtt_ev.severity = "cleared";
                    mqtt_push_event(mqtt_ev);
                    printf("[AI] VLM 排除（%s），cleared 通知已推送\n", vlm_reply.c_str());
                } else {
                    printf("[AI] VLM 排除（%s），ignore_report=1 不推送\n", vlm_reply.c_str());
                }
            } else {
                // 复核链路不可用（无 VLM/config 关/引擎未运行）：旧行为原样推送
                mqtt_push_event(mqtt_ev);
            }
        }
    });

    // ==================== 性能监测变量 ====================
    PerfStats stats;
    int cnt_capture = 0;
    int cnt_encode = 0;
    int64_t last_stat_time = current_time_ms();

    double sum_latency_ms = 0.0;
    int    lat_samples = 0;
    double max_latency_ms = 0.0;
    int64_t t_frame_start = 0;

    int frame_counter = 0;

    std::cout << "子码流 H.264 编码线程启动（独立AI线程 + 事件线程 + 无检测跳过绘图）" << std::endl;

    // ==================== 主采集/编码循环 ====================
    while (1)
    {
        if (exit_flag)
            break;

        int index = v4l2.Dqbuf();
        if (index == -1) continue;
        cnt_capture++;
        t_frame_start = current_time_ms();

        MppBuffer mpp_buf = mpp.GetMppBuffer(index);
        int mpp_buf_fd = mpp_buffer_get_fd(mpp_buf);
        if (ss.extra_copies > 0)
            simulate_extra_copies(mpp_buffer_get_ptr(mpp_buf), (size_t)ss.frame_size, ss.extra_copies);

        bool is_ai_frame = (frame_counter % ai.frame_interval == 0);
        frame_counter++;

        // ---------- 尝试将帧送给 AI 线程 ----------
        if (is_ai_frame)
        {
            int ai_idx = -1;
            {
                std::lock_guard<std::mutex> lock(ai_mtx);
                if (!free_ai_bufs.empty())
                {
                    ai_idx = free_ai_bufs.front();
                    free_ai_bufs.pop_front();
                }
            }
            if (ai_idx >= 0)
            {
                int ret = rga_rotate_fd(mpp_buf_fd, orig_w, orig_h,
                                        ai_rotate_fds[ai_idx], rot_w, rot_h,
                                        IM_HAL_TRANSFORM_ROT_270);
                if (ret == 0)
                {
                    std::lock_guard<std::mutex> lock(ai_mtx);
                    ai_push_ms[ai_idx] = current_time_ms();
                    ai_queue.push_back(ai_idx);
                    ai_cv.notify_one();
                }
                else
                {
                    std::cerr << "AI旋转失败，跳过本帧AI" << std::endl;
                    std::lock_guard<std::mutex> lock(ai_mtx);
                    free_ai_bufs.push_back(ai_idx);
                }
            }
        }

        // ---------- 决定是否绘图（追踪器快照，含 ID/姿态） ----------
        // 漏检宽限（60 个 AI 帧）在追踪层保留目标以防 ID 抖动，但绘图只画
        // 最近 30 个 AI 帧(≈2s)内仍有检测的：人走出画面后"鬼影框"最多停留
        // 2s 而不是 4s——否则人离开后空地上还画着框，观感等于"没人的地方
        // 检测出框"。报警投票/闸门逻辑仍在追踪层完整保留，不受影响
        static const int kDrawStaleFrames = 30;
        std::vector<TrackedPerson> draw_persons;
        {
            std::vector<TrackedPerson> all = person_tracker.getPersons();
            for (auto& p : all)
            {
                if (p.lost_frames <= kDrawStaleFrames)
                    draw_persons.push_back(p);
            }
        }
        bool has_draw = !draw_persons.empty();

        // 诊断（排查"框闪烁"）：统计送编码的帧中带框/无框比例——若 drawn 远小于
        // 总帧数，说明追踪器在编码侧就没有持续输出目标（闪烁源头在检测/追踪）；
        // 若 drawn≈全部，则闪烁在编码/传输侧。每 3s 打一行
        {
            static int dbg_drawn = 0, dbg_undrawn = 0, dbg_frames = 0;
            static int64_t dbg_ts = 0;
            dbg_frames++;
            if (has_draw) dbg_drawn++; else dbg_undrawn++;
            int64_t now = current_time_ms();
            if (dbg_ts == 0) dbg_ts = now;
            if (now - dbg_ts >= 3000) {
                printf("[DRAW] 3s 编码帧=%d 带框=%d 无框=%d\n", dbg_frames, dbg_drawn, dbg_undrawn);
                dbg_frames = dbg_drawn = dbg_undrawn = 0;
                dbg_ts = now;
            }
        }

        if (has_draw)
        {
            int ret = rga_rotate_fd(mpp_buf_fd, orig_w, orig_h,
                                    draw_rotate_fd, rot_w, rot_h,
                                    IM_HAL_TRANSFORM_ROT_270);
            if (ret < 0)
            {
                std::cerr << "RGA rotate 270°（绘图）失败" << std::endl;
                v4l2.Qbuf(mpp.GetMppBufferFd(index), index);
                continue;
            }

            ret = rga_convert_fd(draw_rotate_fd, rot_w, rot_h, RK_FORMAT_YCbCr_420_SP,
                                 draw_mem_fd, rot_w, rot_h, RK_FORMAT_RGB_888);
            if (ret < 0)
            {
                std::cerr << "RGA NV12→RGB888 转换失败" << std::endl;
                v4l2.Qbuf(mpp.GetMppBufferFd(index), index);
                continue;
            }

            image_buffer_t draw_img;
            memset(&draw_img, 0, sizeof(draw_img));
            draw_img.width = rot_w;
            draw_img.height = rot_h;
            draw_img.format = IMAGE_FORMAT_RGB888;
            draw_img.virt_addr = (uint8_t*)draw_mem_ptr;
            draw_img.size = rot_rgb_size;

            // ★每帧 CPU 访问前失效缓存（画框与干净快照共用一次）★：
            // draw_mem 刚由 RGA(DMA) 写入，带缓存的 mmap 若不失效，CPU 画框会
            // 命中上一帧遗留缓存行（旧底+新框），flush 后覆盖 RGA 的新帧 → 拖影
            dma_buf_sync_cpu_start(draw_mem_fd);

            // 画框前的干净帧快照：VLM 复核/巡检/远程问答的输入不带检测标注
            // （框/骨架/标签会干扰大模型姿态判断，实测"手机屏幕"类幻觉即源于此）；
            // 仅被请求（巡检/远程）或跌倒事件待投递时拷贝一次，平时零开销
            cv::Mat clean_rgb;
            if (pending_fall.load() || g_frame_provider.wanted())
            {
                cv::Mat clean_view(rot_h, rot_w, CV_8UC3, draw_mem_ptr);
                clean_rgb = clean_view.clone();
            }

            for (size_t i = 0; i < draw_persons.size(); i++)
            {
                const TrackedPerson& tp = draw_persons[i];
                const object_detect_result& det = tp.det;

                // 跌倒者红色框警示，其余蓝色
                unsigned int box_color = (tp.pose == POSE_FALLING) ? COLOR_RED : COLOR_BLUE;

                // 线宽 6px/点半径 6/字号 22：480x640 竖图上肉眼清晰（原 3px 在
                // 子码流里太细看不清，用户实测反馈后加粗）
                draw_rectangle(&draw_img,
                               det.box.left, det.box.top,
                               det.box.right - det.box.left,
                               det.box.bottom - det.box.top,
                               box_color, 6);
                draw_skeleton(&draw_img, det, COLOR_ORANGE, 6);
                for (int k = 0; k < 17; k++)
                {
                    float x = det.keypoints[k][0];
                    float y = det.keypoints[k][1];
                    float conf = det.keypoints[k][2];
                    // 显示阈值 0.10（判定逻辑仍用 pose_classifier 内部阈值）：
                    // 0.25 恰在照片/远距离场景关键点置信度的抖动带上（实测
                    // 0.0-0.3 跳变），点会一帧有一帧无地闪
                    if (conf > 0.10)
                    {
                        draw_circle(&draw_img, (int)x, (int)y, 6, COLOR_YELLOW, -1);
                    }
                }

                // 标签: 追踪ID + 姿态 + 置信度
                char text[64];
                snprintf(text, sizeof(text), "ID%d %s %.2f",
                         tp.id, pose_state_name(tp.pose), det.prop);
                draw_text(&draw_img, text, det.box.left, det.box.top - 20, COLOR_RED, 22);
            }

            // 3.5 检查待处理的跌倒事件，生成图像并投递到事件线程
            if (pending_fall.load())
            {
                pending_fall.store(false);
                float conf = pending_fall_conf.load();

                cv::Mat event_img(rot_h, rot_w, CV_8UC3);
                if (clean_rgb.empty())
                {
                    // 兜底：画框前快照检查后 AI 线程才置位 pending_fall 的窄窗
                    // （极罕见），退回用画框后的帧
                    cv::Mat draw_mat(rot_h, rot_w, CV_8UC3, draw_mem_ptr);
                    cv::cvtColor(draw_mat, event_img, cv::COLOR_RGB2BGR);
                }
                else
                {
                    cv::cvtColor(clean_rgb, event_img, cv::COLOR_RGB2BGR);
                }

                AIEventData ev_data;
                ev_data.confidence = conf;
                ev_data.timestamp = time(nullptr);
                ev_data.image = event_img.clone();

                {
                    std::lock_guard<std::mutex> lock(event_mtx);
                    event_queue.push_back(std::move(ev_data));
                }
                event_cv.notify_one();
            }

            // 3.6 FrameProvider 快照槽位（巡检/远程抓帧；投递画框前的干净帧，
            // rotate_back 前完成）。仅被请求时才做转换+clone，平时早退零开销
            if (!clean_rgb.empty())
            {
                g_frame_provider.offer(clean_rgb);
            }

            // 刷出缓存：CPU 画的框/骨架/标签写进缓存行，RGA(DMA) 读前必须 SYNC，
            // 否则编码帧"无框"（框闪烁历史根因在 dma-heap mmap 下的复现通道）
            dma_buf_sync_cpu_end(draw_mem_fd);

            ret = rga_convert_fd(draw_mem_fd, rot_w, rot_h, RK_FORMAT_RGB_888,
                                 rotate_back_fd, rot_w, rot_h, RK_FORMAT_YCbCr_420_SP);
            if (ret < 0)
            {
                std::cerr << "RGA RGB888→NV12 转换失败" << std::endl;
                v4l2.Qbuf(mpp.GetMppBufferFd(index), index);
                continue;
            }

            ret = rga_rotate_fd(rotate_back_fd, rot_w, rot_h,
                                mpp_buf_fd, orig_w, orig_h,
                                IM_HAL_TRANSFORM_ROT_90);
            if (ret < 0)
            {
                std::cerr << "RGA rotate 90° 写回失败" << std::endl;
                v4l2.Qbuf(mpp.GetMppBufferFd(index), index);
                continue;
            }
        }
        else if (g_frame_provider.wanted())
        {
            // 无人/未绘图帧的快照支持：远程 snapshot 需要拍"空房间"图。
            // 仅当有请求在等时走一次 RGA 旋转+转换投递（平时零开销）；
            // 不写回 mpp_buf（本帧不绘图，无需改动编码内容）
            int ret = rga_rotate_fd(mpp_buf_fd, orig_w, orig_h,
                                    draw_rotate_fd, rot_w, rot_h,
                                    IM_HAL_TRANSFORM_ROT_270);
            if (ret == 0)
                ret = rga_convert_fd(draw_rotate_fd, rot_w, rot_h, RK_FORMAT_YCbCr_420_SP,
                                     draw_mem_fd, rot_w, rot_h, RK_FORMAT_RGB_888);
            if (ret == 0)
            {
                // 失效缓存：RGA 刚写 draw_mem，CPU(offer 内 cvtColor)读前必须 SYNC
                dma_buf_sync_cpu_start(draw_mem_fd);
                cv::Mat draw_view(rot_h, rot_w, CV_8UC3, draw_mem_ptr);
                g_frame_provider.offer(draw_view);
            }
        }

        // ---------- MPP 编码 ----------
        MppFrame frame = NULL;
        mpp_frame_init(&frame);
        mpp_frame_set_width(frame, orig_w);
        mpp_frame_set_height(frame, orig_h);
        mpp_frame_set_hor_stride(frame, ss.hor_stride);
        mpp_frame_set_ver_stride(frame, ss.ver_stride);
        mpp_frame_set_fmt(frame, MPP_FMT_YUV420SP);
        mpp_frame_set_buffer(frame, mpp_buf);
        mpp_frame_set_eos(frame, 0);

        if (mpp.mpi->encode_put_frame(mpp.ctx, frame) != MPP_SUCCESS)
        {
            std::cerr << "子码流编码失败" << std::endl;
            mpp_frame_deinit(&frame);
            v4l2.Qbuf(mpp.GetMppBufferFd(index), index);
            continue;
        }

        MppPacket packet = NULL;
        if (mpp.mpi->encode_get_packet(mpp.ctx, &packet) != MPP_SUCCESS)
        {
            std::cerr << "子码流获取packet失败" << std::endl;
            mpp_frame_deinit(&frame);
            v4l2.Qbuf(mpp.GetMppBufferFd(index), index);
            continue;
        }

        H265EncodePacket enc_pkt;
        void *p = mpp_packet_get_data(packet);
        size_t len = mpp_packet_get_length(packet);
        enc_pkt.data.assign(static_cast<uint8_t*>(p), static_cast<uint8_t*>(p) + len);
        gettimeofday(&enc_pkt.timestamp, NULL);
        RK_U32 mpp_flags = mpp_packet_get_flag(packet);
        enc_pkt.is_idr = (mpp_flags & 0x01);

        int64_t t_encoded = current_time_ms();
        double frame_latency = t_encoded - t_frame_start;
        sum_latency_ms += frame_latency;
        lat_samples++;
        if (frame_latency > max_latency_ms) max_latency_ms = frame_latency;

        shared_queue->push_packet(enc_pkt);
        cnt_encode++;

        // 注意：子码流不再调用 g_subRecorder.onVideoPacket
        // 子码流仅用于 AI 推理和 RTSP，录像由主码流实例统一负责

        if(shared_queue->GetMppEventID()!=0 && shared_queue->GetSource() != NULL)
        {
            scheduler->triggerEvent(shared_queue->GetMppEventID(), shared_queue->GetSource());
        }

        v4l2.Qbuf(mpp.GetMppBufferFd(index), index);
        mpp_frame_deinit(&frame);
        mpp_packet_deinit(&packet);

        // ---------- 每秒统计并打印 ----------
        int64_t now = current_time_ms();
        if (now - last_stat_time >= 1000)
        {
            stats.capture_fps = cnt_capture;
            stats.encode_fps = cnt_encode;

            AiStats snap;
            double snap_ai_wait = 0.0;
            {
                std::lock_guard<std::mutex> lock(ai_stats_mtx);
                snap = ai_stats;
                ai_stats.frames = 0;
                ai_stats.total_ms = 0.0;
                ai_stats.total_wait_ms = 0.0;
                ai_stats.samples = 0;
            }
            stats.ai_fps = snap.frames;
            if (snap.samples > 0)
            {
                stats.avg_infer_ms = snap.total_ms / snap.samples;
                snap_ai_wait = snap.total_wait_ms / snap.samples;
            }
            else
                stats.avg_infer_ms = 0.0;
            stats.person_count = snap.persons;
            stats.queue_size = shared_queue->size();

            double p50 = 0.0, p95 = 0.0;
            snap.percentile(&p50, &p95);

            double avg_lat = (lat_samples > 0) ? (sum_latency_ms / lat_samples) : 0.0;
            char ts[32];
            get_time_str_ms(ts, sizeof(ts));
            // AIwait=AI 队列等待均值（采集到推理开始的排队）；检测滞后=追踪器
            // 最近更新距当前编码帧的时间（框"旧"了多少）——一帧的完整链路
            // ≈ 队列等待 + 推理 + 后处理 + 检测滞后 + 编码
            double detect_lag = (double)person_tracker.lastUpdateAgeMs();
            printf("[SUB  %s] Cap=%d fps | Enc=%d fps | AI=%d fps | Inf=%.1fms p50=%.1fms p95=%.1fms | AIwait=%.1fms | DetectLag=%.0fms | Queue=%zu | Persons=%d | LatAvg=%.1fms LatMax=%.1fms\n",
                   ts, stats.capture_fps, stats.encode_fps, stats.ai_fps,
                   stats.avg_infer_ms, p50, p95, snap_ai_wait, detect_lag,
                   stats.queue_size, stats.person_count,
                   avg_lat, max_latency_ms);

            cnt_capture = 0;
            cnt_encode = 0;
            sum_latency_ms = 0.0;
            lat_samples = 0;
            max_latency_ms = 0.0;
            last_stat_time = now;
        }
    }

    // ==================== 清理与退出 ====================
    ai_cv.notify_all();
    event_cv.notify_all();
    if (ai_thread.joinable()) ai_thread.join();
    if (event_thread.joinable()) event_thread.join();

    cleanup_resources();
    std::cout << "子码流 AI 资源清理" << std::endl;
}