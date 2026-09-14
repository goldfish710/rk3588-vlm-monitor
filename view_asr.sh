#!/bin/sh
# PC 端通过 adb 实时查看板端语音识别文字（无需开板端终端）
# 用法: ./view_asr.sh        → 显示最近 50 行后持续跟踪（Ctrl+C 退出）
#       ./view_asr.sh -c     → 清空板端记录文件
FILE=/userdata/tlserver/events/asr_text.txt

case "$1" in
    -c|--clear)
        adb shell "> $FILE"
        echo "已清空板端 $FILE"
        exit 0
        ;;
esac

adb shell "tail -n 50 -f $FILE"
