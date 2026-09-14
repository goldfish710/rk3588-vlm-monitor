#ifndef ENCODETHREAD_H
#define ENCODETHREAD_H

#include "config.h"
#include "SharedQueue.h"

// 前向声明 RKNN 上下文（在 yolo 头文件中定义，此处包含）
#include "yolov8-pose.h"

// 修改函数声明，增加 RKNN 上下文参数（主码流 OSD 缓冲用 rknn_create_mem）
void start_encode_h265(SharedQueue *shared_queue, TaskScheduler *scheduler);
void start_encode_h264(SharedQueue *shared_queue, TaskScheduler *scheduler, rknn_app_context_t *app_ctx);

#endif