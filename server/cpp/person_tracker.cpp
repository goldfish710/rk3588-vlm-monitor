#include "person_tracker.h"
#include "Logger.h"
#include <algorithm>
#include <cmath>

// =====================================================
// IoU（交并比）：两框重叠程度，1=完全重合，0=不相交
// =====================================================
float PersonTracker::bboxIoU(const object_detect_result& a, const object_detect_result& b)
{
    float ix1 = std::max((float)a.box.left, (float)b.box.left);
    float iy1 = std::max((float)a.box.top, (float)b.box.top);
    float ix2 = std::min((float)a.box.right, (float)b.box.right);
    float iy2 = std::min((float)a.box.bottom, (float)b.box.bottom);

    float iw = std::max(0.0f, ix2 - ix1);
    float ih = std::max(0.0f, iy2 - iy1);
    float inter = iw * ih;
    if (inter <= 0.0f) return 0.0f;

    float area_a = (float)(a.box.right - a.box.left) * (a.box.bottom - a.box.top);
    float area_b = (float)(b.box.right - b.box.left) * (b.box.bottom - b.box.top);
    float uni = area_a + area_b - inter;
    if (uni <= 0.0f) return 0.0f;

    return inter / uni;
}

// =====================================================
// 单帧更新（AI 线程调用）
// =====================================================
std::vector<FallEvent> PersonTracker::update(const object_detect_result_list& dets)
{
    std::lock_guard<std::mutex> lock(mtx_);
    time_t now = time(NULL);
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    last_update_ms_ = (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;

    // ---------- 1. 过滤出人 + 单帧姿态分类 ----------
    struct Cand {
        object_detect_result det;
        PoseState pose;
        bool matched = false;
    };
    std::vector<Cand> cands;
    bool any_falling_det = false;   // 本帧是否有任何候选被分类为跌倒（场景挂起解除判据）
    for (int i = 0; i < dets.count; i++)
    {
        if (dets.results[i].cls_id != 0) continue;
        Cand c;
        c.det = dets.results[i];
        c.pose = classify_pose(c.det);
        if (c.pose == POSE_FALLING) any_falling_det = true;
        cands.push_back(c);
    }

    // ---------- 2. IoU 贪心匹配（AI 帧率~15fps，人数少，贪心足够） ----------
    std::vector<bool> prev_matched(persons_.size(), false);

    // 循环取全矩阵最大 IoU 对，超过阈值则配对（已配对的行列跳过）
    while (true)
    {
        float best_iou = iou_match_threshold;
        int best_p = -1, best_c = -1;
        for (size_t p = 0; p < persons_.size(); p++)
        {
            if (prev_matched[p]) continue;
            for (size_t c = 0; c < cands.size(); c++)
            {
                if (cands[c].matched) continue;
                float iou = bboxIoU(persons_[p].det, cands[c].det);
                if (iou > best_iou)
                {
                    best_iou = iou;
                    best_p = (int)p;
                    best_c = (int)c;
                }
            }
        }
        if (best_p < 0 || best_c < 0) break;
        prev_matched[best_p] = true;
        cands[best_c].matched = true;

        TrackedPerson& person = persons_[best_p];
        person.det = cands[best_c].det;
        person.lost_frames = 0;
        updatePersonState(person, cands[best_c].pose, now);
    }

    // ---------- 3. 未匹配旧人：漏检宽限期（防单帧漏检即删人重加） ----------
    // 宽限期内保留追踪目标并继续参与下一帧匹配：单帧漏检（运动模糊/遮挡/NMS
    // 抖动）不再导致框消失、ID 重分配、投票清零——实测旧逻辑 ID 涨到 236
    // （人反复删除新建），子码流观感即"框闪烁"
    // 注意：必须在添加新人之前执行，保证 prev_matched 下标与 persons_ 一一对应
    size_t idx = 0;
    persons_.erase(std::remove_if(persons_.begin(), persons_.end(),
                                  [&](TrackedPerson &p) {
                                      bool matched = prev_matched[idx];
                                      idx++;
                                      if (!matched) p.lost_frames++;
                                      return p.lost_frames > lost_grace_frames;
                                  }),
                   persons_.end());

    // ---------- 4. 未匹配的检测 → 新人（先做"子框判定"，双向） ----------
    // 同一人身上检测出"框内框"（实测脸部被误检成人，IoU 与外框低到匹配不上
    // → 被当成新人，多出一个 ID 且可能重复计票）。判据：内 IoU（交集/两框较小
    // 面积）> 0.6，分两个方向处理：
    //   ① 新框更小被已有框包住 → 子框误检，丢弃（脸框压在全身框内）
    //   ② 新框更大包住已有框 → 已有框才是子框误检（脸先于全身被检测到的
    //      场景：首帧只有脸框入追踪，全身框后到，若按①单向丢弃，全身框
    //      被误杀、人永远被脸框跟踪）→ 吸收：全身框接管旧跟踪器（ID 不变）
    // 两个真人部分重叠时内 IoU 小，不受影响
    std::vector<FallEvent> events;
    for (auto& c : cands)
    {
        if (c.matched) continue;
        bool sub_box = false;
        bool absorbed = false;
        float c_area = (float)(c.det.box.right - c.det.box.left) *
                       (c.det.box.bottom - c.det.box.top);
        for (auto& p : persons_)
        {
            float ix1 = std::max((float)c.det.box.left, (float)p.det.box.left);
            float iy1 = std::max((float)c.det.box.top, (float)p.det.box.top);
            float ix2 = std::min((float)c.det.box.right, (float)p.det.box.right);
            float iy2 = std::min((float)c.det.box.bottom, (float)p.det.box.bottom);
            if (ix2 <= ix1 || iy2 <= iy1) continue;
            float inter = (ix2 - ix1) * (iy2 - iy1);
            float p_area = (float)(p.det.box.right - p.det.box.left) *
                           (p.det.box.bottom - p.det.box.top);
            if (inter / std::min(c_area, p_area) <= 0.6f) continue;
            if (c_area > p_area)
            {
                // ② 吸收：全身框接管旧跟踪器，保持 ID 与投票状态
                p.det = c.det;
                p.lost_frames = 0;
                updatePersonState(p, c.pose, now);
                absorbed = true;
                LOG_DEBUG("子框吸收：全身框接管 id=%d", p.id);
            }
            else
            {
                sub_box = true;
            }
            break;
        }
        if (absorbed) continue;
        if (sub_box)
        {
            LOG_DEBUG("子框误检抑制（内 IoU>0.6）");
            continue;
        }
        TrackedPerson person;
        person.id = next_id_++;
        person.det = c.det;
        person.pose = POSE_STANDING;
        updatePersonState(person, c.pose, now);
        persons_.push_back(person);
        LOG_DEBUG("新增追踪目标 id=%d", person.id);
    }

    // ---------- 5. 跌倒确认 + 武装 + 场景挂起 + 冷却 → 生成事件 ----------
    // 三道闸门（同一次跌倒只报一次）：
    //   alarm_armed（单人）：报警后须恢复确认（连续 recovery_frames 帧非跌倒）才重新武装
    //   scene_fall_pending_（场景保险丝）：报警后挂起，直到连续 scene_clear_sec 秒
    //     画面无任何跌倒分类检测才解除；挂起期间出生的新目标默认未武装
    //   cooldown（15s）：防瞬时重复
    for (auto& p : persons_)
    {
        if (p.fall_streak >= fall_confirm_frames && p.alarm_armed && !scene_fall_pending_)
        {
            if (now - p.last_alarm >= alarm_cooldown_sec &&
                now - last_global_alarm_ >= global_cooldown_sec)
            {
                p.last_alarm = now;
                last_global_alarm_ = now;
                p.alarm_armed = false;
                scene_fall_pending_ = true;
                scene_fall_begin_ts_ = now;
                last_falling_seen_ts_ = now;

                FallEvent ev;
                ev.person_id = p.id;
                ev.confidence = p.det.prop;   // 修正：跌倒者本人的置信度
                ev.timestamp = now;
                events.push_back(ev);
                LOG_INFO("跌倒确认: person_id=%d confidence=%.2f", ev.person_id, ev.confidence);
            }
            else
            {
                LOG_DEBUG("跌倒确认但处于冷却期: person_id=%d", p.id);
            }
        }
    }

    // ---------- 6. 场景挂起解除（三个条件任一满足） ----------
    if (scene_fall_pending_)
    {
        if (any_falling_det)
            last_falling_seen_ts_ = now;   // 仍有跌倒迹象：重置条件②计时

        // 条件①：有目标完成恢复确认（连续 ≈1s 站/坐）且无人处于确认跌倒态
        // ——"起身后再跌倒=新事件"（用户旋转照片测试的预期：正常 1s 即解除，
        // 转回跌倒立刻可报）；照片抖动场景的 SITTING 连续段远达不到 1s
        bool someone_recovered = false, any_confirmed = false;
        for (auto& p : persons_)
        {
            if (p.recover_streak >= recovery_frames) someone_recovered = true;
            if (p.fall_streak >= fall_confirm_frames) any_confirmed = true;
        }
        bool clear_by_recover = someone_recovered && !any_confirmed;
        // 条件②：场景清静（拿走照片/事故结束）
        bool clear_by_quiet = (now - last_falling_seen_ts_ >= scene_quiet_clear_sec);
        // 条件③：硬超时（持续倒地最多每 5 分钟重提醒一次）
        bool clear_by_timeout = (now - scene_fall_begin_ts_ >= scene_latch_max_sec);

        if (clear_by_recover || clear_by_quiet || clear_by_timeout)
        {
            if (clear_by_timeout) {
                // 硬超时=持续倒地重提醒：重新武装全部目标（持续倒地的人从未
                // 恢复、armed 恒 false，不重新武装的话 5 分钟重提醒发不出去）
                for (auto& p : persons_) p.alarm_armed = true;
            }
            scene_fall_pending_ = false;
            LOG_INFO("场景恢复：挂起解除（恢复=%d 清静=%d 超时=%d）",
                     clear_by_recover ? 1 : 0, clear_by_quiet ? 1 : 0, clear_by_timeout ? 1 : 0);
        }
    }

    return events;
}

// =====================================================
// 单人状态机：投票累计 / 清零 / 显示姿态
// =====================================================
void PersonTracker::updatePersonState(TrackedPerson& person, PoseState frame_pose, time_t now)
{
    (void)now;

    // 关键点不可信（UNKNOWN）：保守处理，投票清零（宁可漏报不误报）
    if (frame_pose == POSE_UNKNOWN)
    {
        person.fall_streak = 0;
        person.sit_streak = 0;
        person.pose = POSE_UNKNOWN;
        return;
    }

    // 跌倒投票
    if (frame_pose == POSE_FALLING)
    {
        person.fall_streak++;
        person.recover_streak = 0;
    }
    else
    {
        person.fall_streak = 0;
        // 恢复确认（迟滞）：连续 recovery_frames(≈1s) 帧非跌倒才算"已恢复"并重新武装。
        // POSE_UNKNOWN 不参与（顶部早退）；阈值边界抖动的 SITTING 连续段实测可达 3-4 帧，
        // 1s 级确认才足以不被抖掉（3 帧阈值曾被照片场景洗武装 → 每 15s 重报）
        person.recover_streak++;
        if (person.recover_streak >= recovery_frames)
            person.alarm_armed = true;
    }

    // 坐姿投票（仅未跌倒时）
    if (frame_pose == POSE_SITTING)
    {
        person.sit_streak++;
    }
    else
    {
        person.sit_streak = 0;
    }

    // 显示姿态：跌倒锁定（粘性）——一旦进入跌倒显示态，需连续
    // recovery_frames(≈1s) 帧非跌倒才退出。防阈值边界抖动导致框颜色/标签
    // 红蓝频闪（子码流"一闪一闪"观感的来源：抖动期每 0.3-0.5s 红蓝翻转）
    if (person.fall_streak >= fall_confirm_frames)
    {
        person.pose = POSE_FALLING;
    }
    else if (person.pose == POSE_FALLING && person.recover_streak < recovery_frames)
    {
        // 恢复确认中：保持跌倒显示（粘性）
    }
    else if (person.sit_streak >= sit_confirm_frames)
    {
        person.pose = POSE_SITTING;
    }
    else
    {
        person.pose = POSE_STANDING;
    }
}

// =====================================================
// 追踪列表快照（绘图线程用）
// =====================================================
std::vector<TrackedPerson> PersonTracker::getPersons() const
{
    std::lock_guard<std::mutex> lock(mtx_);
    return persons_;
}

int64_t PersonTracker::lastUpdateAgeMs() const
{
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    int64_t now_ms = (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
    std::lock_guard<std::mutex> lock(mtx_);
    return last_update_ms_ ? (now_ms - last_update_ms_) : 0;
}
