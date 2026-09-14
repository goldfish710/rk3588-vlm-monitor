// TLmain.cpp
// #include "const.h"
#include "v4l2.h"
#include "mpp.h"
#include "SharedQueue.h"
#include "H265LiveServerMediaSubsession.h"
#include "H264LiveServerMediaSubsession.h"
#include "EncodeThread.h"
#include "config.h"
#include "thread_util.h"

#include "yolov8-pose.h"
#include "image_utils.h"

#include <liveMedia.hh>
#include <BasicUsageEnvironment.hh>

#include "Logger.h"
#include "EventManager.h"
#include "mqtt_thread.h"
#include "mqtt_client.h"   // mqtt_set_command_handler（远程命令分发注册）
#include "AudioCapture.h"
#include "AacEncoder.h"
#include "AACLiveServerMediaSubsession.h"
#include "asr_thread.h"
#include "chat_engine.h"
#include <atomic>
#include <chrono>
#include <csignal>
#include <exception>
#include <cstring>
#include <ctime>
#include <errno.h>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <queue>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include <vector>
#include "VideoRecorder.h"

// volatile char watchVariable = 0;
// std::atomic<bool> exit_flag(false);

// ==============================
// 退出信号
// ==============================
void handle(int signum)
{
    (void)signum;
    watchVariable = 1;
    exit_flag = true;
}

// ==============================
// 主函数
// ==============================
// 未捕获异常(bad_alloc 等)→ terminate:打印线程名后 abort,方便长稳事故定位
// (2026-09-12 实测:ASR 线程连续运行 8h 后巨型分配失败,bad_alloc 无堆栈)
static void installTerminateHandler()
{
    std::set_terminate([]() {
        char name[16] = "?";
        pthread_getname_np(pthread_self(), name, sizeof(name));
        fprintf(stderr, "[FATAL] uncaught exception in thread [%s], aborting\n", name);
        abort();
    });
}

