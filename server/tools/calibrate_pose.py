#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
姿态分类离线标定工具（调用真正的 C++ 逻辑，无镜像拷贝）
======================================================

原理：libpose_classify.so 由 server/cpp/pose_classifier.cpp + person_tracker.cpp
用本机 gcc 编译而来，Python 通过 ctypes 调用——阈值只活在 C++ 一处，
标定结果与板端行为完全一致。

特征值来源：C++ classify_pose 的 DEBUG_AI 会把每帧全部特征值写入
./ai_debug.log（与板端行为一致），本工具解析该日志做分布统计。

数据来源：pose.py --dump 连板/模拟器推理真实 yolov8n-pose.rknn，
导出每个人的 (框 + 17关键点) 到 .npz。

用法：
  python calibrate_pose.py --sanity                 # 合成关键点走真 C++ 逻辑自检
  python calibrate_pose.py --dump dump_out.npz      # 批量分类 + 特征分布统计
  python calibrate_pose.py --dump dump_out.npz --csv features.csv   # 额外导出 CSV

编译 .so（本机 gcc）：
  g++ -std=c++11 -fPIC -shared -O2 \
      -I../cpp -I../cpp/3rdparty/rknpu2/include -I../cpp/utils \
      ../cpp/pose_classifier.cpp ../cpp/person_tracker.cpp ../cpp/Logger.cpp \
      pose_classify_wrapper.cpp -o libpose_classify.so -pthread
