#ifndef APP_CONFIG_H
#define APP_CONFIG_H

// ============================================================
// 公共系统/平台头文件
// （原 const.h 的包含全部整合到此，其他文件只包含 config.h 即可）
// ============================================================
#include <iostream>
#include <stdio.h>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <vector>
#include <deque>
#include <cstdint>
#include <thread>
#include <mutex>
#include <atomic>
#include <csignal>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <linux/videodev2.h>
#include <rockchip/rk_mpi.h>
#include <rockchip/mpp_buffer.h>
#include <rockchip/mpp_frame.h>
#include <rockchip/mpp_packet.h>
#include <FramedSource.hh>
#include <UsageEnvironment.hh>
#include <OnDemandServerMediaSubsession.hh>
#include <H265VideoStreamFramer.hh>
#include <H265VideoRTPSink.hh>
#include <H264VideoStreamFramer.hh>
#include <H264VideoRTPSink.hh>
#include <BasicUsageEnvironment.hh>
#include <RTSPServer.hh>
#include <ServerMediaSession.hh>

// ============================================================
// 全局共享结构体（原 const.h）
// ============================================================
struct H265EncodePacket
{
    std::vector<uint8_t> data;
    timeval timestamp{};
    bool is_idr = false;   // MPP 编码后填充
};

// ============================================================
// 全局控制变量（原 const.h）
// 定义统一放在 config.cpp，TLmain.cpp 不再定义
// ============================================================
extern volatile char watchVariable;      // live555 事件循环退出标志
extern std::atomic<bool> exit_flag;      // 全局退出标志
// ============================================================
// 编码线程存活标志（监控线程用）
// ============================================================
extern std::atomic<bool> g_h265_thread_alive;
extern std::atomic<bool> g_h264_thread_alive;
// ============================================================
// 编译期功能开关
// ============================================================
// 1 = 开启主码流本地循环录像 + 跌倒事件录像
// 0 = 关闭全部录像功能（调试阶段使用，减少SD卡写入）
#define ENABLE_VIDEO_RECORDING 1

// ============================================================
// 配置结构体（从 config.ini 加载，均有默认值）
// ============================================================
struct MainStreamConfig {
    bool enable = true;                          // 主码流总开关（=0 时无 4K 采集/RGA 缩放/
                                                // RTSP /h265/循环录像；测试用，如 A/B 隔离
                                                // RGA 排队实验。关主码流后子码流不受影响）
    std::string video_device = "/dev/video62";   // MIPI CSI1 rkisp mainpath
    // ★主码流架构（2026-09-10 定稿）★：采集固定传感器原生 4K（全视野），
    // RGA 缩放到以下"编码尺寸"再编码——全视野 + 1080p 级带宽/内存。
    // 背景：正点 BSP 的 mainpath 只有 1080p 与 4K 输出正常；1080p 采集是
    // 1:1 裁切视野小，1920x1440/2560x1440 出"两个对称画面"。
    // 想更清晰可把编码尺寸提到 2560x1440（码率配套 8Mbps）
    int width = 1920;                    // 编码尺寸（非采集尺寸）
    int height = 1080;
    int hor_stride = 1920;
    int ver_stride = 1088;
    int frame_size = 1920 * 1088 * 3 / 2;
    int bitrate = 4000000;
    int bitrate_max = 4500000;
    int bitrate_min = 3000000;
    int fps = 30;
    int gop = 30;
    int buffer_count = 4;                // 4K 采集缓冲 4×12.4MB=50MB（够 30fps 双缓冲流）
    int rtsp_port = 8554;
    std::string rtsp_path = "h265";
    int rtp_out_buf_max = 600000;       // RTSP 输出缓冲上限（4Mbps 码率下关键帧可达 300KB+）
    bool rtsp_audio_enable = true;      // 主码流 RTSP 是否携带 AAC 音频轨
    // 编码器软件旋转（0/90/180/270，顺时针）：摄像头模组物理竖装，传感器直出
    // 画面顺时针转 90°；rotate=270 让编码输出转正为竖屏（尺寸随之互换，
    // 如 1920x1080 → 1080x1920）。MPP 硬件旋转零额外开销，画面与录像一并转正
    int rotate = 270;
    int extra_copies = 0;    // A/B 对照实验：每帧插入的全帧 CPU 拷贝次数（0=零拷贝基线；
                            // 模拟非零拷贝管线；仅测试用，开启时画面会花，只看 [MAIN]/[SUB] 统计行）
};

