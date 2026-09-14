#include "v4l2.h"

V4l2class::V4l2class()
{
    frame_size = 0;
    v4l2_fd = -1;
}

V4l2class::~V4l2class()
{
    Close();
}

int V4l2class::V4l2Check(int fd, std::string error)
{
    if (fd < 0)
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

int V4l2class::InitV4l2(std::string dev,int width,int hight,int frame_size,int buffer_count)
{
    this->frame_size = frame_size;
    v4l2_fd = open(dev.c_str(), O_RDWR);
    if (!V4l2Check(v4l2_fd, "open"))
    {
        return 0;
    }

    memset(&_v4l2_cap, 0, sizeof(_v4l2_cap));
    int result = ioctl(v4l2_fd, VIDIOC_QUERYCAP, &_v4l2_cap);
    if (!V4l2Check(result, "VIDIOC_QUERYCAP"))
    {
        return 0;
    }

    __u32 caps = _v4l2_cap.capabilities;
    if (caps & V4L2_CAP_DEVICE_CAPS)
    {
        caps = _v4l2_cap.device_caps;
    }
    if (caps & V4L2_CAP_VIDEO_CAPTURE)
    {
        std::cout << "支持单平面" << std::endl;
    }
    if (caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE)
    {
        std::cout << "支持多平面" << std::endl;
    }

    memset(&_v4l2_format, 0, sizeof(_v4l2_format));
    _v4l2_format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    _v4l2_format.fmt.pix_mp.width = width;
    _v4l2_format.fmt.pix_mp.height = hight;
    _v4l2_format.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
    _v4l2_format.fmt.pix_mp.num_planes = 1;
    result = ioctl(v4l2_fd, VIDIOC_S_FMT, &_v4l2_format);
    if (!V4l2Check(result, "VIDIOC_S_FMT"))
    {
        return 0;
    }
    std::cout << "sizeimage: " << _v4l2_format.fmt.pix_mp.plane_fmt[0].sizeimage << std::endl;

    memset(&_v4l2_requestbuffes, 0, sizeof(_v4l2_requestbuffes));
    _v4l2_requestbuffes.count = buffer_count;
    _v4l2_requestbuffes.memory = V4L2_MEMORY_DMABUF;
    _v4l2_requestbuffes.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (ioctl(v4l2_fd, VIDIOC_REQBUFS, &_v4l2_requestbuffes) < 0)
    {
        perror("VIDIOC_REQBUFS");
        return 0;
    }
    std::cout << "VIDIOC_REQBUFS 成功" << std::endl;

    return 1;
}

//入队
int V4l2class::Qbuf(int fd, int index)
{
    memset(&_v4l2_buffer, 0, sizeof(_v4l2_buffer));
    memset(&_v4l2_planes, 0, sizeof(_v4l2_planes));

    _v4l2_planes[0].m.fd = fd;
    _v4l2_planes[0].length = frame_size;

    _v4l2_buffer.memory = V4L2_MEMORY_DMABUF;
    _v4l2_buffer.index = index;
    _v4l2_buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    _v4l2_buffer.m.planes = _v4l2_planes;
    _v4l2_buffer.length = 1;

    int result = ioctl(v4l2_fd, VIDIOC_QBUF, &_v4l2_buffer);
    if (result < 0)
    {
        perror("VIDIOC_QBUF");
        return 0;
    }
    return 1;
}

//出队
int V4l2class::Dqbuf()
{
    memset(&_v4l2_buffer, 0, sizeof(_v4l2_buffer));
    memset(&_v4l2_planes, 0, sizeof(_v4l2_planes));

    _v4l2_buffer.memory = V4L2_MEMORY_DMABUF;
    _v4l2_buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    _v4l2_buffer.m.planes = _v4l2_planes;
    _v4l2_buffer.length = 1;

    int result = ioctl(v4l2_fd, VIDIOC_DQBUF, &_v4l2_buffer);
    if (result < 0)
    {
        perror("VIDIOC_DQBUF");
        return -1;
    }
    return _v4l2_buffer.index;
}

int V4l2class::StreamOn()
{
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    int result = ioctl(v4l2_fd, VIDIOC_STREAMON, &type);
    if (result < 0)
    {
        std::cout << "VIDIOC_STREAMON 失败" << std::endl;
        return 0;
    }
    std::cout << "VIDIOC_STREAMON 成功" << std::endl;
    return 1;
}

int V4l2class::GetFrameSize()
{
    return frame_size;
}

void V4l2class::Close()
{
    if(v4l2_fd==-1)
        return;

    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    int result = ioctl(v4l2_fd, VIDIOC_STREAMOFF, &type);
    if (result < 0)
    {
        std::cout << "VIDIOC_STREAMOFF 失败" << std::endl;
    }
    else
    {
        std::cout << "VIDIOC_STREAMOFF 成功" << std::endl;
    }
    

    close(v4l2_fd);
}