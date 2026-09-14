# yolov8n-pose-monitor — 基于 RK3588 的板载 VLM 智能家居监护终端

一台能"看懂画面"的板端多模态 AIoT 终端:板载部署 Qwen3-VL-2B 视觉大模型(NPU 推理),
实现**两级跌倒检测流水线**(YOLO 候选 → VLM 看图复核分级)、**周期状态理解时间线**、
**公网 MQTT 远程操控**(手机异网快照/视觉问答),并保留完整的音视频与语音陪伴能力。

## 功能全景

| 通道 | 能力 |
|---|---|
| **两级跌倒检测** | yolov8n-pose 高召回候选(多人追踪+连续帧投票)→ 板端 Qwen3-VL 看图复核 → 三级分级报警(urgent 真跌倒 / attention 未确认 / cleared 已排除);证据(图/预录录像)先行落盘,超时宁报勿漏,跌倒静默报警(语音通道只服务老人主动发起的交互) |
| **周期状态时间线** | 每 30s 在空闲+有人时抓帧 → VLM 一句话场景摘要 → SQLite 落库 + MQTT 推送,家人可回看老人一天活动流;无人帧零 VLM 调用 |
| **远程操控** | 公网 MQTT 双向协议:手机异地发快照/视觉问答("现在发生了什么")/时间线/事件查询/设备状态,req_id 应答关联 + token 认证 + base64 现场图回传(演示客户端 client/remote_demo/) |
| **单 NPU 资源仲裁** | 复核/对话/巡检/远程问答共用一颗 NPU 与单 VLM 执行线程:优先级队列 + rkllm_abort 抢占 + 句级丢句 + 超时降级 |
| **视觉/音视频** | 1080p 主码流(H.265)+ 640p 子码流(H.264 带 AI 框/骨架),双路 30fps 零拷贝;fMP4 循环录像(断电安全)+ RTSP AAC 音轨 |
| **语音识别** | zipformer 流式 ASR(中英双语):呼救关键词主动上报 MQTT;RTF 0.087 |
| **语音对话** | 多唤醒词 → 板端 VLM 流式对话(全本地,断网可用)→ 本地 Piper TTS(NPU)逐句播报,首句延迟 ~3s;云端 DeepSeek+火山 TTS 可配置回退 |
| **端云可选** | tts_provider=piper|cloud、vlm_enable=0/1 一行切换（0=回退云端 DeepSeek） |

## 架构

```
┌────────────── 板端 RK3588 ──────────────┐        ┌───── 网络/云端 ─────┐
│ 摄像头 → V4L2(DMABUF) → RGA → MPP 双码流 ├─RTSP──→│ VLC 实时观看        │
│      ↓                                   │        │ 公网 MQTT Broker    │
│  RKNN NPU: yolov8n-pose(15fps)          │   ◄───►│  (EMQX/mosquitto)   │
│      ↓ 追踪器跌倒候选                     │  MQTT  │    ↕                │
│  ┌────────────────────────────────┐     │        │ 手机/PC 演示客户端   │
│  │ ChatEngine chat_llm 线程(单 VLM 执行线程)│      │ (快照/问答/时间线)   │
│  │  优先级1 跌倒复核: 候选图→VLM→分级报警 │      │ DeepSeek(可选回退)   │
│  │  优先级2 语音对话: VLM 流式→Piper 逐句播 │      └────────────────────┘
│  │  优先级3 远程命令: 快照/问答/时间线    │
│  │  优先级4 周期巡检: 抓帧→摘要→时间线    │
│  │  抢占: rkllm_abort + 句级丢句 + 超时降级 │
│  └────────────────────────────────┘     │
│  FrameProvider(按需零拷贝快照通道)        │
│  ASR(zipformer)→唤醒/求救→ChatEngine     │
│  SQLite(事件/时间线,幂等迁移) + fMP4 录像  │
└──────────────────────────────────────────┘
```

**线程模型**:H.265 编码 / H.264+AI 编码 / AI 推理 / 事件处理(复核编排) / 录像 / MQTT(发布+订阅) /
音频采集 / ASR 推理 / chat_llm(全部 VLM 调用) / tts_synth / tts_play / 健康监控。
实时链路 nice -5 绑大核,软实时链路 nice 10。

**全链路零拷贝**:V4L2 DMABUF → MPP → RGA → RKNN 全 fd 传递,CPU 只做调度与绘制。
实测:双路 30fps,常驻 CPU ~2%(编码/缩放/旋转全硬件化)。

## MQTT 协议(远程操控)

