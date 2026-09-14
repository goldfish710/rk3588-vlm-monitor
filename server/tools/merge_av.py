#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
音视频合成工具（PC 端）
======================

把板端录制的同名视频/音频对（xxx.h265 + xxx.wav）合成为单个 MKV 文件，
方便任何播放器直接播放（合成是 remux 直接封装，零转码，秒级完成）。

原理：录像时视频存 H.265 裸流、音频存 WAV 裸 PCM（见 docs/技术文档.md 11.4 章），
两者是同名文件、时长天然对齐。播放时用 ffmpeg 的 "-c copy" 把两条流
装进 MKV 容器即可，不重新编码。

依赖：PC 安装 ffmpeg（官网 https://ffmpeg.org/download.html 或 apt install ffmpeg）

用法：
  # 把 adb 拉下来的录像目录里所有配对文件合成 MKV
  python merge_av.py <录像目录>

  # 指定输出目录（默认与视频同目录）
  python merge_av.py <录像目录> --out <输出目录>

  # 合成成功后删除原始 .h265/.wav 对（默认不删）
  python merge_av.py <录像目录> --delete

例（从板子拉一天录像并合成）：
  adb pull /mnt/sdcard/record_main/continuous/20260817 ./recordings
  python merge_av.py ./recordings
  ffplay ./recordings/121827.mkv
"""

import argparse
import os
import subprocess
import sys

VIDEO_EXT = ".h265"
AUDIO_EXT = ".wav"
OUT_EXT = ".mkv"


def find_pairs(root):
    """递归找 (video_path, audio_path) 配对列表"""
    pairs = []
    for dirpath, _dirnames, filenames in os.walk(root):
        for fn in sorted(filenames):
            if not fn.endswith(VIDEO_EXT):
                continue
            base = fn[: -len(VIDEO_EXT)]
            v = os.path.join(dirpath, fn)
            a = os.path.join(dirpath, base + AUDIO_EXT)
            if os.path.exists(a):
                pairs.append((v, a))
            else:
                print("[跳过] 缺音频: %s" % v)
    return pairs


def merge(video, audio, out):
    cmd = ["ffmpeg", "-y", "-loglevel", "error",
           "-i", video, "-i", audio,
           "-c", "copy", "-map", "0:v", "-map", "1:a",
           out]
    rc = subprocess.run(cmd).returncode
    return rc == 0


def main():
    parser = argparse.ArgumentParser(description="板端 .h265+.wav 配对合成为 MKV")
    parser.add_argument("dir", help="录像目录（递归扫描）")
    parser.add_argument("--out", default="", help="输出目录（默认与视频同目录）")
    parser.add_argument("--delete", action="store_true",
                        help="合成成功后删除原始 .h265/.wav")
    args = parser.parse_args()

    if not os.path.isdir(args.dir):
        print("目录不存在: %s" % args.dir)
        sys.exit(1)

    pairs = find_pairs(args.dir)
    print("找到 %d 对视频+音频" % len(pairs))

    ok = 0
    for video, audio in pairs:
        out_dir = args.out if args.out else os.path.dirname(video)
        os.makedirs(out_dir, exist_ok=True)
        base = os.path.splitext(os.path.basename(video))[0]
        out = os.path.join(out_dir, base + OUT_EXT)

        if os.path.exists(out):
            print("[跳过] 已存在: %s" % out)
            ok += 1
            continue

        if merge(video, audio, out):
            print("[成功] %s + %s → %s" % (os.path.basename(video),
                                           os.path.basename(audio), out))
            ok += 1
            if args.delete:
                os.remove(video)
                os.remove(audio)
                print("       已删除原始文件")
        else:
            print("[失败] %s" % video)

    print("完成: %d/%d" % (ok, len(pairs)))


if __name__ == "__main__":
    main()
