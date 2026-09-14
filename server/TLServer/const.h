// #ifndef CONST_H
// #define CONST_H

// #include<iostream>
// #include<stdio.h>
// #include<cstring>
// #include<fcntl.h>
// #include<unistd.h>
// #include<vector>
// #include<deque>
// #include<cstdint>
// #include<thread>
// #include<mutex>
// #include<atomic>
// #include<csignal>
// #include<sys/ioctl.h>
// #include<sys/mman.h>
// #include<sys/time.h>
// #include<linux/videodev2.h>
// #include<rockchip/rk_mpi.h>
// #include<rockchip/mpp_buffer.h>
// #include<rockchip/mpp_frame.h>
// #include<rockchip/mpp_packet.h>
// #include<FramedSource.hh>
// #include<UsageEnvironment.hh>
// #include<OnDemandServerMediaSubsession.hh>
// #include<H265VideoStreamFramer.hh>
// #include<H265VideoRTPSink.hh>
// #include<H264VideoStreamFramer.hh>
// #include<H264VideoRTPSink.hh>
// #include<BasicUsageEnvironment.hh>
// #include<RTSPServer.hh>
// #include<ServerMediaSession.hh>

// // ============================================================
// // 主码流：H.265 2592x1944（IMX415 5MP）
// // 用途：人工观看 + 循环录像
// // 存储：4Mbps = 43.2GB/天 → 128GB约2.8天循环
// // ============================================================
// constexpr int VIDEO_WIDTH = 2592;
// constexpr int VIDEO_HEIGHT = 1944;
// constexpr int VIDEO_HOR_STRIDE = 2592;
// constexpr int VIDEO_VER_STRIDE = 1952;     // 垂直16对齐
// constexpr int VIDEO_FRAME_SIZE = VIDEO_HOR_STRIDE * VIDEO_VER_STRIDE * 3 / 2;

// // 编码参数
// constexpr int ENC_BITRATE = 4000000;       // CBR 目标4Mbps
// constexpr int ENC_BITRATE_MAX = 4500000;   // 峰值4.5Mbps
// constexpr int ENC_BITRATE_MIN = 3000000;   // 下限3Mbps
// constexpr int ENC_FPS = 30;                // 与IMX415实际输出一致（原60无效）
// constexpr int ENC_GOP = 30;                // 1s一个IDR（便于录像切片对齐，原GOP=10过于频繁浪费码率）

// // Buffer参数
// constexpr int BUFFER_COUNT = 6;

// // RTSP参数
// constexpr int RTSP_PORT = 8554;
// constexpr int RTP_OUT_BUF_MAX = 200000;

// // ============================================================
// // 子码流：H.264 640x480
// // 用途：AI推理 + 事件片段（带检测框）
// // ============================================================
// constexpr int SUB_VIDEO_WIDTH = 640;
// constexpr int SUB_VIDEO_HEIGHT = 480;
// constexpr int SUB_VIDEO_HOR_STRIDE = 640;
// constexpr int SUB_VIDEO_VER_STRIDE = 480;
// constexpr int SUB_VIDEO_FRAME_SIZE = SUB_VIDEO_HOR_STRIDE * SUB_VIDEO_VER_STRIDE * 3 / 2;

// // 编码参数（VGA 1.5Mbps已是高质量，2.5Mbps纯浪费）
// constexpr int SUB_ENC_BITRATE = 1500000;
// constexpr int SUB_ENC_BITRATE_MAX = 2000000;
// constexpr int SUB_ENC_BITRATE_MIN = 1000000;
// constexpr int SUB_ENC_FPS = 30;
// constexpr int SUB_ENC_GOP = 15;            // 0.5s一个IDR，事件片段起点对齐误差≤0.5s

// // Buffer参数
// constexpr int SUB_BUFFER_COUNT = 4;

// // RTSP参数
// constexpr int SUB_RTSP_PORT = 8554;
// constexpr int SUB_RTP_OUT_BUF_MAX = 200000;

// struct H265EncodePacket
// {
//     std::vector<uint8_t>data;
//     timeval timestamp{};
//     bool is_idr = false;   // 新增：MPP编码后填充
// };

// extern volatile char watchVariable;
// extern std::atomic<bool> exit_flag;

// #endif