struct SubStreamConfig {
    std::string video_device = "/dev/video63";   // 同 sensor rkisp 低分辨率输出
    int width = 640;
    int height = 480;
    int hor_stride = 640;
    int ver_stride = 480;
    int frame_size = 640 * 480 * 3 / 2;
    int bitrate = 1500000;
    int bitrate_max = 2000000;
    int bitrate_min = 1000000;
    int fps = 30;
    int gop = 15;
    int buffer_count = 4;
    std::string rtsp_path = "h264";
    int rtp_out_buf_max = 300000;       // RTSP 输出缓冲上限（1.5Mbps 码率，300KB 足够）
    // 编码器软件旋转（同主码流）：AI 链路本身在竖屏空间推理/画框，rotate=270
    // 编码输出转正后 AI 框与画面一起旋转、相对位置不变
    int rotate = 270;
    int extra_copies = 0;    // A/B 对照实验：每帧插入的全帧 CPU 拷贝次数（0=零拷贝基线；
                            // 模拟非零拷贝管线；仅测试用，开启时画面会花，只看 [MAIN]/[SUB] 统计行）
};

struct AiConfig {
    std::string model_path = "./model/yolov8n-pose.rknn";
    int frame_interval = 2;
    float box_thresh = 0.25f;
    float nms_thresh = 0.45f;
    int npu_core_num = 0;            // NPU 核数实验键:0=不设置(AUTO 单核,旧行为) 1=CORE_0 2=CORE_0_1 3=CORE_0_1_2
};

struct RecordConfig {
    std::string root_dir = "/mnt/sdcard/record_main";
    int slice_sec = 300;
    int pre_buffer_sec = 5;
    int post_record_sec = 5;
    double disk_watermark = 0.85;
    // 音频录制（ES8388 声卡1；打开失败自动降级，不影响视频）
    bool audio_enable = true;
    std::string audio_device = "hw:1,0";
    int audio_rate = 44100;         // 44.1kHz：VLC/播放器兼容性最好（16kHz 在部分 VLC 版本下音频轨不发声）
    int audio_channels = 2;         // ES8388 采集最小 2 声道
    bool audio_silence_warn = true; // 检测到直流/静音时告警（提示麦克风可能未接线）
    int audio_bitrate = 24000;      // AAC-LC 目标码率 bps（16kHz mono 典型值）
    // 单声道来源：0=(L+R)/2降混 1=仅左声道 2=仅右声道
    // （实测板载麦克风只接声道0，取 1 避免降混损失 6dB 信噪比）
    int audio_mono_source = 1;
    bool audio_kws_enable = false;  // 音频关键词识别预留开关（本期空实现）
    bool audio_debug_dump = false;  // 调试：原始采集 PCM 直写 ./audio_raw.pcm
};

struct EventConfig {
    std::string device_id = "rk3588_home_001";
    std::string image_dir = "./events/image";
    std::string database_path = "./events/database/events.db";
};

struct MqttConfig {
    std::string broker_url = "tcp://192.168.5.77:1883";
    std::string client_id = "rk3588_fall_detector_{device_id}";  // {device_id} 加载时替换——固定 client_id 会让多设备互相踢下线
    std::string topic = "home/fall";
    std::string timeline_topic = "home/{device_id}/timeline";   // 状态摘要上行（{device_id} 加载时替换）
    std::string cmd_topic = "home/{device_id}/cmd";             // 下行命令（设备订阅的唯一下行点）
    std::string resp_topic = "home/{device_id}/resp";           // 上行命令应答（带 req_id）
    std::string state_topic = "home/{device_id}/state";         // 上行设备状态（retained：online/offline）
    bool subscribe_enable = true;                               // 订阅 cmd_topic 开关（[remote] enable=0 时自动失效）
    std::string username = "mqtt1";
    std::string password = "123456";
    int keepalive = 15;
    int connect_timeout = 5;
    int publish_ack_timeout_ms = 5000;  // QoS1 PUBACK 等待上限。公网/大 payload 需放宽：
                                        // 快照 base64 约 27~55KB，1s 等不到 ACK 会被判失败并回队重发（对端收到重复应答）
    int include_image_base64 = 1;   // 1=报警消息携带现场图 base64（缩略图），0=仅带本地路径
};

