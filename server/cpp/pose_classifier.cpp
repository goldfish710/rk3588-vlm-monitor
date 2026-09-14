#include "pose_classifier.h"
#include <cmath>
#include <cstdio>
#include <ctime>
#include <sys/stat.h>

// =====================================================
// Debug
// =====================================================
#define DEBUG_AI
// =====================================================
// ★ 调参指南（阈值均为经验值，按症状调整后重编 TLServer 即可）
//
// 症状                         → 调整项
// -----------------------------------------------------
// 坐姿判不出（漏判）           → SIT_ASPECT_MIN 调小(如0.45) / SIT_THIGH_FOLD 调大(如0.6)
// 站立偶尔误判坐姿（误判）      → SIT_SCORE_THRESHOLD 改为 3 / SIT_THIGH_FOLD 调小
// 远处小人被误判坐姿           → SIT_SCORE_THRESHOLD 改为 3，或只对 bbox 高度>100px 的人分类
// 跌倒漏报                     → FALL_SCORE_THRESHOLD 改为 1 / KEYPOINT_CONF_THRESHOLD 调小
// 跌倒误报（弯腰/蹲下）        → FALL_SCORE_THRESHOLD 改为 3 / 连续帧数 fall_confirm_frames 调大
// 同一人重复报警               → person_tracker.h 里 alarm_cooldown_sec 调大
//
// 标定方法：板端跑 TLServer 时看 ./ai_debug.log 每帧特征值，
// 或 PC 离线标定（server/tools/calibrate_pose.py，见 docs/技术文档.md 第15章）。
// =====================================================
// =====================================================
// Feature switch（跌倒）
// =====================================================
// 特征1: 17个关键点整体方向
#define USE_BODY_ORIENTATION 1
// 特征2: 头-脚方向
#define USE_HEAD_FEET_ANGLE 1
// 特征3: 躯干倾斜角
#define USE_TORSO_ANGLE 1
// 特征4: bbox宽高比
#define USE_ASPECT_RATIO 1
// 特征5: 身体压缩比例（预留）
#define USE_BODY_RATIO 0
// =====================================================
// Feature switch（坐姿）
// =====================================================
// 坐姿特征1: bbox宽高比适中（比站立宽，比躺倒窄）
#define USE_SIT_ASPECT 1
// 坐姿特征2: 大腿折叠比（髋-膝垂直距离远小于躯干长度）
#define USE_SIT_THIGH_FOLD 1
// 坐姿特征3: 踝髋折叠比（腿整体收起，踝-髋垂直距离缩小）
#define USE_SIT_ANKLE_HIP 1

// =====================================================
// Algorithm parameters
// =====================================================
// 关键点最低可信度
#define KEYPOINT_CONF_THRESHOLD 0.3

// -----------------------------------------------------
// 跌倒特征阈值
// -----------------------------------------------------
// 特征1: 人体整体方向比例 width/height，站立<1，躺倒>1
#define BODY_ORIENTATION_THRESHOLD 1.1
// 特征2: 头脚连线角度，90=垂直 0=水平，<45°视为躺倒
#define HEAD_FEET_ANGLE_THRESHOLD 45.0
// 特征3: 躯干与垂直线夹角，>45°视为躯干倒下
#define TORSO_ANGLE_THRESHOLD 45.0
// 特征4: bbox宽高比，>1.15视为躺倒
#define ASPECT_RATIO_THRESHOLD 1.15
// 特征5: 身体压缩比例（预留）
#define BODY_RATIO_THRESHOLD 2.0

// 跌倒投票阈值：本帧得票>=2 视为跌倒候选帧
#define FALL_SCORE_THRESHOLD 2
// 下肢不可信时（半身/遮挡）的跌倒阈值：要求更强证据（如 aspect+orient+torso 3 票），
// 防止近距离半身 bbox 天然高宽高比误判
#define FALL_SCORE_THRESHOLD_PARTIAL 3

// -----------------------------------------------------
// 坐姿特征阈值
// （几何推导初值，需真机数据标定，见 docs/技术文档.md 第8章）
// -----------------------------------------------------
// 坐姿特征1: bbox宽高比区间
// 站立~0.3-0.5 / 坐姿~0.55-1.8 / 躺倒>1.8
// （RK3588 IMX415 实测坐姿 aspect 1.17-1.74，1.15 上限误判跌倒 → 放宽至 1.8）
#define SIT_ASPECT_MIN 0.55
#define SIT_ASPECT_MAX 1.8
// 坐姿特征2: 大腿折叠比 (膝y-髋y)/躯干长
// 站立~0.6-0.9 / 坐姿<0.5
#define SIT_THIGH_FOLD_THRESHOLD 0.5
// 坐姿特征3: 踝髋折叠比 (踝y-髋y)/躯干长
// 站立>=0.9 / 坐姿<0.75
#define SIT_ANKLE_HIP_THRESHOLD 0.75

