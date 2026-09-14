#ifndef POSE_CLASSIFIER_H
#define POSE_CLASSIFIER_H
#include "yolov8-pose.h"

// =====================================================
// 姿态状态（单帧分类结果）
//
// POSE_FALLING 表示"本帧投跌倒票"，
// 连续帧确认/报警冷却等逻辑已移至 PersonTracker。
// =====================================================
enum PoseState
{
    POSE_UNKNOWN = 0,
    POSE_STANDING,
    POSE_SITTING,
    POSE_FALLING
};

// 单帧姿态分类（无状态、可并发调用）
// 输入一个人的检测结果（框 + 17关键点），输出本帧姿态
PoseState classify_pose(const object_detect_result &person);

const char* pose_state_name(PoseState state);

// 关闭 AI 调试日志文件
void close_ai_log();

#endif
