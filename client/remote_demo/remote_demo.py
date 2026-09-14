#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
remote_demo.py —— RK3588 智能家居监护终端 · PC 演示客户端（模拟手机端）

通过公网 MQTT 与板端通信，演示"手机异地操控 + 实时接收家中状态"：
  s          发 snapshot 命令，收到后保存现场图为 shot_<时间>.jpg
  c <文本>   发 chat 命令（如：c 现在发生了什么），打印板端 VLM 的视觉问答
  t [n]      查最近 n 条状态摘要时间线（默认 10）
  e [n]      查最近 n 条报警/时间线记录（含 vlm_confirm 复核结论）
  p          发 ping，查看设备在线状态与画面人数
  h          帮助
  q          退出

实时订阅（无需命令，板端主动推送即打印）：
  home/{device_id}/timeline   状态摘要（周期巡检）
  home/{device_id}/state      设备上下线（retained）
  home/fall                   跌倒报警（urgent/attention/cleared 分级）

连接参数优先读同目录 config.json，也可命令行覆盖：
  python3 remote_demo.py --broker 8.210.x.x --port 1883 --device rk3588_home_001 --token mysecret

依赖：pip install paho-mqtt
"""

import argparse
import base64
import json
import os
import sys
import time
import uuid

import paho.mqtt.client as mqtt


class DemoClient:
    def __init__(self, cfg):
        self.cfg = cfg
        self.dev = cfg["device_id"]
        self.client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
        self.client.on_connect = self._on_connect
        self.client.on_message = self._on_message
        if cfg.get("username"):
            self.client.username_pw_set(cfg["username"], cfg.get("password"))

    # ---------- MQTT 回调 ----------
    def _on_connect(self, client, userdata, flags, reason_code, properties):
        print(f"[MQTT] 已连接 broker（rc={reason_code}），订阅中...")
        client.subscribe(f"home/{self.dev}/resp", qos=1)
        client.subscribe(f"home/{self.dev}/timeline", qos=1)
        client.subscribe(f"home/{self.dev}/state", qos=1)
        client.subscribe("home/fall", qos=1)

    def _on_message(self, client, userdata, msg):
        topic = msg.topic
        try:
            payload = json.loads(msg.payload.decode("utf-8", errors="replace"))
        except Exception:
            print(f"[MQTT] {topic}: {msg.payload!r}")
            return

        if topic == f"home/{self.dev}/resp":
            self._on_resp(payload)
        elif topic == f"home/{self.dev}/timeline":
            print(f"[时间线] {payload.get('summary', '')}"
                  f"（人数={payload.get('confidence', '?')}）")
        elif topic == f"home/{self.dev}/state":
            print(f"[设备] {payload.get('state')} @ {payload.get('ts', '')}")
        elif topic == "home/fall":
            sev = payload.get("severity", "-")
            sev_cn = {"urgent": "★真跌倒★", "attention": "△未确认", "cleared": "○已排除"}.get(sev, sev)
            line = f"[报警 {sev_cn}] conf={payload.get('confidence')}"
            if payload.get("vlm_reply"):
                line += f" 复核=\"{payload['vlm_reply']}\""
            if payload.get("video"):
                line += f" 录像={payload['video']}"
            print(line)

    def _on_resp(self, p):
        cmd = p.get("cmd", "?")
        code = p.get("code", -1)
        data = p.get("data") or {}
        if code != 0:
            print(f"[应答 {cmd}] 失败 code={code}: {data.get('msg', '')}")
            return
        if cmd == "snapshot":
            b64 = data.get("image_base64", "")
            if not b64:
                print("[应答 snapshot] 无图片数据")
                return
            img = base64.b64decode(b64)
            name = time.strftime("shot_%Y%m%d_%H%M%S.jpg")
            with open(name, "wb") as f:
                f.write(img)
            print(f"[应答 snapshot] 现场图已保存: {name}（{data.get('w')}x{data.get('h')}，{len(img)} bytes）")
        elif cmd == "chat":
            print(f"[应答 chat] {data.get('reply', '')}")
        elif cmd == "timeline":
            items = data.get("items", [])
            print(f"[应答 timeline] 最近 {len(items)} 条:")
            for it in items:
                conf = f" 复核={it['vlm_confirm']}" if "vlm_confirm" in it else ""
                print(f"  {it.get('time', '')} [{it.get('type', '')}] {it.get('text', '')}{conf}")
        elif cmd == "ping":
            print(f"[应答 ping] 状态={data.get('state', '?')} 画面人数={data.get('persons', '?')}")
        else:
            print(f"[应答 {cmd}] {json.dumps(p, ensure_ascii=False)}")

    # ---------- 命令发送 ----------
    def send_cmd(self, cmd, **kw):
        msg = {"cmd": cmd, "req_id": uuid.uuid4().hex[:12]}
        if self.cfg.get("token"):
            msg["auth"] = self.cfg["token"]
        msg.update(kw)
        self.client.publish(f"home/{self.dev}/cmd", json.dumps(msg, ensure_ascii=False), qos=1)

    # ---------- 交互循环 ----------
    def run(self):
        self.client.connect(self.cfg["broker"], self.cfg["port"], keepalive=15)
        self.client.loop_start()
        print("=" * 60)
        print("RK3588 监护终端 · 远程操控演示（输入 h 查看命令）")
        print("=" * 60)
        try:
            while True:
                try:
                    raw = input("> ").strip()
                except EOFError:
                    break
                if not raw:
                    continue
                parts = raw.split(maxsplit=1)
                cmd = parts[0].lower()
                arg = parts[1] if len(parts) > 1 else ""
                if cmd == "s":
                    self.send_cmd("snapshot")
                elif cmd == "c":
                    if not arg:
                        print("用法: c <问题>，如：c 现在发生了什么")
                        continue
                    self.send_cmd("chat", text=arg)
                elif cmd == "t":
                    n = int(arg) if arg.isdigit() else 10
                    self.send_cmd("timeline", limit=n)
                elif cmd == "e":
                    n = int(arg) if arg.isdigit() else 10
                    self.send_cmd("events", limit=n)
                elif cmd == "p":
                    self.send_cmd("ping")
                elif cmd == "q":
                    break
                elif cmd == "h":
                    print("s 快照 | c <问题> 视觉问答 | t [n] 时间线 | e [n] 事件 | p 状态 | q 退出")
                else:
                    print(f"未知命令: {cmd}（输入 h 查看帮助）")
        finally:
            self.client.loop_stop()
            self.client.disconnect()
        print("已退出")


def main():
    ap = argparse.ArgumentParser(description="RK3588 监护终端远程操控演示客户端")
    ap.add_argument("--config", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "config.json"))
    ap.add_argument("--broker")
    ap.add_argument("--port", type=int)
    ap.add_argument("--device")
    ap.add_argument("--token")
    ap.add_argument("--username")
    ap.add_argument("--password")
    args = ap.parse_args()

    cfg = {}
    if os.path.exists(args.config):
        with open(args.config, "r", encoding="utf-8") as f:
            cfg = json.load(f)
    if args.broker:
        cfg["broker"] = args.broker
    if args.port:
        cfg["port"] = args.port
    if args.device:
        cfg["device_id"] = args.device
    if args.token:
        cfg["token"] = args.token
    if args.username:
        cfg["username"] = args.username
    if args.password:
        cfg["password"] = args.password

    need = [k for k in ("broker", "device_id") if k not in cfg]
    if need:
        print(f"缺少配置: {need}（config.json 或命令行参数）")
        sys.exit(1)
    cfg.setdefault("port", 1883)
    cfg.setdefault("username", "")
    cfg.setdefault("password", "")
    cfg.setdefault("token", "")

    DemoClient(cfg).run()


if __name__ == "__main__":
    main()