struct AsrConfig {
    bool enable = false;                                   // 语音识别总开关
    std::string model_dir = "./model";                     // encoder/decoder/joiner-epoch-99-avg-1.rknn 所在目录
    std::string vocab_path = "./model/vocab.txt";
    std::string keywords = "救命,来人,救救我,帮帮我";       // 逗号分隔，命中即上报 help_call
    int cooldown_sec = 30;                                 // 同一关键词两次上报的最小间隔
    bool silence_gate = true;                              // 静音时丢弃 PCM 不推理（省 NPU）
    bool debug_text_log = false;                           // 识别全文打日志（仅调试，永不上报全文）
    std::string text_log_file = "";                        // 非空：识别文本按说话段落追加写入该文件
    std::string debug_wav_path = "";                       // 非空：启动时离线跑一遍该 wav 自检
    int segment_timeout_ms = 2000;                         // 说话停顿多少毫秒判定一段结束（越小响应越快，太短会切断慢语速句子）
    int npu_core_num = 0;            // NPU 核数实验键:0=不设置(AUTO 单核,旧行为) 1=CORE_0 2=CORE_0_1 3=CORE_0_1_2（encoder/decoder/joiner 同用）
    // ---- 运行时字段（TLmain 从 [record] 复制，不写入 config.ini） ----
    int capture_rate = 44100;
    int capture_channels = 2;
    int mono_source = 1;                                   // 0=(L+R)/2 1=仅左 2=仅右
};

// 本地 Piper TTS（RK3588 NPU decoder，替代云端火山 TTS）
struct PiperTtsConfig {
    std::string encoder = "./model/piper/encoder.onnx";          // streaming encoder（CPU ONNX）
    std::string decoder = "./model/piper/decoder_rk3588.rknn";   // NPU 版 decoder（或 decoder.onnx CPU 兜底）
    std::string config_json = "./model/piper/config.json";       // 音色配置（音素表/采样率/espeak voice）
    std::string espeak_data = "./model/piper/espeak-ng-data";    // espeak-ng 数据目录
    int npu_core = 0;                                            // 0=auto 1/2/3（仅 NPU decoder 生效）
};

// 本地大模型（RK3588 板端 RKLLM 部署 Qwen3-VL，替代云端 DeepSeek）
struct VlmConfig {
    bool enabled = false;                                          // true=对话走板端 VLM；false=云端 DeepSeek
    // 模型切换机制:改本行路径+重启即可在官方 4B 与 h4g 剪枝版间切换(两文件均在
    // 板端 /userdata/models/,根分区放不下大模型已统一迁出):
    //   官方基线: /userdata/models/qwen3-vl-4b-instruct_w8a8_rk3588.rkllm
    //   h4g 剪枝(部署默认,decode +19%): /userdata/models/qwen3-vl-4b-pruned-h4g-bilingual-merged_w8a8_rk3588.rkllm
    std::string rkllm_model = "/userdata/models/qwen3-vl-4b-instruct_w8a8_rk3588.rkllm";   // 官方 4B 基线
    std::string vision_model = "/userdata/models/qwen3-vl-4b_vision_rk3588.rknn";   // 4B 视觉塔(两模型共用,不随 llm 切换)
    int max_context_len = 4096;      // 上下文窗口（token 数；含系统提示词+历史+图像 token）
    int max_new_tokens = 512;        // 单次回复生成上限
    int top_k = 1;                   // 贪心解码（1）最稳定；>1 增加多样性
    float temperature = 0.7f;        // 采样温度（top_k=1 时无效）
    int npu_core_num = 3;            // 视觉编码器 NPU 核数（0=auto 1/2/3）
};