int main()
{
    installTerminateHandler();
    watchVariable = 0;
    exit_flag = false;

    // ========== 1. 加载配置文件 ==========
    const std::string config_path = "./config.ini";
    if (access(config_path.c_str(), F_OK) != 0) {
        // 配置文件不存在，写出默认配置
        g_config.saveDefault(config_path);
    }
    g_config.load(config_path);

    // ========== 2. 初始化日志系统 ==========
    Logger::getInstance()->init("./events/log/system.log", LOG_INFO);   //

    // ========== 3. 初始化 EventManager ==========
    if (!EventManager::getInstance()->init(g_config.event.database_path,
                                           g_config.event.image_dir)) {
        std::cerr << "EventManager 初始化失败，事件记录将不可用" << std::endl;
    } else {
        LOG_INFO("EventManager 初始化成功");
    }

    // ========== 4. 初始化录像模块（主码流：循环录像 + 事件切片） ==========
#if ENABLE_VIDEO_RECORDING
    {
        VideoRecorder::Config rec_cfg;
        rec_cfg.root_dir = g_config.record.root_dir;
        rec_cfg.enable_continuous = true;
        rec_cfg.enable_events = true;      // 事件录像也由主码流承担
        rec_cfg.record_bitrate_bps = (uint32_t)g_config.main_stream.bitrate;
        rec_cfg.slice_sec = g_config.record.slice_sec;
        rec_cfg.pre_buffer_sec = g_config.record.pre_buffer_sec;
        rec_cfg.post_record_sec = g_config.record.post_record_sec;
        rec_cfg.disk_watermark = g_config.record.disk_watermark;
        rec_cfg.width = g_config.main_stream.width;
        rec_cfg.height = g_config.main_stream.height;

        std::string err;
        if (!g_mainRecorder.init(rec_cfg, &err))
            std::cerr << "主码流录像不可用: " << err << std::endl;
    }
#else
    std::cout << "[DEBUG] 录像功能已关闭（ENABLE_VIDEO_RECORDING=0）" << std::endl;
#endif
    // ========== 5. 初始化 RKNN 模型 ==========
    init_post_process();
    rknn_app_context_t app_ctx;
    memset(&app_ctx, 0, sizeof(rknn_app_context_t));

    int ret = init_yolov8_pose_model(g_config.ai.model_path.c_str(), &app_ctx);
    if (ret != 0) {
        std::cerr << "init_yolov8_pose_model 失败, model_path="
                  << g_config.ai.model_path << std::endl;
        deinit_post_process();
        return -1;
    }
    std::cout << "YOLOv8-Pose 模型加载成功" << std::endl;

    // NPU 核数实验键（性能调优 A/B）：0=不设置（AUTO，旧行为）
    if (g_config.ai.npu_core_num > 0) {
        rknn_core_mask mask = RKNN_NPU_CORE_0;
        if (g_config.ai.npu_core_num == 2) mask = RKNN_NPU_CORE_0_1;
        else if (g_config.ai.npu_core_num == 3) mask = RKNN_NPU_CORE_0_1_2;
        int mret = rknn_set_core_mask(app_ctx.rknn_ctx, mask);
        std::cout << "[Perf] YOLO NPU 核数=" << g_config.ai.npu_core_num
                  << " (rknn_set_core_mask ret=" << mret << ")" << std::endl;
    }

    // 打印 RKNN 库与驱动版本（升级排查时快速确认用的是哪个 runtime）
    {
        rknn_sdk_version ver;
        memset(&ver, 0, sizeof(ver));
        int qret = rknn_query(app_ctx.rknn_ctx, RKNN_QUERY_SDK_VERSION, &ver, sizeof(ver));
        if (qret == RKNN_SUCC) {
            std::cout << "RKNN SDK: " << ver.api_version
                      << ", driver: " << ver.drv_version << std::endl;
        } else {
            std::cout << "RKNN SDK 版本查询失败 ret=" << qret << std::endl;
        }
    }

    // 打印模型输出张量属性（关键点解析依赖 type/zp/scale，排障必看）
    for (uint32_t i = 0; i < app_ctx.io_num.n_output; i++) {
        const rknn_tensor_attr &a = app_ctx.output_attrs[i];
        std::cout << "  输出[" << i << "] " << a.name << " type=" << a.type
                  << " zp=" << a.zp << " scale=" << a.scale
                  << " dims=[";
        for (uint32_t d = 0; d < a.n_dims; d++) {
            std::cout << (d ? "," : "") << a.dims[d];
        }
        std::cout << "]" << std::endl;
    }

    start_mqtt_thread();

    // ========== 6. 创建 RTSP 服务 ==========
    BasicTaskScheduler *scheduler = BasicTaskScheduler::createNew();
    BasicUsageEnvironment *env = BasicUsageEnvironment::createNew(*scheduler);

    RTSPServer *server = RTSPServer::createNew(*env, g_config.main_stream.rtsp_port);
    if (server == nullptr) {
        std::cerr << "RTSPServer 创建失败（端口被占用？）" << std::endl;
        release_yolov8_pose_model(&app_ctx);
        deinit_post_process();
        env->reclaim();
        delete scheduler;
        // 修复：错误路径必须停掉已启动的线程，否则 std::thread 析构 terminate
        stop_mqtt_thread();
#if ENABLE_VIDEO_RECORDING
        g_mainRecorder.shutdown();
#endif
        EventManager::getInstance()->shutdown();
        return -1;
    }

    // ========== 6.5 音频链路：ALSA 采集 → AAC 编码 → 录像 + RTSP（+KWS 预留） ==========
    AudioCapture audio_cap;
    AacEncoder aac_enc;
    AsrEngine *asr_engine = nullptr;
    ChatEngine *chat_engine = nullptr;
    SharedQueue *shared_queue_audio = new SharedQueue();
    bool audio_rtsp_ready = false;

    if (g_config.record.audio_enable) {
        AudioCapture::Config ac_cfg;
        ac_cfg.device = g_config.record.audio_device;
        ac_cfg.rate = g_config.record.audio_rate;
        ac_cfg.channels = g_config.record.audio_channels;
        ac_cfg.pre_buffer_sec = g_config.record.pre_buffer_sec;
        ac_cfg.silence_warn = g_config.record.audio_silence_warn;
        ac_cfg.debug_dump = g_config.record.audio_debug_dump;

        std::string aerr;
        if (!audio_cap.init(ac_cfg, &aerr)) {
            std::cerr << "音频采集不可用: " << aerr << "（录像与 RTSP 均无音轨，视频不受影响）" << std::endl;
        } else {
            // ---------- ASR 语音识别（zipformer 流式 → 呼救关键词 → MQTT help_call） ----------
            if (g_config.asr.enable) {
                AsrConfig acfg;
                acfg.model_dir = g_config.asr.model_dir;
                acfg.vocab_path = g_config.asr.vocab_path;
                // 唤醒词（可多个，逗号分隔）自动并入 ASR 关键词表——
                // 唤醒靠关键词匹配机制，用户只需在 [chat] wake_word 配置
                acfg.keywords = g_config.asr.keywords;
                for (const auto &w : splitCsv(g_config.chat.wake_word)) {
                    if (acfg.keywords.find(w) == std::string::npos) {
                        if (!acfg.keywords.empty()) acfg.keywords += ",";
                        acfg.keywords += w;
                    }
                }
                acfg.cooldown_sec = g_config.asr.cooldown_sec;
                acfg.silence_gate = g_config.asr.silence_gate;
                acfg.debug_text_log = g_config.asr.debug_text_log;
                acfg.text_log_file = g_config.asr.text_log_file;
                acfg.debug_wav_path = g_config.asr.debug_wav_path;
                acfg.segment_timeout_ms = g_config.asr.segment_timeout_ms;
                acfg.capture_rate = g_config.record.audio_rate;
                acfg.capture_channels = g_config.record.audio_channels;
                acfg.mono_source = g_config.record.audio_mono_source;

                asr_engine = new AsrEngine();
                std::string aerr2;
                if (asr_engine->init(acfg, [&audio_cap]() { return audio_cap.silent(); }, &aerr2)) {
                    // 命中关键词 → MQTT（线程安全，同跌倒事件范式）
                    // 唤醒词只负责唤醒对话，不作为呼救事件上报
                    asr_engine->addEventCallback([](const std::string &kw, float conf) {
                        for (const auto &w : splitCsv(g_config.chat.wake_word))
                            if (kw == w) return;
                        MQTTEvent ev;
                        ev.event = "help_call";
                        ev.device_id = g_config.event.device_id;
                        ev.confidence = conf;
                        ev.timestamp = time(nullptr);
                        mqtt_push_event(ev);
                    });
                    // 采集线程 sink：仅降混+入队，重活在 ASR 线程
                    audio_cap.addSink([asr_engine](AudioChunk &&c) { asr_engine->feedPcm(c); });
                    asr_engine->start();
                    std::cout << "[ASR] 语音识别已启动" << std::endl;

                    // ---------- 语音对话（唤醒词 → DeepSeek LLM → 火山 TTS → 播放） ----------
                    if (g_config.chat.enable) {
                        chat_engine = new ChatEngine();
                        std::string cerr;
                        if (chat_engine->init(g_config.chat, &cerr)) {
                            // 唤醒词命中 → 进入对话模式
                            asr_engine->addEventCallback([chat_engine](const std::string &kw, float conf) {
                                chat_engine->onKeyword(kw, conf);
                            });
                            // 说话段落 → 对话查询（ASR 线程回调，仅入队；
                            // 第二参=话音结束墙钟，[Voice] 全链路打点用）
                            asr_engine->setSegmentCallback(
                                [chat_engine](const std::string &seg, int64_t speech_end_ms) {
                                    chat_engine->onSegment(seg, speech_end_ms);
                                });
                            // 播放状态 → ASR 回声门控 + 采集暂停
                            // （播放与采集并发会 xrun 损坏采集流,详见 AudioCapture 注释）
                            chat_engine->setPlayStateCallback(
                                [asr_engine, &audio_cap](bool playing) {
                                    asr_engine->setSuppressInput(playing);
                                    if (playing) audio_cap.pauseCapture();
                                    else audio_cap.resumeCapture();
                                });
                            chat_engine->start();
                            g_chat_engine = chat_engine;   // 供 EncodeThread 跌倒事件跨模块调用
                            // MQTT 下行命令 → chat 线程（回调线程只入队，见 mqtt_client 注释）
                            mqtt_set_command_handler([](const char *payload, int len) {
                                if (g_chat_engine)
                                    g_chat_engine->onRemoteCommand(std::string(payload, (size_t)len));
                            });
                            std::cout << "[Chat] 语音对话已启动（远程命令已接入）" << std::endl;
                        } else {
                            std::cerr << "[Chat] 语音对话不可用: " << cerr << std::endl;
                            delete chat_engine;
                            chat_engine = nullptr;
                        }
                    }
                } else {
                    std::cerr << "[ASR] 语音识别不可用: " << aerr2 << std::endl;
                    delete asr_engine;
                    asr_engine = nullptr;
                }
            }
            AacEncoder::Config ec;
            ec.sample_rate = g_config.record.audio_rate;
            ec.channels = g_config.record.audio_channels;
            ec.bitrate_bps = g_config.record.audio_bitrate;
            ec.mono_source = g_config.record.audio_mono_source;

            if (!aac_enc.init(ec, &aerr)) {
                std::cerr << "AAC 编码器不可用: " << aerr << std::endl;
            } else {
                // PCM → AAC（采集线程内联编码）
                audio_cap.addSink([&aac_enc](AudioChunk&& c) { aac_enc.feedPcm(c); });

                // AAC → 录像（MP4 音轨）
                aac_enc.addAacSink([](const AacFrame& f) { g_mainRecorder.onAacFrame(f); });
                g_mainRecorder.setAacInfo(aac_enc.asc(), aac_enc.outSampleRate(), aac_enc.outChannels());

                // AAC → RTSP 音频轨（仅主码流）
                if (g_config.main_stream.rtsp_audio_enable) {
                    aac_enc.addAacSink([shared_queue_audio, scheduler](const AacFrame& f) {
                        H265EncodePacket p;
                        p.data = f.data;
                        p.timestamp.tv_sec = f.ts_ms / 1000;
                        p.timestamp.tv_usec = (f.ts_ms % 1000) * 1000;
                        shared_queue_audio->push_packet(p);
                        if (shared_queue_audio->GetMppEventID() != 0 &&
                            shared_queue_audio->GetSource() != NULL) {
                            scheduler->triggerEvent(shared_queue_audio->GetMppEventID(),
                                                    shared_queue_audio->GetSource());
                        }
                    });
                    audio_rtsp_ready = true;
                }
            }
        }
    }

    // ========== 7. 主码流 H.265 session（+AAC 音频轨） ==========
    SharedQueue *shared_queue_main = new SharedQueue();
    ServerMediaSession *session_main =
        ServerMediaSession::createNew(*env, g_config.main_stream.rtsp_path.c_str());
    H265LiveServerMediaSubsession *subsession_main =
        H265LiveServerMediaSubsession::createNew(shared_queue_main, *env, True);
    session_main->addSubsession(subsession_main);
    if (audio_rtsp_ready) {
        session_main->addSubsession(AACLiveServerMediaSubsession::createNew(
            shared_queue_audio, *env, True, aac_enc.ascHex(),
            aac_enc.outSampleRate(), aac_enc.outChannels()));
    }
    server->addServerMediaSession(session_main);

    // ========== 8. 子码流 H.264 session（仅AI+RTSP） ==========
    SharedQueue *shared_queue_sub = new SharedQueue();
    ServerMediaSession *session_sub =
        ServerMediaSession::createNew(*env, g_config.sub_stream.rtsp_path.c_str());
    H264LiveServerMediaSubsession *subsession_sub =
        H264LiveServerMediaSubsession::createNew(shared_queue_sub, *env, True);
    session_sub->addSubsession(subsession_sub);
    server->addServerMediaSession(session_sub);

    // ========== 9. 启动编码线程 ==========
    std::thread encode_h265;
    if (g_config.main_stream.enable)
        encode_h265 = std::thread(start_encode_h265, shared_queue_main, scheduler);
    std::thread encode_h264(start_encode_h264, shared_queue_sub, scheduler, &app_ctx);
    std::cout << (g_config.main_stream.enable
                      ? "双编码线程启动（子码流仅AI推理，不录像）"
                      : "主码流已禁用（enable=0），仅子码流运行") << std::endl;
    // ========== 9.1 编码线程健康监控（异常则重启进程） ==========
    std::thread monitor_thread([&]() {
        // 给线程初始化留出时间，避免误判
        std::this_thread::sleep_for(std::chrono::seconds(3));
        while (!exit_flag) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            if (exit_flag) break;
            if ((g_config.main_stream.enable && !g_h265_thread_alive) || !g_h264_thread_alive) {
                fprintf(stderr, "[FATAL] 编码线程异常退出，2秒后触发进程重启\n");
                std::this_thread::sleep_for(std::chrono::seconds(2));
                abort();   // 由 systemd / init 脚本守护自动重启
            }
        }
    });
    // ========== 10. 注册信号 ==========
    signal(SIGINT, handle);
    signal(SIGTERM, handle);
    signal(SIGHUP, handle);

    std::cout << "RTSP Server 启动" << std::endl;
    std::cout << "主码流: rtsp://<board-ip>:" << g_config.main_stream.rtsp_port
              << "/" << g_config.main_stream.rtsp_path
              << (audio_rtsp_ready ? " (H.265+AAC音频)" : "") << std::endl;
    std::cout << "子码流: rtsp://<board-ip>:" << g_config.main_stream.rtsp_port
              << "/" << g_config.sub_stream.rtsp_path << " (带AI检测框)" << std::endl;

    // ========== 11. 进入事件循环 ==========
    // 所有 init 完成后绑定 main 线程到 A76 大核（此前绑定会让 rkllm 内部线程继承 affinity）
    bind_rt_thread();
    env->taskScheduler().doEventLoop(&watchVariable);

    std::cout << "收到退出信号，开始清理资源..." << std::endl;

    exit_flag = true;

    // ========== 12. 等待编码线程结束 ==========
    if (encode_h265.joinable()) encode_h265.join();
    if (encode_h264.joinable()) encode_h264.join();
    if (monitor_thread.joinable()) monitor_thread.join(); 
    std::cout << "编码线程已退出" << std::endl;

    // ========== 13. 释放 RTSP 资源 ==========
    // 注意顺序：音频采集线程的 RTSP sink lambda 捕获了 scheduler 指针，
    // 必须先停音频生产者（采集线程退出后不再触发事件），否则
    // delete scheduler 后采集线程触发事件 = use-after-free（曾致退出 segfault）
    audio_cap.shutdown();
    aac_enc.shutdown();
    // 对话线程的播放门控回调访问 ASR，须在 ASR 停止前结束
    if (chat_engine) {
        g_chat_engine = nullptr;   // 先摘全局指针，防其他线程再回调
        chat_engine->stop();
        delete chat_engine;
        chat_engine = nullptr;
    }
    // ASR 的 feedPcm 由采集线程调用，须活到采集停止之后
    if (asr_engine) {
        asr_engine->stop();
        delete asr_engine;
        asr_engine = nullptr;
    }

    Medium::close(server);
    env->reclaim();
    delete scheduler;

    delete shared_queue_main;
    delete shared_queue_sub;
    delete shared_queue_audio;

    // ========== 14. 释放模型资源 ==========
    release_yolov8_pose_model(&app_ctx);
    deinit_post_process();

    stop_mqtt_thread();
#if ENABLE_VIDEO_RECORDING
    g_mainRecorder.shutdown();
#endif
    std::cout << "所有资源释放完成，程序退出" << std::endl;

    extern void close_ai_log();
    close_ai_log();
    EventManager::getInstance()->shutdown();
    return 0;
}