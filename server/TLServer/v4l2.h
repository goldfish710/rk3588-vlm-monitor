#ifndef V4L2_H
#define V4L2_H

#include "config.h"

class V4l2class
{
    public:
        V4l2class();
        ~V4l2class();
        int InitV4l2(std::string dev,int width,int hight,int frame_size,int buffer_count);
        int Qbuf(int fd, int index);
        int Dqbuf();
        int StreamOn();
        int GetFrameSize();
        void Close();

    private:
        int V4l2Check(int fd, std::string error);

        int v4l2_fd;
        int frame_size;
        struct v4l2_capability _v4l2_cap;
        struct v4l2_format _v4l2_format;
        struct v4l2_buffer _v4l2_buffer;
        struct v4l2_plane _v4l2_planes[1];
        struct v4l2_requestbuffers _v4l2_requestbuffes;
};

#endif
