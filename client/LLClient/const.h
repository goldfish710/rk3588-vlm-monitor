#ifndef CONST_H
#define CONST_H


extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
}

//主/子 码流拉流地址
constexpr const char* MAIN_STREAM_URL = "rtsp://192.168.100.1:8554/h265";
constexpr const char* SUB_STREAM_URL  = "rtsp://192.168.100.1:8554/h264";

//主码流分辨率
constexpr int MAIN_VIDEO_WIDTH  = 1920;
constexpr int MAIN_VIDEO_HEIGHT = 1080;

//子码流分辨率
constexpr int SUB_VIDEO_WIDTH  = 640;
constexpr int SUB_VIDEO_HEIGHT = 360;

#endif // CONST_H