// VLM 视觉业务流水线（[vlm_pipeline] 节）：跌倒复核 + 周期巡检 + 远程问答
// 老 config.ini 无本节 → 以下默认值生效；vlm_enable=0 时全部功能旁路（旧行为零变化）
struct VlmPipelineConfig {
    // ---- 跌倒复核（两级检测：YOLO 候选 → VLM 看图确认） ----
    bool confirm_enable = true;      // 复核总开关（VLM 可用时生效）
    int  confirm_timeout_ms = 30000; // event_proc 等待复核 future 的上限，超时按"未确认"直报
                                     // （4B 实测预算：vision 2.4s + prefill 2.9s + 生成 40~80tok@4.7
                                     //   tok/s≈8.5~17s ≈ 14~22s；再含让位等待，30s 留裕量；
                                     //   2B 时代 12s 已不适用）
    std::string confirm_fallback = "alarm";  // alarm=未确认直报 attention（宁报勿漏）| none=不报
    bool ignore_report = true;       // VLM 判"否"时仍发 cleared 低打扰通知（监护人可见"已排除"）
    std::string confirm_prompt =
        "你是异常事件复核系统。这是监控摄像头拍到的画面。"
        "请判断画面中是否存在异常（人员倒地、明火、烟雾、大量漏水等）。"
        "请分两行输出：先输出结果行（结果:是 或 结果:否 或 结果:不确定），再输出原因行（原因:不超过30字的一句话）。"
        "若结果为是，请另起一行输出慰问行（慰问:对画面中的人说的一句简短温和的安慰话，提醒保持不动等待救援，不超过30字）";
    // ---- 周期状态巡检（阶段B） ----
    bool status_enable = true;       // 巡检总开关
    int  status_interval_sec = 60;   // 巡检周期（4B 一轮实测 ~14s，60s 周期占 23% NPU，
                                    // 与 2B 时代 30s/7s 同占比；火警需求：有人无人都巡检）
    int  status_min_idle_sec = 10;   // 距上次对话完成的最短间隔（防与语音轮抢 NPU）
    std::string status_prompt =
        "观察画面，用一句中文客观描述当前场景：有人的话说明人数和大致活动；"
        "发现异常（明火、烟雾、漏水、人员倒地等）必须明确指出。只输出这一句，不超过30字。";
    // ---- 远程操控（阶段C） ----
    bool remote_enable = true;       // 远程命令总开关
    std::string remote_token = "";   // 命令认证 token（空=不校验，公网必须设置）
    bool remote_voice = false;       // 远程问答是否本地语音播报（默认关：避免房间突然出声）
    // 远程问答的临时人设（问答期间切换，结束后恢复陪伴助手人设——实测不切换
    // 时"现在发生了什么"会得到陪伴口吻的闲聊答复而非场景描述）
    std::string remote_chat_prompt =
        "你是智能家居监控终端的场景观察员。请根据监控画面简洁客观地回答用户的问题，"
        "只描述画面中实际发生的事情，不要闲聊。";
    // ---- 远程实时视频（按需推流：start_stream 命令起 ffmpeg 转推到云端 MediaMTX） ----
    std::string stream_rtmp_url = "";   // RTMP 推流目标（空=功能关闭）；如 rtmp://IP:1935/live/xxx
    // 推流源（板端本地 RTSP）：
    //   默认子码流 h264（H.264 640p 带 AI 框，全平台浏览器可播）
    //   主码流 h265（H.265 1080p 高清，仅 iPhone Safari 原生可播；Android/PC Chrome 黑屏）
    std::string stream_src_url = "rtsp://127.0.0.1:8554/h264";
    // ---- 求助事件视觉判定（有人喊救命/说好痛 → 抓帧看图二次分析） ----
    // {text} 占位符替换为求助者原话；结果:真|假|不确定 三选一（parseVerdict 解析）
    std::string sos_prompt =
        "你是家庭监护终端。有人刚刚说了求助的话：【{text}】。"
        "请仔细观察画面：判断画面中的人是否真的处于痛苦、受伤或危险状态"
        "（表情痛苦、倒地、捂着身体部位、无法行动为真；表情放松、微笑、正常活动为假）。"
        "请分两行输出：先输出结果行（结果:真 或 结果:假 或 结果:不确定），再输出原因行（原因:不超过30字的一句话）";
    std::string sos_true_reply = "我已经通知家人马上过来帮你，请保持不动，深呼吸，坚持一下";   // 判定为真：本地播报安慰
    std::string sos_false_reply = "看你的状态还好，是遇到什么麻烦了吗？跟我说说";               // 判定为假：本地播报询问
    std::string sos_unknown_reply = "我没法确认你的情况，已经先通知家人了，别慌，跟我说说发生了什么"; // 判定不确定：本地播报询问
    // ---- 巡检异常词升级报警（火警等：VLM 巡检摘要命中即 urgent 上报 + 现场播报） ----
    std::string patrol_alert_words = "明火,起火,火灾,火苗,大火,烟雾,浓烟,冒烟,漏水";   // 逗号分隔：摘要命中任一即报警
    std::string fire_alert_reply = "检测到疑似火情，请立即远离，已通知联系人";           // 命中时本地播报（消防提醒，不限定"老人"）
    // ---- 跌倒确认后的现场慰问（复核一次调用内产出：confirm_prompt 要求模型
    //      在"结果:是"时附一行"慰问:"；模型未输出慰问行时用此兜底文案） ----
    std::string fall_console_fallback = "我已通知家人来帮你，请保持不动，等待救援";
};

