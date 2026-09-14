# rk3588-vlm-monitor — 基于 RK3588 的板端多模态 VLM 智能家居监护终端

一台能"看懂画面"的板端多模态 AIoT 终端:板载部署 Qwen3-VL 多模态大模型(NPU 推理,支持 2B/4B 双模型切换),
实现**两级跌倒检测流水线**(YOLO 候选 → VLM 看图复核分级)、**周期状态理解时间线**、
**公网 MQTT 远程操控与实时视频**(手机异地快照/视觉问答/直播),并保留完整的音视频与语音交互能力。

## 功能全景

| 通道 | 能力 |
|---|---|
| **两级跌倒检测** | yolov8n-pose 高召回候选(多人追踪+连续帧投票)→ 板端 VLM 看图复核 → 三级分级报警(urgent 真跌倒 / attention 未确认 / cleared 已排除);证据(图/预录录像)先行落盘,超时宁报勿漏,跌倒静默报警(语音通道只服务在场者主动发起的交互) |
| **周期状态时间线** | 每 60s VLM 巡检画面生成一句话场景摘要,留存本地形成可回看的"历史活动日志";发现明火/浓烟等异常即报警 + 现场语音提醒 |
| **日志语义总结** | VLM 汇总当天活动("今天发生了什么")与异常回顾("今日异常回顾",区分候选触发与复核确认真) |
| **远程操控** | 公网 MQTT 双向协议:手机异地发快照 / 视觉问答 / 时间线 / 事件查询 / 历史异常快照 / 设备状态,req_id 应答关联 + token 认证 + base64 现场图回传 |
| **远程实时视频** | MQTT 信令按需推流:板端零转码推 RTMP → 云端 MediaMTX 转 HLS → 网页多端同看,关流即停 |
| **单 NPU 资源仲裁** | 复核/语音对话/巡检/远程问答共用一颗 NPU 与单 VLM 执行线程:优先级队列 + rkllm_abort 抢占 + 超时降级 |
| **视觉/音视频** | 主码流 4K 采集→RGA 硬件缩放→1080p H.265 编码(全链路零拷贝)+ 子码流 640p H.264(带 AI 框/骨架),双路 30fps;MP4 循环录像(断电安全)+ 异常事件截存事发前后各 5 秒 + RTSP AAC 音轨 |
| **语音识别** | zipformer 流式 ASR(中英双语,边说边识别):语音流实时匹配呼救词,命中自动抓帧交 VLM 判断真伪 |
| **语音对话** | 多唤醒词 → 板端 VLM 流式对话(全本地,断网可用)→ 本地 Piper TTS(NPU)逐句播报;云端 DeepSeek+火山 TTS 可配置回退 |
| **端云可选** | tts_provider=piper\|cloud、vlm_enable=0/1 一行切换(0=回退云端 DeepSeek) |

## 架构

```
┌────────────── 板端 RK3588 ──────────────┐        ┌───── 网络/云端 ─────┐
│ 摄像头 → V4L2(DMABUF) → RGA → MPP 双码流 ├─RTSP──→│ VLC 实时观看        │
│      ↓                                   │        │ 公网 MQTT Broker    │
│  RKNN NPU: yolov8n-pose                 │   ◄───►│  (mosquitto)        │
│      ↓ 追踪器跌倒候选                     │  MQTT  │    ↕                │
│  ┌────────────────────────────────┐     │        │ 网页客户端(手机/PC)  │
│  │ ChatEngine chat_llm 线程(单 VLM 执行线程)│      │ (快照/问答/视频)     │
│  │  优先级1 跌倒复核: 候选图→VLM→分级报警 │      │ MediaMTX 转 HLS      │
│  │  优先级2 语音对话: VLM 流式→Piper 逐句播 │      │ DeepSeek(可选回退)   │
│  │  优先级3 远程命令: 快照/问答/时间线/推流│      └────────────────────┘
│  │  优先级4 周期巡检: 抓帧→摘要→时间线    │
│  │  抢占: rkllm_abort + 句级丢句 + 超时降级 │
│  └────────────────────────────────┘     │
│  FrameProvider(按需零拷贝快照通道)        │
│  ASR(zipformer)→唤醒/求救→ChatEngine     │
│  SQLite(事件/时间线,幂等迁移) + MP4 录像   │
└──────────────────────────────────────────┘
```

**线程模型**:H.265 编码 / H.264+AI 编码 / AI 推理 / 事件处理(复核编排) / 录像 / MQTT(发布+订阅) /
音频采集 / ASR 推理 / chat_llm(全部 VLM 调用) / tts_synth / tts_play / 健康监控。
实时链路 nice -5 绑大核,软实时链路 nice 10。

**全链路零拷贝**:V4L2 DMABUF → MPP → RGA → RKNN 全 fd 传递,CPU 只做调度与绘制,
编码/缩放/旋转全硬件化。

## 技术栈

| 层 | 技术 |
|---|---|
| 采集/编码 | V4L2 MPLANE + DMABUF 零拷贝;RK MPP(H.265/H.264 硬编);RGA 硬件缩放 |
| 流媒体/通信 | live555 RTSP;MQTT(paho C,QoS1,发布+订阅,断线指数退避+重订阅);RTMP/HLS(MediaMTX) |
| 视觉 AI | RKNN(NPU):yolov8n-pose 混合量化、Qwen3-VL(2B/4B,RKLLM w8a8 + vision rknn)、zipformer 流式 ASR |
| 大模型 | 板端 RKLLM(keep_history KV 多轮、rkllm_abort 抢占、多模态 vision 编码) |
| TTS | 本地 Piper(NPU decoder,全离线)/ 云端火山(可配置回退) |
| 存储 | MP4 循环录像(断电安全)+ SQLite(事件/时间线,幂等 ALTER 迁移) |
| 语言 | C++11(paroli C++20 静态库经 C 边界混链),10+ 线程,手写 HTTPS/SSE 客户端、cJSON |

## 目录结构

```
server/
├── TLServer/          板端主程序(CMake 交叉编译)
│   ├── AudioCapture    ALSA 采集(多路分发)
│   ├── AudioPlayback   ALSA 播放(重采样+会话状态机)
│   ├── EncodeThread    视频编码+AI+事件(复核编排)
│   └── config          配置系统([vlm_pipeline] 等)
├── cpp/
│   ├── chat_engine     语音对话引擎(状态机+优先级仲裁+巡检+远程命令+按需推流)
│   ├── vlm_engine      RKLLM 封装(多模态+流式+rkllm_abort 抢占)
│   ├── frame_provider  按需零拷贝快照通道(巡检/远程共用)
│   ├── EventManager    SQLite(幂等迁移+时间线+复核回填)
│   ├── mqtt_thread     MQTT 发布/订阅线程(原始发布原语)
│   ├── asr_thread      zipformer 流式 ASR 引擎
│   ├── pose_classifier 跌倒/坐姿多特征投票
│   └── 3rdparty/       paroli(Piper)/kaldi-native-fbank/cJSON/rknpu2
client/
├── web_demo/           网页远程操控面板(手机/电脑浏览器直连,快照/视频/今日总结)
└── remote_demo/        远程操控命令行客户端(协议调试用)
```

> 注：本地另有 `docs/` 目录（开发日志、技术文档等），为个人资料未上传。

## 许可与说明

个人学习项目。云端 API key 明文存板端配置、TLS 跳过证书校验均为个人场景权衡;
商用需服务端中转与证书校验。
