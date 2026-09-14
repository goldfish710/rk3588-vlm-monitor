#include "config.h"
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <cstring>

// ============================================================
// 全局变量定义（原 TLmain.cpp 中的定义移到这里）
// ============================================================
volatile char watchVariable = 0;
std::atomic<bool> exit_flag(false);
std::atomic<bool> g_h265_thread_alive(false);
std::atomic<bool> g_h264_thread_alive(false);
// ============================================================
// 全局配置实例
// ============================================================
AppConfig g_config;

// ============================================================
// INI 解析辅助函数
// ============================================================
static std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

static bool parseKeyValue(const std::string& line, std::string& key, std::string& value) {
    size_t pos = line.find('=');
    if (pos == std::string::npos) return false;
    key = trim(line.substr(0, pos));
    value = trim(line.substr(pos + 1));
    size_t comment = value.find('#');
    if (comment != std::string::npos) {
        value = trim(value.substr(0, comment));
    }
    return !key.empty();
}

static int toInt(const std::string& v, int def) {
    if (v.empty()) return def;
    return atoi(v.c_str());
}

static float toFloat(const std::string& v, float def) {
    if (v.empty()) return def;
    return atof(v.c_str());
}

static double toDouble(const std::string& v, double def) {
    if (v.empty()) return def;
    return atof(v.c_str());
}