// 坐姿投票阈值：本帧得票>=2 判定坐姿
#define SIT_SCORE_THRESHOLD 2

// 坐姿腿关键点置信度阈值（独立于主体 KEYPOINT_CONF_THRESHOLD）：
// 坐姿时膝盖/脚踝被躯干遮挡或折叠，conf 天然低于 0.3，用 0.15 让坐姿投票可参与
#define SIT_LEG_CONF_THRESHOLD 0.15

// =====================================================
// YOLOv8 Pose keypoints (COCO 17点)
// =====================================================
#define NOSE 0

#define LEFT_SHOULDER 5
#define RIGHT_SHOULDER 6

#define LEFT_HIP 11
#define RIGHT_HIP 12

#define LEFT_KNEE 13
#define RIGHT_KNEE 14

#define LEFT_ANKLE 15
#define RIGHT_ANKLE 16

// =====================================================
// Log
// =====================================================
static FILE* ai_log_file = NULL;

static void open_ai_log()
{
    if (ai_log_file == NULL)
    {
        const char* path = "./ai_debug.log";
        // 超过 5MB 直接覆盖重写，防止无限膨胀
        struct stat st;
        if (stat(path, &st) == 0 && st.st_size > 5 * 1024 * 1024) {
            ai_log_file = fopen(path, "w");
        } else {
            ai_log_file = fopen(path, "a");
        }
        if (ai_log_file == NULL)
        {
            ai_log_file = stderr;
        }
    }
}

void close_ai_log()
{
    if (ai_log_file &&
       ai_log_file != stderr)
    {
        fclose(ai_log_file);
        ai_log_file = NULL;
    }
}

static void get_time_str(char* buffer, size_t size)
{
    time_t now = time(NULL);
    struct tm* tm_info = localtime(&now);
    strftime(buffer, size, "%Y-%m-%d %H:%M:%S", tm_info);
}

// =====================================================
// Geometry
// =====================================================
// 计算一条线和水平线夹角（0°~90°）
static float line_angle(float x1, float y1, float x2, float y2)
{
    float angle = atan2(fabs(y2-y1), fabs(x2-x1)) * 180.0 / M_PI;
    return angle;
}

// =====================================================
// Debug log helper：把本帧全部特征值写入 ai_debug.log + 控制台
// =====================================================
static void log_pose_result(float body_orientation_ratio, float head_feet_angle,
                            float torso_angle, float aspect_ratio, float body_ratio,
                            float sit_thigh_fold, float sit_ankle_hip,
                            int fall_score, int sit_score, const char* state_name)
{
#ifdef DEBUG_AI
    open_ai_log();
    char time_str[32];
    get_time_str(time_str, sizeof(time_str));
    fprintf(ai_log_file,
            "[%s] orient=%.2f head_angle=%.1f torso=%.1f aspect=%.2f "
            "body_ratio=%.2f thigh_fold=%.2f ankle_hip=%.2f "
            "fall_score=%d sit_score=%d state=%s\n",
            time_str, body_orientation_ratio, head_feet_angle, torso_angle,
            aspect_ratio, body_ratio, sit_thigh_fold, sit_ankle_hip,
            fall_score, sit_score, state_name);
    fflush(ai_log_file);
    printf("[POSE] orient %.2f head %.1f torso %.1f aspect %.2f "
           "thigh %.2f ankle_hip %.2f score %d sit %d %s\n",
           body_orientation_ratio, head_feet_angle, torso_angle,
           aspect_ratio, sit_thigh_fold, sit_ankle_hip,
           fall_score, sit_score, state_name);
#endif
}