struct ChatConfig {
    bool enable = false;                                          // 语音对话总开关
    std::string llm_api_host = "api.deepseek.com";                // LLM host（HTTPS 443）
    std::string llm_api_key = "";                                 // DeepSeek API key（sk-...）
    std::string llm_model = "deepseek-v4-flash";                  // 旧名 deepseek-chat 已停用（2026-07）
    std::string tts_api_host = "openspeech.bytedance.com";        // 火山 TTS host
    std::string tts_app_id = "";                                  // 火山 appid
    std::string tts_access_token = "";                            // 火山 access token
    std::string tts_voice_type = "zh_female_vv_uranus_bigtts";   // TTS2.0 音色（vivi 2.0）
    std::string tts_cluster = "volcano_tts";
    std::string tts_provider = "cloud";                           // TTS 后端: cloud=火山引擎 | piper=本地 NPU（全离线）
    std::string wake_word = "你好";                               // 唤醒词（逗号分隔可多个；自动并入 ASR 关键词表）
    int wake_timeout_sec = 10;                                   // 唤醒后等待第一句话的超时
    int idle_timeout_sec = 15;                                   // 每轮对话完成后，多少秒无说话退出对话模式
    std::string vision_words = "看到,看见,看看,画面,什么情况,谁在,在哪,有没有人,好痛,好疼,疼";  // 逗号分隔：语音问句命中任一即抓当前帧带图给 VLM（视觉轮）
    std::string sos_words = "好痛,好疼,好难受,我摔了,摔倒了,我不行了";  // 逗号分隔：对话段落命中任一即走求助视觉判定链（SOS）
    std::string assistant_name = "小安";                          // 助手名字（占位符 {name} 会被替换进 system_prompt）
    std::string system_prompt =
        "你是智能家居助手，你的名字叫{name}。"
        "用口语化的中文回复家人，回答简洁自然、内容清楚，一般两三句话以内。"
        "语气温和耐心，可以聊聊家常、天气、健康提醒。";
    int max_tokens = 512;                                        // LLM 回复长度上限（token 数）
    float temperature = 0.7f;                                    // LLM 随机性（0 最确定，1 更活泼）
    float tts_speed_ratio = 0.95f;                               // TTS 语速（1.0=正常，稍快可降低感知延迟）
    std::string playback_device = "hw:1,0";                       // ES8388 声卡1；直连硬件（dmix 与采集并发会阻塞）
    VlmConfig vlm;                                                 // 板端 VLM 配置（[local_llm] 节）
    PiperTtsConfig piper;                                          // 本地 Piper TTS 配置（[piper_tts] 节）
    VlmPipelineConfig vlm_pipeline;                                // VLM 视觉业务流水线（[vlm_pipeline] 节：复核/巡检/远程）
    // 注意：API key/access token 明文落板（config.ini），个人项目可接受；
    // 商用须改为服务端中转或加密存储。跳过证书校验见 https_client.cpp。
};

struct AppConfig {
    MainStreamConfig main_stream;
    SubStreamConfig sub_stream;
    AiConfig ai;
    RecordConfig record;
    EventConfig event;
    MqttConfig mqtt;
    AsrConfig asr;
    ChatConfig chat;

    bool load(const std::string& path);
    bool saveDefault(const std::string& path);
};

// ============================================================
// 全局唯一配置实例
// ============================================================
extern AppConfig g_config;

// 逗号分隔字符串拆分（唤醒词/关键词等多值配置共用）
inline std::vector<std::string> splitCsv(const std::string &s)
{
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos < s.size()) {
        size_t comma = s.find(',', pos);
        std::string item = s.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        // 去空白
        size_t b = item.find_first_not_of(" \t");
        size_t e = item.find_last_not_of(" \t");
        if (b != std::string::npos && !item.empty())
            out.push_back(item.substr(b, e - b + 1));
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    return out;
}

#endif // APP_CONFIG_H