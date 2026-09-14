#!/usr/bin/env bash
# ============================================================================
# 公网 MQTT broker 一键部署（mosquitto）—— 供板端 + 网页客户端异网访问
#
# 用法（在云服务器上，root）：
#     bash setup_public_broker.sh                 # 自动生成强密码
#     bash setup_public_broker.sh <板端密码> <网页密码>   # 指定密码
#
# 部署内容：
#   - 1883 明文 MQTT（板端连）
#   - 8083 WebSocket（网页客户端连 ws://<IP>:8083/mqtt）
#   - 关闭匿名访问；board / web 两个账号 + topic 级 ACL
#
# 安全说明：本方案**不做 TLS**（个人演示场景的明确取舍）。
#   访问控制 = 账号密码（挡住陌生人）+ ACL（限制能碰哪些 topic）+ 板端 remote_token（能否执行命令）。
#   ⚠️ 因此密码强度就是唯一屏障——密码由脚本随机生成，不要改成弱口令。
# ============================================================================
set -euo pipefail

BOARD_PW="${1:-$(openssl rand -hex 12)}"
WEB_PW="${2:-$(openssl rand -hex 12)}"
DEV="rk3588_home_001"

if [ "$(id -u)" -ne 0 ]; then
    echo "请用 root 运行：sudo bash $0" >&2
    exit 1
fi

echo "==> 安装 mosquitto"
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq mosquitto mosquitto-clients

echo "==> 创建账号"
rm -f /etc/mosquitto/passwd
mosquitto_passwd -c -b /etc/mosquitto/passwd board "$BOARD_PW"
mosquitto_passwd    -b /etc/mosquitto/passwd web   "$WEB_PW"
chown mosquitto:mosquitto /etc/mosquitto/passwd
chmod 600 /etc/mosquitto/passwd

echo "==> 写 ACL"
# 规则说明：
#   topic write = 发布（客户端 → broker）
#   topic read  = 订阅（broker → 客户端）
#   未在上面 user 段落中列出的账号，默认一律拒绝
cat > /etc/mosquitto/acl <<EOF
# ---- 板端：只能收发自己设备前缀下的 topic ----
user board
topic write home/${DEV}/#
topic read  home/${DEV}/#
topic write home/fall

# ---- 网页客户端：发命令到本设备，收全部上行 ----
user web
topic write home/${DEV}/#
topic read  home/#
EOF
chown mosquitto:mosquitto /etc/mosquitto/acl

echo "==> 写 mosquitto 配置"
mkdir -p /etc/mosquitto/conf.d
rm -f /etc/mosquitto/conf.d/*.conf
cat > /etc/mosquitto/conf.d/zz-public-demo.conf <<'EOF'
# ---------- 全局认证（mosquitto 全局项只允许写一次；Ubuntu 默认主配置
#            已含 persistence/persistence_location/log_dest，这里再写会报
#            "Duplicate ... value in configuration"——2026-09-13 首次部署踩过） ----------
allow_anonymous false
password_file /etc/mosquitto/passwd
acl_file /etc/mosquitto/acl

# ---------- 明文 MQTT：板端连 tcp://<IP>:1883 ----------
listener 1883 0.0.0.0

# ---------- WebSocket：网页客户端连 ws://<IP>:8083/mqtt ----------
listener 8083 0.0.0.0
protocol websockets

# connection_messages 会记录每次连接的 client_id（排查 client_id 撞车时很有用）
connection_messages true
EOF

# Ubuntu 的 mosquitto.conf 默认就 include_dir 了 conf.d；没有则补上
if ! grep -q '^include_dir' /etc/mosquitto/mosquitto.conf 2>/dev/null; then
    echo 'include_dir /etc/mosquitto/conf.d' >> /etc/mosquitto/mosquitto.conf
fi

echo "==> 启动服务"
systemctl enable mosquitto >/dev/null 2>&1 || true
# 不用 set -e 直接等 restart 失败：restart 失败要能走到诊断分支
if ! systemctl restart mosquitto; then
    echo "✗ mosquitto 启动失败，最近日志：" >&2
    journalctl -u mosquitto -n 40 --no-pager >&2 || true
    echo >&2
    echo "直接前台跑看具体错误：" >&2
    echo "  sudo mosquitto -c /etc/mosquitto/mosquitto.conf -v" >&2
    echo "常见原因：① Duplicate password_file（全局项在每个 listener 各写一份）" >&2
    echo "          ② 主配置 /etc/mosquitto/mosquitto.conf 里也有重复的全局项" >&2
    echo "          ③ 该 mosquitto 包没编 websockets → 删 8083 listener 段重试" >&2
    exit 1
fi
sleep 1
if ! systemctl is-active --quiet mosquitto; then
    echo "✗ mosquitto 进程未存活，日志如下：" >&2
    journalctl -u mosquitto -n 40 --no-pager >&2 || true
    exit 1
fi
echo "✓ 服务已启动"

echo
echo "==> 自检 1/3：端口监听"
ss -lntp 2>/dev/null | grep -E ':1883|:8083' || echo "⚠ 没看到 1883/8083 监听，请查日志"

echo
echo "==> 自检 2/3：匿名连接应被拒绝"
if mosquitto_pub -h 127.0.0.1 -p 1883 -t test -m hi >/dev/null 2>&1; then
    echo "✗ 匿名连接竟然成功了！allow_anonymous 没生效，切勿上线" >&2
    exit 1
else
    echo "✓ 匿名连接被拒绝"
fi

echo
echo "==> 自检 3/3：账号能否收发"
if mosquitto_pub -h 127.0.0.1 -p 1883 -u web -P "$WEB_PW" \
        -t "home/${DEV}/cmd" -m '{"cmd":"ping","req_id":"selftest"}' 2>/dev/null; then
    echo "✓ web 账号可发布命令"
else
    echo "✗ web 账号发布失败，检查 password_file / acl_file" >&2
    exit 1
fi

if mosquitto_pub -h 127.0.0.1 -p 1883 -u web -P "$WEB_PW" -t "home/other_device/cmd" -m x >/dev/null 2>&1; then
    echo "✗ ACL 没生效：web 账号能往别的设备 topic 发命令" >&2
else
    echo "✓ ACL 生效（越权 topic 被拒绝）"
fi

PUB_IP="$(curl -s --max-time 5 ifconfig.me 2>/dev/null || echo '<见云控制台>')"

cat <<EOF

============================================================
 部署完成 —— 以下参数填进板端与网页客户端
============================================================
 公网 IP    : ${PUB_IP}
 板端账号   : board / ${BOARD_PW}
 板端地址   : tcp://${PUB_IP}:1883
 网页账号   : web / ${WEB_PW}
 网页地址   : ws://${PUB_IP}:8083/mqtt
 device_id   : ${DEV}
============================================================

⚠️ 密码请立刻抄走保存（本脚本不会再次显示）。
⚠️ 云控制台的安全组必须放行 1883 和 8083，否则外网连不上。
EOF
