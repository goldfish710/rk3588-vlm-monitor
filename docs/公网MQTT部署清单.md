# 公网 MQTT 部署清单（板端 + mosquitto + 网页客户端）

> 目标：板子连家里 WiFi、手机用 4G 流量，双方都通过公网 broker 通信，实现异网远程操控。
> 前置：板端二进制已更新（修复 ACK 超时重复发送 / client_id 撞车两个隐患）。
> 已确认：板端可出公网（wlan0 已连家里 WiFi）。

---

## 一、云服务器 + mosquitto

### 1. 租服务器

轻量应用服务器（2 核 2G 起），系统 Ubuntu 22.04 / 24.04。
记下**公网 IP**，下文用 `<SERVER_IP>` 代指。

### 2. 开放端口（云控制台安全组）

| 端口 | 用途 | 放行 |
|---|---|---|
| **1883** | MQTT（板端连 `tcp://`） | 对全网 |
| **8083** | WebSocket（网页客户端连 `ws://`） | 对全网 |

> ⚠️ **只放行到云控制台安全组，系统内还有 ufw 的话也要放**——只开一处是连不上的最常见原因。
> 验证：`nc -zv <SERVER_IP> 1883` 从开发机能通才算数。

### 3. 一键部署脚本（推荐）

把 `server/tools/setup_public_broker.sh` 传到服务器跑：

```bash
scp server/tools/setup_public_broker.sh root@<SERVER_IP>:/root/
ssh root@<SERVER_IP> bash /root/setup_public_broker.sh
```

脚本做的事（全程自动）：
- `apt` 装 mosquitto + mosquitto-clients
- 自动生成 **board / web 两个随机强密码**，关匿名访问
- 写 ACL（board 只能碰 `home/rk3588_home_001/#` 和 `home/fall`；web 可发命令、订阅 `home/#`）
- 开 1883（MQTT）+ 8083（WebSocket）
- 三项自检：端口监听 ✓ / 匿名被拒 ✓ / ACL 越权被拒 ✓
- 最后打印所有连接参数——**把输出存好**，后面的配置全要用

> 脚本跑完给的两行密码**只显示这一次**，立刻抄下来。

### 3'. 手动步骤（不想用脚本时照这个）

```bash
apt install -y mosquitto mosquitto-clients
mosquitto_passwd -c /etc/mosquitto/passwd board        # 设板端密码
mosquitto_passwd /etc/mosquitto/passwd web             # 设网页密码

cat > /etc/mosquitto/acl <<'EOF'
user board
topic write home/rk3588_home_001/#
topic read  home/rk3588_home_001/#
topic write home/fall

user web
topic write home/rk3588_home_001/#
topic read  home/#
EOF

cat > /etc/mosquitto/conf.d/zz-public-demo.conf <<'EOF'
# 全局项只写一次（每个 listener 重复写会报 Duplicate password_file；
# Ubuntu 主配置已含 persistence/log_dest，这里不要再写）
allow_anonymous false
password_file /etc/mosquitto/passwd
acl_file /etc/mosquitto/acl

listener 1883 0.0.0.0

listener 8083 0.0.0.0
protocol websockets

connection_messages true
EOF

chown mosquitto:mosquitto /etc/mosquitto/passwd /etc/mosquitto/acl
chmod 600 /etc/mosquitto/passwd
systemctl restart mosquitto
```

---

## 二、MediaMTX（远程实时视频，可选）

### 1. 下载与启动

```bash
cd ~
# GitHub 直连慢 → 用 ghfast.top 镜像（ghproxy 常不完整/失效）
curl -sL --max-time 60 "https://ghfast.top/https://github.com/bluenviron/mediamtx/releases/download/v1.21.0/mediamtx_v1.21.0_linux_amd64.tar.gz" -o mediamtx.tar.gz
tar xzf mediamtx.tar.gz          # 完整包约 27MB
nohup ./mediamtx > ~/mediamtx.log 2>&1 &
ss -lntp | grep -E '1935|8888'   # 1935=RTMP 收流 8888=HLS 分发
```

### 2. 防火墙

放行 **TCP 1935、8888**（所有 IPv4）。

### 3. 板端推流配置

```ini
[vlm_pipeline]
stream_rtmp_url=rtmp://<SERVER_IP>:1935/live_<随机串>
; 源可配：h264=子码流带AI框(全平台) / h265=主码流高清(仅 iPhone Safari)
stream_src_url=rtsp://127.0.0.1:8554/h264
```

### 4. 网页端

设置里填 **HLS 地址**：`http://<SERVER_IP>:8888/live_<随机串>/index.m3u8`

点「▶ 看视频」→ 板端起 ffmpeg 零转码推 RTMP → MediaMTX 转 HLS → 网页播放（约 3~5s 延迟）。
「■ 关流」停推流。

### 5. 排错