// =====================================================
// Main classifier（单帧、无状态）
// =====================================================
PoseState classify_pose(const object_detect_result &person)
{
    const float (*kp)[3] = person.keypoints;

    // -------------------------------------------------
    // 基础检查：双肩双髋任一不可信 → 本帧不可判定
    // （宁可漏判不误判，确认逻辑由追踪器按帧累积）
    // -------------------------------------------------
    if (kp[LEFT_SHOULDER][2] < KEYPOINT_CONF_THRESHOLD ||
        kp[RIGHT_SHOULDER][2] < KEYPOINT_CONF_THRESHOLD ||
        kp[LEFT_HIP][2] < KEYPOINT_CONF_THRESHOLD ||
        kp[RIGHT_HIP][2] < KEYPOINT_CONF_THRESHOLD)
    {
        return POSE_UNKNOWN;
    }

    int fall_score = 0;
    int sit_score = 0;

    float body_orientation_ratio = 0;
    float head_feet_angle = 90;
    float torso_angle = 0;
    float aspect_ratio = 0;
    float body_ratio = 0;

    float sit_thigh_fold = 0;
    float sit_ankle_hip = 0;

    // =================================================
    // 跌倒特征1: 17关键点整体方向
    // =================================================
#if USE_BODY_ORIENTATION
    float min_x = 99999, max_x = -99999;
    float min_y = 99999, max_y = -99999;

    for (int i = 0; i < 17; i++)
    {
        if (kp[i][2] < KEYPOINT_CONF_THRESHOLD)
            continue;

        if (kp[i][0] < min_x) min_x = kp[i][0];
        if (kp[i][0] > max_x) max_x = kp[i][0];
        if (kp[i][1] < min_y) min_y = kp[i][1];
        if (kp[i][1] > max_y) max_y = kp[i][1];
    }

    float body_width = max_x - min_x;
    float body_height = max_y - min_y;

    body_orientation_ratio = body_width / (body_height + 0.001);

    if (body_orientation_ratio > BODY_ORIENTATION_THRESHOLD)
    {
        fall_score++;
    }
#endif

    // =================================================
    // 跌倒特征2: 头脚方向
    // =================================================
#if USE_HEAD_FEET_ANGLE
    if (kp[NOSE][2] > KEYPOINT_CONF_THRESHOLD &&
        kp[LEFT_ANKLE][2] > KEYPOINT_CONF_THRESHOLD &&
        kp[RIGHT_ANKLE][2] > KEYPOINT_CONF_THRESHOLD)
    {
        float ankle_x = (kp[LEFT_ANKLE][0] + kp[RIGHT_ANKLE][0]) / 2.0;
        float ankle_y = (kp[LEFT_ANKLE][1] + kp[RIGHT_ANKLE][1]) / 2.0;

        head_feet_angle = line_angle(kp[NOSE][0], kp[NOSE][1], ankle_x, ankle_y);

        if (head_feet_angle < HEAD_FEET_ANGLE_THRESHOLD)
        {
            fall_score++;
        }
    }
#endif

    // =================================================
    // 跌倒特征3: 躯干角度
    // =================================================
#if USE_TORSO_ANGLE
    float shoulder_x = (kp[LEFT_SHOULDER][0] + kp[RIGHT_SHOULDER][0]) / 2;
    float shoulder_y = (kp[LEFT_SHOULDER][1] + kp[RIGHT_SHOULDER][1]) / 2;

    float hip_x = (kp[LEFT_HIP][0] + kp[RIGHT_HIP][0]) / 2;
    float hip_y = (kp[LEFT_HIP][1] + kp[RIGHT_HIP][1]) / 2;

    torso_angle = atan2(fabs(shoulder_x - hip_x), fabs(shoulder_y - hip_y)) * 180.0 / M_PI;

    if (torso_angle > TORSO_ANGLE_THRESHOLD)
    {
        fall_score++;
    }
#endif

    // =================================================
    // 跌倒特征4: bbox宽高比
    // =================================================
#if USE_ASPECT_RATIO
    float box_w = person.box.right - person.box.left;
    float box_h = person.box.bottom - person.box.top;

    aspect_ratio = box_w / (box_h + 0.001);

    if (aspect_ratio > ASPECT_RATIO_THRESHOLD)
    {
        fall_score++;
    }
#endif

    // =================================================
    // 跌倒特征5: 身体压缩比例（预留）
    // =================================================
#if USE_BODY_RATIO
    // 后续测试添加
#endif

    // =================================================
    // 下肢完整性（坐姿投票与跌倒证据强度共用）：
    // 近距离半身/下肢被遮挡时 legs_valid=false → 坐姿不可判、跌倒需更强证据、
    // 无强证据时判 UNKNOWN（防止半身 bbox 天然高宽高比被误判跌倒）
    // =================================================
    bool legs_valid =
        kp[LEFT_KNEE][2] > SIT_LEG_CONF_THRESHOLD &&
        kp[RIGHT_KNEE][2] > SIT_LEG_CONF_THRESHOLD &&
        kp[LEFT_ANKLE][2] > SIT_LEG_CONF_THRESHOLD &&
        kp[RIGHT_ANKLE][2] > SIT_LEG_CONF_THRESHOLD;

    // =================================================
    // 坐姿特征（先于跌倒判定：坐姿是明确状态，命中即排除跌倒；
    // 原"跌倒优先"导致坐姿 aspect 1.17-1.74 撞跌倒阈值 1.15 被误判）
    // =================================================
#if USE_SIT_ASPECT || USE_SIT_THIGH_FOLD || USE_SIT_ANKLE_HIP
    {
        // 躯干垂直长度（肩-髋，y轴向下为正，站立时>0）
        float torso_len = fabs(hip_y - shoulder_y);

        float knee_y = (kp[LEFT_KNEE][1] + kp[RIGHT_KNEE][1]) / 2;
        float ankle_y = (kp[LEFT_ANKLE][1] + kp[RIGHT_ANKLE][1]) / 2;

#if USE_SIT_ASPECT
        // 坐姿特征1: bbox宽高比落在"偏方"区间
        if (aspect_ratio >= SIT_ASPECT_MIN && aspect_ratio <= SIT_ASPECT_MAX)
        {
            sit_score++;
        }
#endif

#if USE_SIT_THIGH_FOLD
        // 坐姿特征2: 大腿接近水平（髋-膝垂直距离远小于躯干）
        if (legs_valid && knee_y > hip_y && torso_len > 0.001)
        {
            sit_thigh_fold = (knee_y - hip_y) / torso_len;
            if (sit_thigh_fold < SIT_THIGH_FOLD_THRESHOLD)
            {
                sit_score++;
            }
        }
#endif

#if USE_SIT_ANKLE_HIP
        // 坐姿特征3: 腿整体收起（踝-髋垂直距离缩小）
        if (legs_valid && ankle_y > hip_y && torso_len > 0.001)
        {
            sit_ankle_hip = (ankle_y - hip_y) / torso_len;
            if (sit_ankle_hip < SIT_ANKLE_HIP_THRESHOLD)
            {
                sit_score++;
            }
        }
#endif
    }
#endif

    // =================================================
    // 决策1: 坐姿（明确状态，命中即排除跌倒）
    // =================================================
    if (sit_score >= SIT_SCORE_THRESHOLD)
    {
        log_pose_result(body_orientation_ratio, head_feet_angle, torso_angle,
                        aspect_ratio, body_ratio, sit_thigh_fold, sit_ankle_hip,
                        fall_score, sit_score, "SITTING");
        return POSE_SITTING;
    }

    // =================================================
    // 决策2: 跌倒（安全第一；下肢不可信时要求更强证据——
    // 半身 bbox 天然高宽高比，需 3 票才能判跌倒）
    // =================================================
    int fall_threshold = legs_valid ? FALL_SCORE_THRESHOLD
                                    : FALL_SCORE_THRESHOLD_PARTIAL;
    if (fall_score >= fall_threshold)
    {
        log_pose_result(body_orientation_ratio, head_feet_angle, torso_angle,
                        aspect_ratio, body_ratio, sit_thigh_fold, sit_ankle_hip,
                        fall_score, sit_score, "FALL_CANDIDATE");
        return POSE_FALLING;
    }

    // =================================================
    // 决策3: 下肢不可信 → 未知（半身不可判状态，避免当站立）
    // =================================================
    if (!legs_valid)
    {
        log_pose_result(body_orientation_ratio, head_feet_angle, torso_angle,
                        aspect_ratio, body_ratio, sit_thigh_fold, sit_ankle_hip,
                        fall_score, sit_score, "PARTIAL_UNKNOWN");
        return POSE_UNKNOWN;
    }

    // =================================================
    // 决策4: 站立
    // =================================================
    log_pose_result(body_orientation_ratio, head_feet_angle, torso_angle,
                    aspect_ratio, body_ratio, sit_thigh_fold, sit_ankle_hip,
                    fall_score, sit_score, "NORMAL");
    return POSE_STANDING;
}

const char* pose_state_name(PoseState state)
{
    switch (state)
    {
        case POSE_FALLING:
            return "falling";
        case POSE_SITTING:
            return "sitting";
        case POSE_STANDING:
            return "normal";
        default:
            return "unknown";
    }
}
