// =====================================================
// C ABI 封装：让 Python(ctypes) 调用真正的 C++ 分类逻辑
//
// 编译（本机 gcc，无需交叉工具链）：
//   g++ -std=c++11 -fPIC -shared -O2 \
//       -I../cpp -I../cpp/3rdparty/rknpu2/include -I../cpp/utils \
//       ../cpp/pose_classifier.cpp ../cpp/person_tracker.cpp \
//       pose_classify_wrapper.cpp -o libpose_classify.so -pthread
//
// 目的：离线标定阈值时不存在"Python 镜像版"逻辑，
//      阈值只活在 C++ 一处，标定结果与板端行为完全一致。
// =====================================================
#include "pose_classifier.h"
#include <cstring>

extern "C" {

// 单帧单人分类
// kps: 17x3 连续内存 [x, y, conf]
// box: [left, top, right, bottom]
// 返回: PoseState 枚举值 (0=UNKNOWN 1=STANDING 2=SITTING 3=FALLING)
int pose_classify(const float* kps, const int* box)
{
    object_detect_result det;
    memset(&det, 0, sizeof(det));
    memcpy(det.keypoints, kps, 17 * 3 * sizeof(float));
    det.box.left   = box[0];
    det.box.top    = box[1];
    det.box.right  = box[2];
    det.box.bottom = box[3];
    return (int)classify_pose(det);
}

// 姿态枚举转字符串（绘图标签用，与板端一致）
const char* pose_state_name_c(int state)
{
    return pose_state_name((PoseState)state);
}

} // extern "C"