"""

import argparse
import ctypes
import os
import re
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
SO_PATH = os.path.join(HERE, "libpose_classify.so")
AI_LOG = os.path.join(HERE, "ai_debug.log")   # C++ 写入的特征日志

POSE_NAMES = {0: "unknown", 1: "normal", 2: "sitting", 3: "falling"}

# C++ ai_debug.log 行格式（pose_classifier.cpp 的 log_pose_result fprintf 版）：
#   [时间] orient=0.25 head_angle=89.1 torso=2.3 aspect=0.38 body_ratio=0.00
#          thigh_fold=0.77 ankle_hip=1.51 fall_score=0 sit_score=0 state=NORMAL
POSE_LINE_RE = re.compile(
    r"orient=([\d.]+) head_angle=([\d.]+) torso=([\d.]+) aspect=([\d.]+) "
    r"body_ratio=([\d.]+) thigh_fold=([\d.]+) ankle_hip=([\d.]+) "
    r"fall_score=(\d+) sit_score=(\d+) state=(\w+)")

FEATURES = ["orientation", "head_angle", "torso", "aspect", "thigh_fold", "ankle_hip"]


class PoseClassifier:
    def __init__(self):
        if not os.path.exists(SO_PATH):
            print("找不到 %s，请先按文件头注释编译 .so" % SO_PATH)
            sys.exit(1)
        self.lib = ctypes.CDLL(SO_PATH)
        self.lib.pose_classify.argtypes = [
            ctypes.POINTER(ctypes.c_float),          # kps 17x3
            ctypes.POINTER(ctypes.c_int)]            # box [l,t,r,b]
        self.lib.pose_classify.restype = ctypes.c_int
        self.lib.pose_state_name_c.argtypes = [ctypes.c_int]
        self.lib.pose_state_name_c.restype = ctypes.c_char_p

    def classify(self, kps, box):
        kps_arr = np.ascontiguousarray(kps, dtype=np.float32)
        box_arr = np.ascontiguousarray(box, dtype=np.int32)
        return self.lib.pose_classify(
            kps_arr.ctypes.data_as(ctypes.POINTER(ctypes.c_float)),
            box_arr.ctypes.data_as(ctypes.POINTER(ctypes.c_int)))

    def state_name(self, state):
        return self.lib.pose_state_name_c(state).decode()


def rotate_ai_log():
    """清空旧特征日志（C++ 追加写入，先删掉避免统计到旧数据）"""
    if os.path.exists(AI_LOG):
        os.remove(AI_LOG)


def make_kps(pts, conf=0.9):
    """合成关键点：只给指定点高置信度，其余点 (0,0) 低置信度（与真实数据一致）"""
    kps = np.zeros((17, 3), dtype=np.float32)
    kps[:, 2] = 0.05
    for i, (x, y) in pts.items():
        kps[i, 0], kps[i, 1], kps[i, 2] = x, y, conf
    return kps


def run_sanity():
    print("=" * 60)
    print("合成关键点自检（走真正的 C++ classify_pose）")
    print("=" * 60)
    cases = [
        ("站立", make_kps({0: (320, 60), 5: (310, 160), 6: (330, 160),
                           11: (310, 320), 12: (330, 320),
                           13: (310, 470), 14: (330, 470),
                           15: (310, 620), 16: (330, 620)}),
         (280, 20, 360, 640), 1),
        ("坐姿(正面)", make_kps({0: (320, 180), 5: (295, 250), 6: (345, 250),
                                 11: (295, 330), 12: (345, 330),
                                 13: (260, 345), 14: (380, 345),
                                 15: (260, 420), 16: (380, 420)}),
         (230, 140, 410, 440), 2),
        ("躺倒", make_kps({0: (500, 310), 5: (220, 300), 6: (260, 300),
                           11: (360, 300), 12: (400, 300),
                           13: (440, 300), 14: (480, 300),
                           15: (490, 310), 16: (500, 310)}),
         (200, 280, 520, 330), 3),
        # 腿部低置信度：只有 bbox 一票 → 应判 normal
        ("坐姿(腿部置信度低)", make_kps({0: (320, 180), 5: (295, 250), 6: (345, 250),
                                         11: (295, 330), 12: (345, 330)}),
         (230, 140, 410, 440), 1),
        # 肩部低置信度 → unknown
        ("右肩置信度低", make_kps({0: (320, 60), 5: (310, 160),
                                   11: (310, 320), 12: (330, 320)}),
         (280, 20, 360, 640), 0),
    ]

    clf = PoseClassifier()
    passed = 0
    for name, kps, box, expect in cases:
        state = clf.classify(kps, box)
        ok = (state == expect)
        passed += ok
        print("[%s] %-16s → %-8s (期望 %s)" % ("PASS" if ok else "FAIL", name,
                                               clf.state_name(state), POSE_NAMES[expect]))
    print("结果: %d/%d 通过" % (passed, len(cases)))
    print("(每帧特征值见 %s)" % AI_LOG)
    return passed == len(cases)


def parse_ai_log():
    rows = []
    if not os.path.exists(AI_LOG):
        return rows
    with open(AI_LOG, "r") as f:
        for line in f:
            m = POSE_LINE_RE.search(line)
            if m:
                g = m.groups()
                rows.append({
                    "orientation": float(g[0]), "head_angle": float(g[1]),
                    "torso": float(g[2]), "aspect": float(g[3]),
                    "thigh_fold": float(g[5]), "ankle_hip": float(g[6]),
                    "fall_score": int(g[7]), "sit_score": int(g[8]),
                    "state": g[9],
                })
    return rows


def run_batch(dump_path, csv_path):
    data = np.load(dump_path, allow_pickle=True)
    boxes = data["boxes"]        # (n, 4)
    kpss = data["keypoints"]     # (n, 17, 3)
    print("加载 %s: %d 个人体样本" % (dump_path, len(boxes)))

    clf = PoseClassifier()
    rotate_ai_log()
    states = [clf.classify(kpss[i], boxes[i]) for i in range(len(boxes))]
    rows = parse_ai_log()

    # ---------- 分类结果统计 ----------
    print("\n分类结果:")
    for s in sorted(set(states)):
        print("  %-8s: %d 人" % (POSE_NAMES[s], states.count(s)))

    if not rows:
        print("\n(ai_debug.log 无特征行——检查 C++ 是否定义了 DEBUG_AI)")
        return

    # ---------- 特征分布统计（按预测状态分组） ----------
    print("\n特征分布（按预测状态分组，用于确定阈值分界）:")
    for s in sorted(set(r["state"] for r in rows)):
        grp = [r for r in rows if r["state"] == s]
        print("  [%s] n=%d" % (s, len(grp)))
        for f in FEATURES:
            vals = [r[f] for r in grp]
            print("    %-12s mean=%6.3f  std=%6.3f  min=%6.3f  max=%6.3f"
                  % (f, np.mean(vals), np.std(vals), np.min(vals), np.max(vals)))

    if csv_path and rows:
        import csv
        with open(csv_path, "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
            w.writeheader()
            w.writerows(rows)
        print("\n特征明细已导出: %s（用 Excel 看分布定阈值）" % csv_path)


def main():
    parser = argparse.ArgumentParser(description="姿态分类离线标定（真 C++ 逻辑）")
    parser.add_argument("--sanity", action="store_true", help="合成关键点自检")
    parser.add_argument("--dump", type=str, help="pose.py --dump 导出的 .npz 路径")
    parser.add_argument("--csv", type=str, default="", help="导出特征明细 CSV")
    args = parser.parse_args()

    if args.sanity:
        sys.exit(0 if run_sanity() else 1)
    if args.dump:
        run_batch(args.dump, args.csv)
    else:
        parser.print_help()


if __name__ == "__main__":
    main()
