#include "mpp.h"
#include <vector>
#include <string>
#include <linux/dma-heap.h>

int mpp_alloc_cma_fd(size_t size)
{
    static int heap_fd = -1;
    if (heap_fd < 0)
        heap_fd = open("/dev/dma_heap/cma", O_RDWR | O_CLOEXEC);
    if (heap_fd < 0)
    {
        std::cerr << "打开 /dev/dma_heap/cma 失败" << std::endl;
        return -1;
    }
    struct dma_heap_allocation_data data = {};
    data.len = size;
    data.fd = 0;
    data.fd_flags = O_RDWR | O_CLOEXEC;
    if (ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &data) < 0)
    {
        std::cerr << "CMA 分配失败 size=" << size << " errno=" << errno << std::endl;
        return -1;
    }
    return (int)data.fd;
}
Mppclass::Mppclass()
{
    mpp_ret = MPP_SUCCESS;
    mpi = NULL;
    ctx = NULL;
    cfg = NULL;
    _mpp_buffer_group = NULL;
    _mpp_hdr_packet = NULL;
    _buffer_count = 0;
}

Mppclass::~Mppclass()
{
    Close();
}

int Mppclass::MppCheck(MPP_RET type, std::string error)
{
    if (type != MPP_SUCCESS)
    {
        std::cout << error << " 失败" << std::endl;
        return 0;
    }
    else
    {
        std::cout << error << " 成功" << std::endl;
        return 1;
    }
}

