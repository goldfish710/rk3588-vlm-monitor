#ifndef MPP_H
#define MPP_H

#include <cstddef>

#include "config.h"
#include "SharedQueue.h"

// 从 /dev/dma_heap/cma 分配物理连续 dma-buf（返回 fd，失败 -1）。
// RGA 对连续内存直接映射；若用 system 堆（页不连续），RGA 无 IOMMU 时会把
// 缓冲弹跳进有限的 swiotlb 池——4K 缓冲(12.4MB)的连续槽段需求会碎片化耗尽
// 该池，导致**所有** RGA 任务失败（实测 dmesg "swiotlb buffer is full"，
// 连子码流小任务一并挂掉）
int mpp_alloc_cma_fd(size_t size);
class Mppclass
{
    public:
        Mppclass();
        ~Mppclass();
        // buf_size：内部缓冲池每个 buffer 的分配尺寸（-1 = 用 frame_size）。
        // 主码流 4K 采集→RGA 缩放到 1080p 编码场景：编码尺寸是 1080p，
        // 但 buffer 要按 4K 分配才能 Qbuf 进 V4L2（DMABUF 尺寸必须匹配采集格式）
        int InitMpp(MppCodingType codec_type, int width, int height,int hor_stride, int ver_stride, int frame_size,int bitrate, int bitrate_max, int bitrate_min,int fps, int gop, int buffer_count,SharedQueue* shared_queue, int buf_size = -1, int rotation = 0);
        int GetMppBufferFd(int index);
        MppBuffer GetMppBuffer(int index);
        void Close();

        MppApi *mpi;
        MppCtx ctx;
        MppPacket _mpp_hdr_packet;

    private:
        int MppCheck(MPP_RET type, std::string error);

        MPP_RET mpp_ret;
        MppEncCfg cfg;
        MppBufferGroup _mpp_buffer_group;
        // MppBuffer _mpp_buffer[6];
        // int _mpp_buffer_fd[6];
        std::vector<MppBuffer> _mpp_buffer;   // ← 改为 vector
        std::vector<int> _mpp_buffer_fd;      // ← 改为 vector
        int _buffer_count;
        int _buf_size = 0;   // 每个 buffer 的分配尺寸（0=未设置）
};

#endif
