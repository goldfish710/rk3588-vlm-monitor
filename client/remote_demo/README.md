# 远程操控演示客户端（模拟手机端）

PC 端 Python 交互客户端，通过**公网 MQTT** 与板端 RK3588 监护终端通信，
演示"手机异地查看画面 + 与板端 VLM 文本对话 + 接收家中状态推送"。

## 环境

```bash
pip install paho-mqtt
# Ubuntu 系统 Python（PEP 668 受管环境）用：
pip3 install --break-system-packages paho-mqtt
# Windows：装 Python 3 后直接 pip install paho-mqtt
```

要求 paho-mqtt ≥ 2.0（客户端使用 V2 回调 API）。

## 配置

复制 [config.json](config.json) 修改连接参数：

| 字段 | 说明 |
|---|---|
| broker | 公网 MQTT broker 地址（自建 EMQX/mosquitto 云服务器，或内网测试 IP） |
| port | 端口（自建默认 1883） |
| username/password | broker 账号（板端 config.ini `[mqtt]` 同款） |
| device_id | 设备编号（板端 config.ini `[event] device_id`，topic 按此隔离） |
| token | 板端 config.ini `[vlm_pipeline] remote_token`；板端设了 token 这里必须一致 |

也可命令行覆盖：`python3 remote_demo.py --broker 8.210.x.x --device rk3588_home_001 --token xxx`

## 用法

```
> s              现场快照（保存 shot_时间.jpg；无人空房间也能拍）
> c 现在发生了什么   与板端 VLM 视觉问答（抓帧+理解+回文本）
> t 10           最近 10 条状态摘要时间线
> e 10           最近 10 条事件（跌倒报警含 vlm_confirm 复核结论）
> p              设备状态（online/画面人数）
> q              退出
```

运行期间自动实时打印：
- **时间线**：板端周期巡检的状态摘要（`home/{device_id}/timeline`）
- **报警**：跌倒分级报警（urgent=真跌倒 / attention=未确认 / cleared=已排除）
- **设备状态**：online/offline（retained，后来者也能读到最近状态）

## 异网演示（面试演示建议流程）

1. 板端接网（路由器或 4G 热点），PC 走另一条网络（手机流量/公司网），
   两者都连同一个公网 broker —— 验证真正的"异地"链路
2. PC 发 `p`：应 <1s 收到 online + 画面人数
3. 人在摄像头前做动作，发 `c 老人在做什么`：回复与画面一致
4. 摆人静置 2 分钟：客户端自动收到时间线摘要
5. 模拟跌倒：收到 urgent 报警（含复核原因/录像路径），且**板端不发出语音**
6. 断板端电源：收到 state offline（retained）

## 协议速查

下行 `home/{device_id}/cmd`：
```json
{"cmd":"snapshot|chat|timeline|events|ping","req_id":"uuid","auth":"token","text":"chat 必填","limit":10}
```
上行 `home/{device_id}/resp`：`{"cmd","req_id","code":0,"ts","data":{...}}`，code≠0 时 `data.msg` 为错误原因（401 未授权 / 404 取帧失败 / 503 VLM 忙）。

完整协议见 [docs/技术文档.md](../../docs/技术文档.md) 远程操控章节。