int Mppclass::InitMpp(MppCodingType codec_type, int width, int height,
                      int hor_stride, int ver_stride, int frame_size,
                      int bitrate, int bitrate_max, int bitrate_min,
                      int fps, int gop, int buffer_count,
                      SharedQueue *shared_queue, int buf_size, int rotation)
{
    if (buf_size <= 0) buf_size = frame_size;   // 默认与编码帧同尺寸
    _buf_size = buf_size;
    _buffer_count = buffer_count;
    mpp_ret = mpp_create(&ctx, &mpi);
    if (!MppCheck(mpp_ret, "mpp_create"))
        return 0;

    mpp_ret = mpp_init(ctx, MPP_CTX_ENC, codec_type);
    if (!MppCheck(mpp_ret, "mpp_init"))
        return 0;

    mpp_ret = mpp_enc_cfg_init(&cfg);
    if (!MppCheck(mpp_ret, "mpp_enc_cfg_init"))
        return 0;

    mpp_enc_cfg_set_s32(cfg, "prep:width", width);
    mpp_enc_cfg_set_s32(cfg, "prep:height", height);
    mpp_enc_cfg_set_s32(cfg, "prep:hor_stride", hor_stride);
    mpp_enc_cfg_set_s32(cfg, "prep:ver_stride", ver_stride);
    mpp_enc_cfg_set_s32(cfg, "prep:format", MPP_FMT_YUV420SP);
    // 编码器旋转（0/90/180/270 顺时针；输出尺寸随之互换）。
    // 键名必须是 "prep:rotation"（板端日志实证：写 "prep:rotate" 会被 kmpp
    // 拒绝 `set prep:rotate s32 failed ret -1`，旋转静默失效）。
    // 值语义 = MPP_ENC_ROT_* 枚举（0/1/2/3），mpp_enc_impl 对 90/270 自动
    // 交换编码宽高并重发 SPS，下行 RTSP 从 SPS 解析尺寸，无需改 SDP
    int mpp_rot = MPP_ENC_ROT_0;
    if (rotation == 90)       mpp_rot = MPP_ENC_ROT_90;
    else if (rotation == 180) mpp_rot = MPP_ENC_ROT_180;
    else if (rotation == 270) mpp_rot = MPP_ENC_ROT_270;
    mpp_enc_cfg_set_s32(cfg, "prep:rotation", mpp_rot);

    mpp_enc_cfg_set_s32(cfg, "rc:mode", MPP_ENC_RC_MODE_CBR);
    mpp_enc_cfg_set_s32(cfg, "rc:bps_target", bitrate);
    mpp_enc_cfg_set_s32(cfg, "rc:bps_max", bitrate_max);
    mpp_enc_cfg_set_s32(cfg, "rc:bps_min", bitrate_min);

    // ========== 码率控制帧率（影响 PTS 和码率控制） ==========
    mpp_enc_cfg_set_s32(cfg, "rc:fps_in_num", fps);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_in_denorm", 1);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_out_num", fps);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_out_denorm", 1);

    mpp_enc_cfg_set_s32(cfg, "rc:gop", gop);

    mpp_enc_cfg_set_s32(cfg, "codec:type", codec_type);

    // ========== 探测有效的帧率字段 ==========
    struct FpsField {
        const char* name_num;
        const char* name_den;
    };
    std::vector<FpsField> candidates = {
        {"rc:fps_in_num",     "rc:fps_in_denorm"},
        {"rc:fps_out_num",    "rc:fps_out_denorm"},
        {"prep:fps_in_num",   "prep:fps_in_denorm"},
        {"prep:fps_out_num",  "prep:fps_out_denorm"},
        {"prep:fps_num",      "prep:fps_denorm"},      // 旧字段
        {"rc:fps_num",        "rc:fps_denorm"},
        {"prep:fps",          "prep:fps_den"},         // 极旧版本
        {"rc:fps",            "rc:fps_den"},
    };
    bool fps_set_ok = false;
    for (auto& f : candidates) {
        RK_S32 tmp_num = 0, tmp_den = 0;
        // 先测试读取，若读取不到则跳过
        mpp_enc_cfg_get_s32(cfg, f.name_num, &tmp_num);
        mpp_enc_cfg_get_s32(cfg, f.name_den, &tmp_den);
        if (tmp_num == 0 && tmp_den == 0) {
            // 尝试写入并回读
            mpp_enc_cfg_set_s32(cfg, f.name_num, fps);
            mpp_enc_cfg_set_s32(cfg, f.name_den, 1);
            mpp_enc_cfg_get_s32(cfg, f.name_num, &tmp_num);
            mpp_enc_cfg_get_s32(cfg, f.name_den, &tmp_den);
        }
        if (tmp_num == fps && tmp_den == 1) {
            printf("[MPP] 有效帧率字段: %s / %s，回读 %d/%d\n", f.name_num, f.name_den, tmp_num, tmp_den);
            fps_set_ok = true;
            break;
        }
    }
    if (!fps_set_ok) {
        printf("[MPP] 警告：未找到有效的帧率配置字段，SPS VUI 将保持默认值！\n");
        // 可继续运行，但录像文件可能播放异常
    }

    mpp_ret = mpi->control(ctx, MPP_ENC_SET_CFG, cfg);
    if (!MppCheck(mpp_ret, "MPP_ENC_SET_CFG"))
        return 0;

    // 缓冲池不再从 DRM system 堆分配（页不连续，RGA 无 IOMMU 时走 swiotlb
    // 弹跳，4K 大缓冲会把 swiotlb 池碎片化耗尽、拖垮所有 RGA 任务），
    // 改为从 /dev/dma_heap/cma 分配物理连续缓冲再 import 进 MPP
    // ========== 动态分配缓冲区（原固定数组[6]改为 vector） ==========
    _mpp_buffer.clear();
    _mpp_buffer.resize(buffer_count, NULL);
    _mpp_buffer_fd.clear();
    _mpp_buffer_fd.resize(buffer_count, -1);
    for (int i = 0; i < buffer_count; i++)
    {
        int fd = mpp_alloc_cma_fd((size_t)_buf_size);
        if (fd < 0)
            return 0;

        MppBufferInfo info = {};
        info.type = MPP_BUFFER_TYPE_EXT_DMA;
        info.fd = fd;
        info.size = (size_t)_buf_size;
        mpp_ret = mpp_buffer_import(&_mpp_buffer[i], &info);
        if (!MppCheck(mpp_ret, "mpp_buffer_import"))
        {
            close(fd);
            return 0;
        }

        // 初始化mpp_buffer Y平面全黑（防未写入区域花屏）
        void *ptr = mpp_buffer_get_ptr(_mpp_buffer[i]);
        size_t y_size = hor_stride * ver_stride;
        memset(ptr, 0x00, y_size);
        memset((uint8_t *)ptr + y_size, 0x80, y_size / 2);

        _mpp_buffer_fd[i] = fd;   // fd 保持打开：V4L2 Qbuf(DMABUF) 需要
    }

    _mpp_hdr_packet = NULL;
    mpp_ret = mpi->control(ctx, MPP_ENC_GET_EXTRA_INFO, &_mpp_hdr_packet);
    if (mpp_ret == MPP_SUCCESS && _mpp_hdr_packet)
    {
        std::cout << "获取头信息成功" << std::endl;
    }
    else
    {
        std::cout << "获取头信息失败" << std::endl;
        return 0;
    }

    // 解析头数据给SharedQueue再在创建RTPsink的时候传入
    void *p = mpp_packet_get_data(_mpp_hdr_packet);
    uint8_t *data = static_cast<uint8_t *>(p);
    size_t length = mpp_packet_get_length(_mpp_hdr_packet);
    std::vector<size_t> sc_pos; // 存起始码索引下标
    for (size_t i = 0; i + 3 < length; i++)
    {
        if (data[i] == 0x00 && data[i + 1] == 0x00 && data[i + 2] == 0x00 && data[i + 3] == 0x01)
        {
            sc_pos.push_back(i);
        }
    }

    // 遍历每一个NAL
    for (size_t i = 0; i < sc_pos.size(); i++)
    {
        uint8_t *nal_data = data + sc_pos[i] + 4;
        size_t nal_len;
        if (i + 1 < sc_pos.size()) // 说明还有下一个nal
        {
            nal_len = sc_pos[i + 1] - (sc_pos[i] + 4);
        }
        else
        {
            nal_len = length - (sc_pos[i] + 4);
        }

        if (codec_type == MPP_VIDEO_CodingHEVC)
        {
            // H.265: NAL type = (byte>>1) & 0x3F, VPS=32 SPS=33 PPS=34
            uint8_t nal_type = (nal_data[0] >> 1) & 0x3F;
            if (nal_type == 32)
                shared_queue->SetVPS(nal_data, nal_len);
            else if (nal_type == 33)
                shared_queue->SetSPS(nal_data, nal_len);
            else if (nal_type == 34)
                shared_queue->SetPPS(nal_data, nal_len);
        }
        else if (codec_type == MPP_VIDEO_CodingAVC)
        {
            // H.264: NAL type = byte & 0x1F, SPS=7 PPS=8, 无VPS
            uint8_t nal_type = nal_data[0] & 0x1F;
            if (nal_type == 7)
                shared_queue->SetSPS(nal_data, nal_len);
            else if (nal_type == 8)
                shared_queue->SetPPS(nal_data, nal_len);
        }
    }
    return 1;
}

int Mppclass::GetMppBufferFd(int index)
{
    return _mpp_buffer_fd[index];
}

MppBuffer Mppclass::GetMppBuffer(int index)
{
    return _mpp_buffer[index];
}

void Mppclass::Close()
{
    if (ctx == NULL)
        return;

    for (int i = 0; i < _mpp_buffer.size(); i++)
    {
        if (_mpp_buffer[i])
        {
            mpp_buffer_put(_mpp_buffer[i]);
            _mpp_buffer[i] = NULL;
        }
    }
    _mpp_buffer.clear();
    _mpp_buffer_fd.clear();

    if (_mpp_buffer_group)
    {
        mpp_buffer_group_put(_mpp_buffer_group);
        _mpp_buffer_group = NULL;
    }

    if (cfg)
    {
        mpp_enc_cfg_deinit(cfg);
        cfg = NULL;
    }

    mpp_destroy(ctx);
    ctx = NULL;
    mpi = NULL;

    std::cout << "MPP 资源释放完成" << std::endl;
}