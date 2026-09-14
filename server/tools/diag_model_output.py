#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
诊断：对比新旧 yolov8n-pose 模型的输出张量结构（连板）
用法: python diag_model_output.py
"""
import numpy as np
import cv2
import sys
from rknn.api import RKNN

BASE = "/home/yzy/aicode/yolov8n-pose-monitor/server"
IMG = BASE + "/model/bus.jpg"
MODELS = [
    (BASE + "/model/yolov8n-pose.rknn", "新模型(2.2.1重转)"),
    (BASE + "/model/yolov8n-pose.rknn.v1.bak", "旧模型(v1)"),
]


def letterbox(img, size=(640, 640), bg=114):
    h, w = img.shape[:2]
    scale = min(size[0] / w, size[1] / h)
    nw, nh = int(w * scale), int(h * scale)
    out = np.ones((size[1], size[0], 3), dtype=np.uint8) * bg
    dx, dy = (size[0] - nw) // 2, (size[1] - nh) // 2
    out[dy:dy + nh, dx:dx + nw] = cv2.resize(img, (nw, nh), interpolation=cv2.INTER_AREA)
    return out, scale, dx, dy


def inspect(model_path, label):
    print("\n" + "=" * 60)
    print(label, "->", model_path)
    rknn = RKNN(verbose=False)
    ret = rknn.load_rknn(model_path)
    if ret != 0:
        print("load_rknn 失败 ret=%d" % ret)
        return
    ret = rknn.init_runtime(target="rk3568")
    if ret != 0:
        print("init_runtime 失败 ret=%d" % ret)
        return

    img = cv2.imread(IMG)
    lb, scale, dx, dy = letterbox(img)
    inp = lb[..., ::-1]  # BGR->RGB
    outputs = rknn.inference(inputs=[inp])
    print("输出张量数: %d" % len(outputs))
    for i, o in enumerate(outputs):
        print("  [%d] shape=%s dtype=%s" % (i, o.shape, o.dtype))

    # 关键点张量分析（假设是 index 3，同 pose.py）
    kp = outputs[3]
    print("关键点张量[3]: shape=%s dtype=%s" % (kp.shape, kp.dtype))
    flat = kp.reshape(-1).astype(np.float64)
    nonzero = np.count_nonzero(flat)
    print("  非零值比例: %.1f%% (总元素 %d)" % (100.0 * nonzero / flat.size, flat.size))
    print("  值域: min=%.1f max=%.1f mean=%.2f" % (flat.min(), flat.max(), flat.mean()))
    # 前 20 个非零值采样
    nz = flat[flat != 0][:20]
    print("  前20个非零值: %s" % nz)

    # 检查是否是 [1,51,8400] 布局且值看起来像坐标/置信度
    if kp.size >= 51 * 8400:
        kp3 = kp.reshape(51, 8400)
        # 置信度行（每 3 行第 3 个）
        conf_rows = kp3[2::3, :]
        print("  置信度行统计: mean=%.3f max=%.3f 非零比例=%.1f%%"
              % (conf_rows.mean(), conf_rows.max(),
                 100.0 * np.count_nonzero(conf_rows) / conf_rows.size))
    rknn.release()


for path, label in MODELS:
    inspect(path, label)
print("\n完成")