| 方向 | topic | 说明 |
|---|---|---|
| 上行报警 | `home/fall` | 现报文 + `severity/vlm_confirm/vlm_reply` 可选字段(老订阅端零感知) |
| 上行摘要 | `home/{device_id}/timeline` | 周期巡检状态摘要 |
| 上行应答 | `home/{device_id}/resp` | `{cmd,req_id,code,ts,data}`,code≠0 时 data.msg 为错误原因 |
| 上行状态 | `home/{device_id}/state` | retained:online/offline |
| 下行命令 | `home/{device_id}/cmd` | `{"cmd":"snapshot|chat|timeline|events|ping","req_id":"…","auth":"token","text":"…","limit":10}` |

演示:`client/remote_demo/remote_demo.py`(`pip install paho-mqtt`;`s` 快照 / `c 现在发生了什么` 问答 / `t` 时间线 / `p` 状态)。

## 技术栈

| 层 | 技术 |
|---|---|
| 采集/编码 | V4L2 MPLANE + DMABUF 零拷贝;RK MPP(H.265/H.264 硬编) |
| 流媒体/通信 | live555 RTSP;MQTT(paho C,QoS1,发布+订阅,断线指数退避+重订阅) |
| 视觉 AI | RKNN(NPU):yolov8n-pose 混合量化、Qwen3-VL-2B(RKLLM w8a8 + vision rknn)、zipformer 流式 ASR |
| 大模型 | 板端 RKLLM 1.3.0(keep_history KV 多轮、rkllm_abort 抢占、多模态 vision 编码) |
| TTS | 本地 Piper(NPU decoder,全离线)/ 云端火山(可配置回退) |
| 存储 | fMP4(断电安全)+ SQLite(事件/时间线,幂等 ALTER 迁移)+ 循环覆盖 |
| 语言 | C++11(paroli C++20 静态库经 C 边界混链),10+ 线程,手写 HTTPS/SSE 客户端、cJSON |

## 快速开始(板端 RK3588)

```bash
# 部署(PC):交叉编译 server/TLServer/build-linux.sh -p 后推板 /AI/
# 配置 /AI/config.ini(首次运行自动生成默认;重点节见下)
# 启动
cd /AI && ./start.sh
# 观看
VLC → rtsp://<板IP>:8554/h265 (主码流) / :8554/h264 (子码流)
# 对话
说"你好"唤醒 → 说话 → 板端语音回复(全本地 VLM+Piper,断网可用)
# 报警订阅
mosquitto_sub -h <broker> -t home/fall -v        # severity: urgent/attention/cleared
mosquitto_sub -h <broker> -t home/<device_id>/timeline -v   # 状态摘要
# 远程操控(PC/手机)
cd client/remote_demo && python3 remote_demo.py
```

config.ini 新节速览:

```ini
[local_llm]      vlm_enable=1                    # 板端 Qwen3-VL(核心卖点;0=回退云端 DeepSeek)
[chat]           tts_provider=piper              # piper=本地全离线 | cloud=火山
[vlm_pipeline]   confirm_enable=1                # 跌倒复核总开关
                 confirm_timeout_ms=5000         # 超时按未确认直报(宁报勿漏)
                 status_interval_sec=30          # 周期巡检间隔
                 remote_token=                   # 公网部署必须设置(命令认证)
[mqtt]           cmd_topic=home/{device_id}/cmd  # 下行命令订阅点
```

## 目录结构

```
server/
├── TLServer/          板端主程序(CMake 交叉编译)
│   ├── AudioCapture    ALSA 采集(多路分发)
│   ├── AudioPlayback   ALSA 播放(重采样+会话状态机)
│   ├── EncodeThread    视频编码+AI+事件(复核编排)
│   └── config          配置系统([vlm_pipeline] 等)
├── cpp/
│   ├── chat_engine     语音对话引擎(状态机+优先级仲裁+巡检+远程命令)
│   ├── vlm_engine      RKLLM 封装(多模态+流式+rkllm_abort 抢占)
│   ├── frame_provider  按需零拷贝快照通道(巡检/远程共用)
│   ├── EventManager    SQLite(幂等迁移+时间线+复核回填)
│   ├── mqtt_thread     MQTT 发布/订阅线程(原始发布原语)
│   ├── asr_thread      zipformer 流式 ASR 引擎
│   ├── pose_classifier 跌倒/坐姿多特征投票
│   └── 3rdparty/       paroli(Piper)/kaldi-native-fbank/cJSON/rknpu2
client/
├── web_demo/           网页远程操控面板(手机/电脑浏览器直连,快照直接出图)
└── remote_demo/        远程操控命令行客户端(协议调试用)
```

> 注：本地另有 `docs/` 目录（开发日志、技术文档等），为个人资料未上传。

## 许可与说明

个人学习项目。云端 API key 明文存板端配置、TLS 跳过证书校验均为个人场景权衡;
商用需服务端中转与证书校验。