// ============================================================
// 加载配置文件
// ============================================================
bool AppConfig::load(const std::string& path) {
    std::ifstream f(path.c_str());
    if (!f.is_open()) {
        std::cerr << "[Config] 配置文件 " << path
                  << " 不存在，使用全部默认参数" << std::endl;
        return false;
    }

    std::string section;
    std::string line;
    bool any_change = false;

    while (std::getline(f, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;

        if (line[0] == '[' && line[line.size()-1] == ']') {
            section = line.substr(1, line.size() - 2);
            continue;
        }

        std::string key, value;
        if (!parseKeyValue(line, key, value)) continue;

        // ----- [main_stream] -----
        if (section == "main_stream") {
            if (key == "video_device")        main_stream.video_device = value;
            else if (key == "width")          main_stream.width = toInt(value, main_stream.width);
            else if (key == "height")         main_stream.height = toInt(value, main_stream.height);
            else if (key == "hor_stride")     main_stream.hor_stride = toInt(value, main_stream.hor_stride);
            else if (key == "ver_stride")     main_stream.ver_stride = toInt(value, main_stream.ver_stride);
            else if (key == "bitrate")        main_stream.bitrate = toInt(value, main_stream.bitrate);
            else if (key == "bitrate_max")    main_stream.bitrate_max = toInt(value, main_stream.bitrate_max);
            else if (key == "bitrate_min")    main_stream.bitrate_min = toInt(value, main_stream.bitrate_min);
            else if (key == "fps")            main_stream.fps = toInt(value, main_stream.fps);
            else if (key == "gop")            main_stream.gop = toInt(value, main_stream.gop);
            else if (key == "buffer_count")   main_stream.buffer_count = toInt(value, main_stream.buffer_count);
            else if (key == "rtsp_port")      main_stream.rtsp_port = toInt(value, main_stream.rtsp_port);
            else if (key == "rtsp_path")      main_stream.rtsp_path = value;
            else if (key == "rtp_out_buf_max") main_stream.rtp_out_buf_max = toInt(value, main_stream.rtp_out_buf_max);
            else if (key == "rtsp_audio_enable") main_stream.rtsp_audio_enable = (toInt(value, 1) != 0);
            else if (key == "rotate")          main_stream.rotate = toInt(value, main_stream.rotate);
            else if (key == "extra_copies")   main_stream.extra_copies = toInt(value, main_stream.extra_copies);
            else if (key == "enable")         main_stream.enable = toInt(value, main_stream.enable);
            any_change = true;
        }
        // ----- [sub_stream] -----
        else if (section == "sub_stream") {
            if (key == "video_device")        sub_stream.video_device = value;
            else if (key == "width")          sub_stream.width = toInt(value, sub_stream.width);
            else if (key == "height")         sub_stream.height = toInt(value, sub_stream.height);
            else if (key == "hor_stride")     sub_stream.hor_stride = toInt(value, sub_stream.hor_stride);
            else if (key == "ver_stride")     sub_stream.ver_stride = toInt(value, sub_stream.ver_stride);
            else if (key == "bitrate")        sub_stream.bitrate = toInt(value, sub_stream.bitrate);
            else if (key == "bitrate_max")    sub_stream.bitrate_max = toInt(value, sub_stream.bitrate_max);
            else if (key == "bitrate_min")    sub_stream.bitrate_min = toInt(value, sub_stream.bitrate_min);
            else if (key == "fps")            sub_stream.fps = toInt(value, sub_stream.fps);
            else if (key == "gop")            sub_stream.gop = toInt(value, sub_stream.gop);
            else if (key == "buffer_count")   sub_stream.buffer_count = toInt(value, sub_stream.buffer_count);
            else if (key == "rtsp_path")      sub_stream.rtsp_path = value;
            else if (key == "rtp_out_buf_max") sub_stream.rtp_out_buf_max = toInt(value, sub_stream.rtp_out_buf_max);
            else if (key == "rotate")          sub_stream.rotate = toInt(value, sub_stream.rotate);
            else if (key == "extra_copies")   sub_stream.extra_copies = toInt(value, sub_stream.extra_copies);
            any_change = true;
        }
        // ----- [ai] -----
        else if (section == "ai") {
            if (key == "model_path")          ai.model_path = value;
            else if (key == "frame_interval") ai.frame_interval = toInt(value, ai.frame_interval);
            else if (key == "box_thresh")     ai.box_thresh = toFloat(value, ai.box_thresh);
            else if (key == "nms_thresh")     ai.nms_thresh = toFloat(value, ai.nms_thresh);
            else if (key == "npu_core_num")   ai.npu_core_num = toInt(value, ai.npu_core_num);
            any_change = true;
        }
        // ----- [record] -----
        else if (section == "record") {
            if (key == "root_dir")            record.root_dir = value;
            else if (key == "slice_sec")      record.slice_sec = toInt(value, record.slice_sec);
            else if (key == "pre_buffer_sec") record.pre_buffer_sec = toInt(value, record.pre_buffer_sec);
            else if (key == "post_record_sec") record.post_record_sec = toInt(value, record.post_record_sec);
            else if (key == "disk_watermark") record.disk_watermark = toDouble(value, record.disk_watermark);
            else if (key == "audio_enable")        record.audio_enable = (toInt(value, 1) != 0);
            else if (key == "audio_device")        record.audio_device = value;
            else if (key == "audio_rate")          record.audio_rate = toInt(value, record.audio_rate);
            else if (key == "audio_channels")      record.audio_channels = toInt(value, record.audio_channels);
            else if (key == "audio_silence_warn")  record.audio_silence_warn = (toInt(value, 1) != 0);
            else if (key == "audio_bitrate")       record.audio_bitrate = toInt(value, record.audio_bitrate);
            else if (key == "audio_mono_source")   record.audio_mono_source = toInt(value, record.audio_mono_source);
            else if (key == "audio_kws_enable")    record.audio_kws_enable = (toInt(value, 0) != 0);
            else if (key == "audio_debug_dump")  record.audio_debug_dump = (toInt(value, 0) != 0);
            any_change = true;
        }
        // ----- [event] -----
        else if (section == "event") {
            if (key == "device_id")           event.device_id = value;
            else if (key == "image_dir")      event.image_dir = value;
            else if (key == "database_path")  event.database_path = value;
            any_change = true;
        }
        // ----- [mqtt] -----
        else if (section == "mqtt") {
            if (key == "broker_url")          mqtt.broker_url = value;
            else if (key == "client_id")      mqtt.client_id = value;
            else if (key == "topic")          mqtt.topic = value;
            else if (key == "timeline_topic") mqtt.timeline_topic = value;
            else if (key == "cmd_topic")      mqtt.cmd_topic = value;
            else if (key == "resp_topic")     mqtt.resp_topic = value;
            else if (key == "state_topic")    mqtt.state_topic = value;
            else if (key == "subscribe_enable") mqtt.subscribe_enable = (toInt(value, 1) != 0);
            else if (key == "username")       mqtt.username = value;
            else if (key == "password")       mqtt.password = value;
            else if (key == "keepalive")      mqtt.keepalive = toInt(value, mqtt.keepalive);
            else if (key == "connect_timeout") mqtt.connect_timeout = toInt(value, mqtt.connect_timeout);
            else if (key == "publish_ack_timeout_ms") mqtt.publish_ack_timeout_ms = toInt(value, mqtt.publish_ack_timeout_ms);
            else if (key == "include_image_base64") mqtt.include_image_base64 = toInt(value, mqtt.include_image_base64);
            any_change = true;
        }
        // ----- [asr] -----
        else if (section == "asr") {
            if (key == "asr_enable")          asr.enable = (toInt(value, 0) != 0);
            else if (key == "asr_model_dir")  asr.model_dir = value;
            else if (key == "asr_vocab_path") asr.vocab_path = value;
            else if (key == "asr_keywords")   asr.keywords = value;
            else if (key == "asr_cooldown_sec") asr.cooldown_sec = toInt(value, asr.cooldown_sec);
            else if (key == "asr_silence_gate") asr.silence_gate = (toInt(value, 1) != 0);
            else if (key == "asr_debug_text_log") asr.debug_text_log = (toInt(value, 0) != 0);
            else if (key == "asr_text_log_file") asr.text_log_file = value;
            else if (key == "asr_debug_wav_path") asr.debug_wav_path = value;
            else if (key == "asr_segment_timeout_ms") asr.segment_timeout_ms = toInt(value, asr.segment_timeout_ms);
            else if (key == "asr_npu_core_num")  asr.npu_core_num = toInt(value, asr.npu_core_num);
            any_change = true;
        }
        // ----- [chat] -----
        else if (section == "chat") {
            if (key == "chat_enable")          chat.enable = (toInt(value, 0) != 0);
            else if (key == "llm_api_host")    chat.llm_api_host = value;
            else if (key == "llm_api_key")     chat.llm_api_key = value;
            else if (key == "llm_model")       chat.llm_model = value;
            else if (key == "tts_api_host")    chat.tts_api_host = value;
            else if (key == "tts_app_id")      chat.tts_app_id = value;
            else if (key == "tts_access_token") chat.tts_access_token = value;
            else if (key == "tts_voice_type")  chat.tts_voice_type = value;
            else if (key == "tts_cluster")     chat.tts_cluster = value;
            else if (key == "tts_provider")    chat.tts_provider = value;
            else if (key == "wake_word")       chat.wake_word = value;
            else if (key == "wake_timeout_sec") chat.wake_timeout_sec = toInt(value, chat.wake_timeout_sec);
            else if (key == "idle_timeout_sec") chat.idle_timeout_sec = toInt(value, chat.idle_timeout_sec);
            else if (key == "vision_words")      chat.vision_words = value;
            else if (key == "sos_words")        chat.sos_words = value;
            else if (key == "assistant_name")  chat.assistant_name = value;
            else if (key == "system_prompt")   chat.system_prompt = value;
            else if (key == "max_tokens")      chat.max_tokens = toInt(value, chat.max_tokens);
            else if (key == "temperature")     chat.temperature = toFloat(value, chat.temperature);
            else if (key == "tts_speed_ratio") chat.tts_speed_ratio = toFloat(value, chat.tts_speed_ratio);
            else if (key == "playback_device") chat.playback_device = value;
            any_change = true;
        }
        // ----- [local_llm] 板端 VLM（RK3588 RKLLM 部署，替代云端 DeepSeek） -----
        else if (section == "local_llm") {
            if (key == "vlm_enable")             chat.vlm.enabled = (toInt(value, 0) != 0);
            else if (key == "rkllm_model")       chat.vlm.rkllm_model = value;
            else if (key == "vision_model")      chat.vlm.vision_model = value;
            else if (key == "max_context_len")   chat.vlm.max_context_len = toInt(value, chat.vlm.max_context_len);
            else if (key == "max_new_tokens")    chat.vlm.max_new_tokens = toInt(value, chat.vlm.max_new_tokens);
            else if (key == "top_k")             chat.vlm.top_k = toInt(value, chat.vlm.top_k);
            else if (key == "temperature")       chat.vlm.temperature = toFloat(value, chat.vlm.temperature);
            else if (key == "npu_core_num")      chat.vlm.npu_core_num = toInt(value, chat.vlm.npu_core_num);
            any_change = true;
        }
        // ----- [piper_tts] 本地 Piper TTS（RK3588 NPU，全离线） -----
        else if (section == "piper_tts") {
            if (key == "piper_encoder")          chat.piper.encoder = value;
            else if (key == "piper_decoder")     chat.piper.decoder = value;
            else if (key == "piper_config")      chat.piper.config_json = value;
            else if (key == "piper_espeak_data") chat.piper.espeak_data = value;
            else if (key == "piper_npu_core")    chat.piper.npu_core = toInt(value, chat.piper.npu_core);
            any_change = true;
        }
        // ----- [vlm_pipeline] VLM 视觉业务流水线（跌倒复核/巡检/远程） -----
        else if (section == "vlm_pipeline") {
            if (key == "confirm_enable")         chat.vlm_pipeline.confirm_enable = (toInt(value, 1) != 0);
            else if (key == "confirm_timeout_ms") chat.vlm_pipeline.confirm_timeout_ms = toInt(value, chat.vlm_pipeline.confirm_timeout_ms);
            else if (key == "confirm_fallback")  chat.vlm_pipeline.confirm_fallback = value;
            else if (key == "ignore_report")     chat.vlm_pipeline.ignore_report = (toInt(value, 1) != 0);
            else if (key == "confirm_prompt")    chat.vlm_pipeline.confirm_prompt = value;
            else if (key == "status_enable")     chat.vlm_pipeline.status_enable = (toInt(value, 1) != 0);
            else if (key == "status_interval_sec") chat.vlm_pipeline.status_interval_sec = toInt(value, chat.vlm_pipeline.status_interval_sec);
            else if (key == "status_min_idle_sec") chat.vlm_pipeline.status_min_idle_sec = toInt(value, chat.vlm_pipeline.status_min_idle_sec);
            else if (key == "status_prompt")     chat.vlm_pipeline.status_prompt = value;
            else if (key == "remote_enable")     chat.vlm_pipeline.remote_enable = (toInt(value, 1) != 0);
            else if (key == "remote_token")      chat.vlm_pipeline.remote_token = value;
            else if (key == "remote_voice")      chat.vlm_pipeline.remote_voice = (toInt(value, 0) != 0);
            else if (key == "remote_chat_prompt") chat.vlm_pipeline.remote_chat_prompt = value;
            else if (key == "stream_rtmp_url")  chat.vlm_pipeline.stream_rtmp_url = value;
            else if (key == "stream_src_url")   chat.vlm_pipeline.stream_src_url = value;
            else if (key == "sos_prompt")          chat.vlm_pipeline.sos_prompt = value;
            else if (key == "sos_true_reply")      chat.vlm_pipeline.sos_true_reply = value;
            else if (key == "sos_false_reply")     chat.vlm_pipeline.sos_false_reply = value;
            else if (key == "sos_unknown_reply")   chat.vlm_pipeline.sos_unknown_reply = value;
            else if (key == "patrol_alert_words") chat.vlm_pipeline.patrol_alert_words = value;
            else if (key == "fire_alert_reply")    chat.vlm_pipeline.fire_alert_reply = value;
            else if (key == "fall_console_fallback") chat.vlm_pipeline.fall_console_fallback = value;
            any_change = true;
        }
    }

    // 兼容：旧配置 audio_kws_enable=1 → 自动开 ASR
    if (record.audio_kws_enable)
        asr.enable = true;

    // {device_id} 占位符替换（多设备天然隔离；topic 模板在 saveDefault 中保持占位符形式）
    auto resolveTopic = [](std::string &tmpl, const std::string &dev) {
        size_t p = tmpl.find("{device_id}");
        if (p != std::string::npos) tmpl.replace(p, 11, dev);
    };
    resolveTopic(mqtt.timeline_topic, event.device_id);
    resolveTopic(mqtt.cmd_topic, event.device_id);
    resolveTopic(mqtt.resp_topic, event.device_id);
    resolveTopic(mqtt.state_topic, event.device_id);
    resolveTopic(mqtt.client_id, event.device_id);
    // 老配置文件里 client_id 是写死的（如 rk3588_fall_detector），没有占位符 →
    // 上面替换不生效。MQTT 按 client_id 唯一标识会话，两台设备用同一个 id 会互相踢下线
    // （表现为无限重连抖动）。这里兜底追加 device_id 保证唯一。
    if (mqtt.client_id.find(event.device_id) == std::string::npos)
        mqtt.client_id += "_" + event.device_id;

    // 根据 stride 重算 frame_size（防止配置文件只改 stride 未改 size）
    main_stream.frame_size = main_stream.hor_stride * main_stream.ver_stride * 3 / 2;
    sub_stream.frame_size  = sub_stream.hor_stride  * sub_stream.ver_stride  * 3 / 2;

    if (any_change) {
        std::cout << "[Config] 配置文件加载成功: " << path << std::endl;
        std::cout << "[Config] 主码流: " << main_stream.width << "x" << main_stream.height
                  << " @" << main_stream.fps << "fps "
                  << main_stream.bitrate / 1000 << "kbps "
                  << "rtsp=/" << main_stream.rtsp_path << std::endl;
        std::cout << "[Config] 子码流: " << sub_stream.width << "x" << sub_stream.height
                  << " @" << sub_stream.fps << "fps "
                  << sub_stream.bitrate / 1000 << "kbps "
                  << "rtsp=/" << sub_stream.rtsp_path << std::endl;
        std::cout << "[Config] 录像: " << (ENABLE_VIDEO_RECORDING ? "开启" : "关闭")
                  << " 根目录=" << record.root_dir
                  << " 切片=" << record.slice_sec << "s"
                  << " 预录=" << record.pre_buffer_sec << "s"
                  << " 后录=" << record.post_record_sec << "s" << std::endl;
        std::cout << "[Config] 音频: " << (record.audio_enable ? "开启" : "关闭")
                  << " 设备=" << record.audio_device
                  << " " << record.audio_rate << "Hz/" << record.audio_channels << "ch"
                  << " 静音告警=" << (record.audio_silence_warn ? "开" : "关") << std::endl;
        std::cout << "[Config] MQTT: " << mqtt.broker_url
                  << " topic=" << mqtt.topic << std::endl;
    } else {
        std::cerr << "[Config] 配置文件解析失败或无有效配置，使用默认值" << std::endl;
    }

    return true;
}

// ============================================================
// 写出默认配置文件
// ============================================================
bool AppConfig::saveDefault(const std::string& path) {
    std::ofstream f(path.c_str());
    if (!f.is_open()) return false;

    f << "# ============================================\n";
    f << "# 嵌入式录像系统配置文件\n";
    f << "# ============================================\n\n";

    f << "[main_stream]\n";
    f << "video_device="  << main_stream.video_device << "\n";
    f << "width="         << main_stream.width << "\n";
    f << "height="        << main_stream.height << "\n";
    f << "hor_stride="    << main_stream.hor_stride << "\n";
    f << "ver_stride="    << main_stream.ver_stride << "\n";
    f << "bitrate="       << main_stream.bitrate << "\n";
    f << "bitrate_max="   << main_stream.bitrate_max << "\n";
    f << "bitrate_min="   << main_stream.bitrate_min << "\n";
    f << "fps="           << main_stream.fps << "\n";
    f << "gop="           << main_stream.gop << "\n";
    f << "buffer_count="  << main_stream.buffer_count << "\n";
    f << "rtsp_port="     << main_stream.rtsp_port << "\n";
    f << "rtsp_path="     << main_stream.rtsp_path << "\n";
    f << "rtp_out_buf_max=" << main_stream.rtp_out_buf_max << "\n";
    f << "rtsp_audio_enable=" << (main_stream.rtsp_audio_enable ? 1 : 0) << "\n";
    f << "; rotate: 编码器旋转 0/90/180/270（顺时针；270=模组竖装场景的转正档）\n";
    f << "rotate="          << main_stream.rotate << "\n";
    f << "extra_copies="    << main_stream.extra_copies << "\n";
    f << "enable="          << main_stream.enable << "\n";

    f << "[sub_stream]\n";
    f << "video_device="  << sub_stream.video_device << "\n";
    f << "width="         << sub_stream.width << "\n";
    f << "height="        << sub_stream.height << "\n";
    f << "hor_stride="    << sub_stream.hor_stride << "\n";
    f << "ver_stride="    << sub_stream.ver_stride << "\n";
    f << "bitrate="       << sub_stream.bitrate << "\n";
    f << "bitrate_max="   << sub_stream.bitrate_max << "\n";
    f << "bitrate_min="   << sub_stream.bitrate_min << "\n";
    f << "fps="           << sub_stream.fps << "\n";
    f << "gop="           << sub_stream.gop << "\n";
    f << "buffer_count="  << sub_stream.buffer_count << "\n";
    f << "rtsp_path="     << sub_stream.rtsp_path << "\n";
    f << "rtp_out_buf_max=" << sub_stream.rtp_out_buf_max << "\n";
    f << "rotate="          << sub_stream.rotate << "\n";
    f << "extra_copies="    << sub_stream.extra_copies << "\n";

    f << "[ai]\n";
    f << "; npu_core_num: NPU 核数实验键(0=不设置/AUTO 1/2/3 核)" << "\n";
    f << "model_path="    << ai.model_path << "\n";
    f << "frame_interval=" << ai.frame_interval << "\n";
    f << "box_thresh="    << ai.box_thresh << "\n";
    f << "nms_thresh="    << ai.nms_thresh << "\n";
    f << "npu_core_num="  << ai.npu_core_num << "\n\n";

    f << "[record]\n";
    f << "root_dir="       << record.root_dir << "\n";
    f << "slice_sec="      << record.slice_sec << "\n";
    f << "pre_buffer_sec=" << record.pre_buffer_sec << "\n";
    f << "post_record_sec=" << record.post_record_sec << "\n";
    f << "disk_watermark=" << record.disk_watermark << "\n";
    f << "# 音频录制（板载 ES8388 声卡，麦克风未接时自动降级/静音告警）\n";
    f << "audio_enable=" << (record.audio_enable ? 1 : 0) << "\n";
    f << "audio_device=" << record.audio_device << "\n";
    f << "audio_rate=" << record.audio_rate << "\n";
    f << "audio_channels=" << record.audio_channels << "\n";
    f << "audio_silence_warn=" << (record.audio_silence_warn ? 1 : 0) << "\n";
    f << "audio_bitrate=" << record.audio_bitrate << "\n";
    f << "audio_mono_source=" << record.audio_mono_source << "\n";
    f << "audio_kws_enable=" << (record.audio_kws_enable ? 1 : 0) << "\n\n";

    f << "[event]\n";
    f << "device_id="      << event.device_id << "\n";
    f << "image_dir="      << event.image_dir << "\n";
    f << "database_path="  << event.database_path << "\n\n";

    f << "[mqtt]\n";
    f << "broker_url="     << mqtt.broker_url << "\n";
    f << "client_id="      << mqtt.client_id << "\n";
    f << "topic="          << mqtt.topic << "\n";
    f << "; timeline_topic: 周期状态摘要上行 topic（{device_id} 自动替换为 [event] device_id）\n";
    f << "timeline_topic="  << mqtt.timeline_topic << "\n";
    f << "; cmd/resp/state: 远程操控协议 topic（cmd=下行命令 resp=上行应答 state=设备状态 retained）\n";
    f << "cmd_topic="      << mqtt.cmd_topic << "\n";
    f << "resp_topic="     << mqtt.resp_topic << "\n";
    f << "state_topic="    << mqtt.state_topic << "\n";
    f << "subscribe_enable=" << (mqtt.subscribe_enable ? 1 : 0) << "\n";
    f << "username="       << mqtt.username << "\n";
    f << "password="       << mqtt.password << "\n";
    f << "keepalive="      << mqtt.keepalive << "\n";
    f << "connect_timeout=" << mqtt.connect_timeout << "\n";
    f << "; publish_ack_timeout_ms: QoS1 PUBACK 等待上限；公网/大 payload（快照 base64 27~55KB）需放宽，过小会重发\n";
    f << "publish_ack_timeout_ms=" << mqtt.publish_ack_timeout_ms << "\n";
    f << "include_image_base64=" << mqtt.include_image_base64 << "\n\n";

    f << "[asr]\n";
    f << "asr_enable="       << (asr.enable ? 1 : 0) << "\n";
    f << "asr_model_dir="    << asr.model_dir << "\n";
    f << "asr_vocab_path="   << asr.vocab_path << "\n";
    f << "asr_keywords="     << asr.keywords << "\n";
    f << "asr_cooldown_sec=" << asr.cooldown_sec << "\n";
    f << "asr_silence_gate=" << (asr.silence_gate ? 1 : 0) << "\n";
    f << "asr_debug_text_log=" << (asr.debug_text_log ? 1 : 0) << "\n";
    f << "asr_text_log_file=" << asr.text_log_file << "\n";
    f << "asr_debug_wav_path=" << asr.debug_wav_path << "\n";
    f << "asr_segment_timeout_ms=" << asr.segment_timeout_ms << "\n";
    f << "; asr_npu_core_num: ASR NPU 核数实验键(0=不设置/AUTO)" << "\n";
    f << "asr_npu_core_num=" << asr.npu_core_num << "\n\n";

    f << "[chat]\n";
    f << "chat_enable="      << (chat.enable ? 1 : 0) << "\n";
    f << "llm_api_host="     << chat.llm_api_host << "\n";
    f << "llm_api_key="      << chat.llm_api_key << "\n";
    f << "llm_model="        << chat.llm_model << "\n";
    f << "tts_api_host="     << chat.tts_api_host << "\n";
    f << "tts_app_id="       << chat.tts_app_id << "\n";
    f << "tts_access_token=" << chat.tts_access_token << "\n";
    f << "tts_voice_type="   << chat.tts_voice_type << "\n";
    f << "tts_cluster="      << chat.tts_cluster << "\n";
    f << "; tts_provider: TTS 后端 cloud=火山引擎(默认) | piper=本地 NPU(全离线)" << "\n";
    f << "tts_provider="     << chat.tts_provider << "\n";
    f << "wake_word="        << chat.wake_word << "\n";
    f << "wake_timeout_sec="  << chat.wake_timeout_sec << "\n";
    f << "idle_timeout_sec="  << chat.idle_timeout_sec << "\n";
    f << "vision_words="      << chat.vision_words << "\n";
    f << "sos_words="        << chat.sos_words << "\n";
    f << "assistant_name="   << chat.assistant_name << "\n";
    f << "system_prompt="    << chat.system_prompt << "\n";
    f << "max_tokens="       << chat.max_tokens << "\n";
    f << "temperature="      << chat.temperature << "\n";
    f << "tts_speed_ratio="  << chat.tts_speed_ratio << "\n";
    f << "playback_device="  << chat.playback_device << "\n\n";

    f << "[local_llm]\n";
    f << "; 板端 VLM（RKLLM 部署 Qwen3-VL；0=回退云端 DeepSeek）\n";
    f << "vlm_enable="        << (chat.vlm.enabled ? 1 : 0) << "\n";
    f << "rkllm_model="       << chat.vlm.rkllm_model << "\n";
    f << "vision_model="      << chat.vlm.vision_model << "\n";
    f << "max_context_len="   << chat.vlm.max_context_len << "\n";
    f << "max_new_tokens="    << chat.vlm.max_new_tokens << "\n";
    f << "top_k="             << chat.vlm.top_k << "\n";
    f << "temperature="       << chat.vlm.temperature << "\n";
    f << "npu_core_num="      << chat.vlm.npu_core_num << "\n\n";

    f << "[piper_tts]\n";
    f << "; 本地 Piper TTS（RK3588 NPU decoder，全离线；tts_provider=piper 时启用）\n";
    f << "piper_encoder="     << chat.piper.encoder << "\n";
    f << "piper_decoder="     << chat.piper.decoder << "\n";
    f << "piper_config="      << chat.piper.config_json << "\n";
    f << "piper_espeak_data=" << chat.piper.espeak_data << "\n";
    f << "piper_npu_core="    << chat.piper.npu_core << "\n\n";

    f << "[vlm_pipeline]\n";
    f << "; VLM 视觉业务流水线：跌倒复核 / 周期巡检 / 远程操控（vlm_enable=1 时生效）\n";
    f << "; confirm_fallback: alarm=复核超时按未确认直报(宁报勿漏) | none=不报\n";
    f << "; ignore_report: VLM 判\"否\"时仍发 cleared 低打扰通知\n";
    f << "confirm_enable="    << (chat.vlm_pipeline.confirm_enable ? 1 : 0) << "\n";
    f << "confirm_timeout_ms=" << chat.vlm_pipeline.confirm_timeout_ms << "\n";
    f << "confirm_fallback="  << chat.vlm_pipeline.confirm_fallback << "\n";
    f << "ignore_report="     << (chat.vlm_pipeline.ignore_report ? 1 : 0) << "\n";
    f << "confirm_prompt="    << chat.vlm_pipeline.confirm_prompt << "\n";
    f << "; ---- 周期状态巡检（status_enable=1 时每 interval 秒在空闲+有人时抓帧问 VLM） ----\n";
    f << "status_enable="     << (chat.vlm_pipeline.status_enable ? 1 : 0) << "\n";
    f << "status_interval_sec=" << chat.vlm_pipeline.status_interval_sec << "\n";
    f << "status_min_idle_sec=" << chat.vlm_pipeline.status_min_idle_sec << "\n";
    f << "status_prompt="     << chat.vlm_pipeline.status_prompt << "\n";
    f << "; ---- 远程操控（remote_token 空=不校验；公网部署必须设置） ----\n";
    f << "remote_enable="     << (chat.vlm_pipeline.remote_enable ? 1 : 0) << "\n";
    f << "remote_token="      << chat.vlm_pipeline.remote_token << "\n";
    f << "remote_voice="      << (chat.vlm_pipeline.remote_voice ? 1 : 0) << "\n";
    f << "; remote_chat_prompt: 远程问答期间的临时人设（场景观察员，避免陪伴口吻）\n";
    f << "remote_chat_prompt=" << chat.vlm_pipeline.remote_chat_prompt << "\n";
    f << "; stream_rtmp_url: 远程实时视频推流目标（start_stream 命令起 ffmpeg 转推；空=关闭）\n";
    f << "stream_rtmp_url="   << chat.vlm_pipeline.stream_rtmp_url << "\n";
    f << "; stream_src_url: 推流源（板端本地 RTSP）。h264=子码流带AI框全平台可播；h265=主码流高清仅iPhone Safari可播\n";
    f << "stream_src_url="    << chat.vlm_pipeline.stream_src_url << "\n\n";
    f << "; sos_prompt: 求助事件视觉判定提示词（{text} 替换为求助者原话；结果:真|假|不确定）\n";
    f << "sos_prompt=" << chat.vlm_pipeline.sos_prompt << "\n";
    f << "sos_true_reply=" << chat.vlm_pipeline.sos_true_reply << "\n";
    f << "sos_false_reply=" << chat.vlm_pipeline.sos_false_reply << "\n";
    f << "sos_unknown_reply=" << chat.vlm_pipeline.sos_unknown_reply << "\n";
    f << "patrol_alert_words=" << chat.vlm_pipeline.patrol_alert_words << "\n";
    f << "fire_alert_reply=" << chat.vlm_pipeline.fire_alert_reply << "\n";
    f << "fall_console_fallback=" << chat.vlm_pipeline.fall_console_fallback << "\n";


    f.close();
    std::cout << "[Config] 默认配置文件已写入: " << path << std::endl;
    return true;
}