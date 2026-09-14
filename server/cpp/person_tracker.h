#ifndef PERSON_TRACKER_H
#define PERSON_TRACKER_H

#include "yolov8-pose.h"
#include "pose_classifier.h"
#include <vector>
#include <mutex>
#include <ctime>
#include <cstdint>
#include <sys/time.h>

// =====================================================
// 多人追踪 + 逐人跌倒确认（本次新增）
//
// 背景：原实现把"连续帧确认"的计数器做成全局变量，
// 多人场景下不同人的投票互相干扰（A投一票B清零）。
// 本追踪器为每个"人"维护独立的投票/冷却状态：
//   IoU 匹配 → 每人独立 fall_streak/sit_streak → 确认后产生事件
// =====================================================

// 一次已确认的跌倒事件（由追踪器产生）
struct FallEvent {
    int person_id;          // 跌倒者的追踪 ID
    float confidence;       // 跌倒者本人的检测置信度（修正：原来取的是循环最后一个人）
    time_t timestamp;       // 确认时间
};

// 追踪到的人（供绘图使用）
struct TrackedPerson {
    int id;                         // 追踪 ID
    object_detect_result det;       // 最近一帧检测结果（框+关键点）
    PoseState pose;                 // 当前显示姿态（连续帧确认后的稳定状态）
    int fall_streak = 0;            // 连续跌倒候选帧数
    int sit_streak = 0;             // 连续坐姿帧数
    time_t last_alarm = 0;          // 该人上次报警时间（冷却）
    int lost_frames = 0;            // 连续未匹配帧数（宽限期内保留，防框闪烁/ID 抖动）
    int recover_streak = 0;         // 连续非跌倒帧数：达到 recovery_frames 才算"已恢复"
                                    // （迟滞机制——防阈值边界 FALLING↔SITTING 抖动
                                    // 把武装位洗回去，导致持续倒地无限重报）
    bool alarm_armed = true;        // 是否允许产生新跌倒事件：
                                    // 报警后置 false，需恢复确认才重新武装——
                                    // 同一次跌倒只报一次（静态跌倒画面不再每 15s 无限重报）
};

class PersonTracker {
public:
    PersonTracker() = default;

    // 每帧调用一次（AI 线程）：
    //   1. 过滤出人（cls_id==0）
    //   2. 单帧姿态分类 classify_pose
    //   3. 与上一帧追踪结果做 IoU 贪心匹配
    //   4. 逐人更新投票/冷却，确认跌倒则产生事件
    // 返回本帧确认的跌倒事件列表（可能为空）
    std::vector<FallEvent> update(const object_detect_result_list& dets);

    // 获取当前追踪列表快照（绘图线程用，内部加锁）
    std::vector<TrackedPerson> getPersons() const;

    // 检测滞后诊断：最近一次 update() 距今毫秒数（绘制线程算"框旧了多久"）
    int64_t lastUpdateAgeMs() const;

    // 可调参数（默认值见 .cpp）
    int fall_confirm_frames = 5;    // 连续多少帧确认跌倒
    int sit_confirm_frames = 3;     // 连续多少帧确认坐姿（显示用）
    int alarm_cooldown_sec = 15;    // 单人报警冷却
    int global_cooldown_sec = 15;   // 全局报警冷却（防 ID 丢失重分配后重复报警）
    int lost_grace_frames = 60;     // 漏检宽限帧数（AI 15fps 下 ≈4s）：期间保留追踪
                                    // 目标并继续参与匹配，防漏检删人→新人重加导致的
                                    // 框闪烁与 ID 抖动。实测照片被 YOLO 间歇性检测
                                    // （出现 1-2s 又消失 1-2s），15 帧宽限正好卡边界
                                    // → 必须显著大于检测间歇
    int recovery_frames = 15;       // 恢复确认帧数（AI 15fps 下 ≈1s）：连续这么多帧
                                    // 非跌倒才算"已恢复"（重新武装 + 退出跌倒显示态）。
                                    // 实测阈值边界抖动的 SITTING 连续段可达 3-4 帧，
                                    // 3 帧阈值仍会被洗武装 → 取 1s 级确认
    int scene_quiet_clear_sec = 30; // 场景解除条件②（秒）：连续这么久无任何跌倒分类检测
                                    // （场景清空/照片拿走/事故结束）
    int scene_latch_max_sec = 300;  // 场景解除条件③（秒）：挂起硬超时——持续倒地场景
                                    // 最多 5 分钟强制解除一次（持续倒地每 5 分钟重提醒，
                                    // 防"换人倒地"永久漏报；用墙钟，帧率无关）
    float iou_match_threshold = 0.3f;

private:
    static float bboxIoU(const object_detect_result& a, const object_detect_result& b);

    // 单人状态机：投票累计/清零 + 更新显示姿态
    void updatePersonState(TrackedPerson& person, PoseState frame_pose, time_t now);

    std::vector<TrackedPerson> persons_;
    time_t last_global_alarm_ = 0;
    int next_id_ = 1;
    // AI 线程最近一次 update 的墙钟毫秒（绘制线程算检测滞后）
    int64_t last_update_ms_ = 0;

    // 场景级报警挂起（保险丝）：一次报警后挂起，三个解除条件任一满足即解除：
    //   ① 有追踪目标完成恢复确认（连续 ≈1s 站/坐）且无人处于确认跌倒态
    //      ——"起身后再跌倒=新事件"，旋转照片测试的预期语义
    //   ② 连续 scene_quiet_clear_sec 秒无任何跌倒分类检测（场景清空/事故结束）
    //   ③ scene_latch_max_sec 硬超时（持续倒地最多每 5 分钟重提醒一次）
    // 挂起期间的一切跌倒确认都不产生事件（多人在同一事故中相继倒地也只报一次）
    bool scene_fall_pending_ = false;
    time_t scene_fall_begin_ts_ = 0;    // 挂起起始（硬超时用）
    time_t last_falling_seen_ts_ = 0;   // 最近一次任何候选被分类为跌倒的时刻（条件②用）
    mutable std::mutex mtx_;
};

#endif