| 现象 | 看哪 |
|---|---|
| 播放器 1 秒 networkError | 网页是旧版（Ctrl+Shift+R 强制刷新）；新版会延迟 3s 加载+重试 6 次 |
| 应答了但一直没流 | 板端 `cat /tmp/stream_push.log`；服务器 `tail ~/mediamtx.log` |
| ffmpeg 报 HandShake 错误 | 一次性告警，ffmpeg 会自动重试连上；频繁断流才需处理 |
| 多端同时看 | 天然支持（HLS 多 reader） |

---

## 三、板端配置

编辑板端 `/AI/config.ini`：

```ini
[mqtt]
broker_url=tcp://<SERVER_IP>:1883
username=board
password=<board 账号的密码>
publish_ack_timeout_ms=5000

[vlm_pipeline]          ; ← 原来没有这一节，必须新增
remote_enable=1
remote_token=<强随机字符串，例如 openssl rand -hex 16 生成>
```

**`[vlm_pipeline]` 这一节不加，等于零鉴权**——任何人知道 topic 就能控制你的摄像头。

改完重启板端，看日志：

```bash
adb shell "tail -f /AI/tlserver.log" | grep -E 'MQTT|订阅'
```

期望看到：连接成功 → 订阅 `home/rk3588_home_001/cmd` → 发布 retained online。

---

## 四、网页客户端

`client/web_demo/index.html` 是单文件网页，手机/电脑浏览器打开即用。

### 部署到服务器（推荐）

```bash
scp client/web_demo/index.html root@<SERVER_IP>:/root/
# 服务器上起个静态服务（nginx 或 python 都行，用高位端口避免备案）
nohup python3 -m http.server 8080 --directory /root &
```

手机浏览器打开 `http://<SERVER_IP>:8080/`。

> ⚠️ **必须用 `http://` 打开**。网页用 `ws://` 连 mosquitto，而 https 页面会被浏览器拦截 ws:// 连接（混合内容）。
> 演示时浏览器会标"不安全"——功能不受影响。

### 本地打开（仅电脑）

直接双击 `index.html` 也能用（file:// 不受混合内容限制）。

### 填写设置

| 字段 | 值 |
|---|---|
| Broker | `ws://<SERVER_IP>:8083/mqtt` |
| 用户名 / 密码 | `web` 账号 |
| 设备 ID | `rk3588_home_001`（必须与板端 `[event] device_id` 一致） |
| 令牌 | 与板端 `remote_token` 完全一致 |

配置存在浏览器 localStorage，下次打开自动连接。

---

## 五、验证

| # | 操作 | 期望 |
|---|---|---|
| 1 | 点「设备状态」 | 返回 `在线` + 画面人数，日志显示 RTT |
| 2 | 点「快照」 | 显示现场画面（长边 320 缩略图） |
| 3 | 输入"现在发生了什么"→ 提问 | 返回与画面一致的描述（约 12~17s） |
| 4 | 点「时间线」/「事件记录」 | 列出最近 10 条摘要/事件 |
| 5 | **手机关 WiFi 用 4G**，重复 1~3 | 全部正常 → **异网验证通过** |
| 6 | 服务器 `systemctl restart mosquitto` | 板端日志出现重连退避，恢复后命令仍可用（重订阅生效） |
| 7 | 故意填错令牌 | 返回 `401 unauthorized` |
| 8 | 板端 Ctrl+C 退出 | 网页收到 `offline`（retained） |

第 5 条是**整个工作的核心目标**——它证明设备与手机不在同一局域网也能通信。

---

## 六、排错

| 现象 | 原因 |
|---|---|
| 板端日志 `MQTT connect failed rc=-1` | 安全组没放行 1883，或用户名密码错 |
| 板端连上又立刻掉线、反复重连 | `client_id` 撞车（两台设备用同一 id）——已修，确认板端跑的是新二进制 |
| 网页连不上、浏览器控制台报 1006 | 安全组没放行 8083；或页面是 https 而连的是 ws |
| 网页连上但收不到应答 | 设备 ID 填错（topic 前缀不一致）；或令牌不对（应回 401） |
| 命令返回 `503` | VLM 调用失败（板端 NPU 侧异常）。注意：**板端"忙"不会导致命令丢失**——忙时命令只排队，当前任务结束后按序执行 |
| 快照收到两张图 | 旧二进制（ACK 超时 1s 导致重发）——确认已推新版本 |
| 时间戳对不上 | 板端无 NTP，时钟可能偏；演示前 `adb shell date` 核对一下 |

---

## 七、回滚

```bash
# 板端二进制
adb shell "cd /AI && cp TLServer.old54 TLServer && chmod 777 TLServer"
# 配置改回内网 broker
# broker_url=tcp://192.168.5.77:1883，删掉 [vlm_pipeline] 的 remote_token
```
