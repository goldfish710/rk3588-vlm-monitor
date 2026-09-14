#include "AudioCapture.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <pthread.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <unistd.h>

// ==================== 工具 ====================
static int64_t current_time_ms() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

// =====================================================
// 打开设备（回退链：配置设备 → "default"）
// =====================================================
bool AudioCapture::openDevice(std::string* err_out) {
    const char* candidates[3] = {cfg_.device.c_str(), "default", nullptr};

    // 配置的设备不是 "default" 时才把 default 作为回退，避免重复尝试
    int n = 1;
    if (cfg_.device != "default") candidates[n++] = "default";
    candidates[n] = nullptr;

    for (int i = 0; candidates[i]; i++) {
        int rc = snd_pcm_open(&pcm_, candidates[i], SND_PCM_STREAM_CAPTURE, 0);
        if (rc < 0) {
            printf("[AudioCapture] 打开 %s 失败: %s\n", candidates[i], snd_strerror(rc));
            continue;
        }

        // S16_LE 交错读写，2ch，16kHz，目标延迟 200ms（buffer≈200ms, period≈50ms）
        rc = snd_pcm_set_params(pcm_, SND_PCM_FORMAT_S16_LE,
                                SND_PCM_ACCESS_RW_INTERLEAVED,
                                (unsigned int)cfg_.channels, (unsigned int)cfg_.rate,
                                0, 200000);
        if (rc < 0) {
            printf("[AudioCapture] %s 不支持 %dHz/%dch/S16_LE: %s\n",
                   candidates[i], cfg_.rate, cfg_.channels, snd_strerror(rc));
            snd_pcm_close(pcm_);
            pcm_ = nullptr;
            continue;
        }

        snd_pcm_uframes_t buffer_size = 0, period_size = 0;
        snd_pcm_get_params(pcm_, &buffer_size, &period_size);
        printf("[AudioCapture] 设备=%s %dHz/%dch/S16_LE "
               "period=%lu帧(%.1fms) buffer=%lu帧(%.1fms)\n",
               candidates[i], cfg_.rate, cfg_.channels,
               period_size, period_size * 1000.0 / cfg_.rate,
               buffer_size, buffer_size * 1000.0 / cfg_.rate);

        // 显式启动流：否则 poll 在未启动的流上永远等不到事件
        rc = snd_pcm_start(pcm_);
        if (rc < 0 && rc != -EBADFD) {
            printf("[AudioCapture] snd_pcm_start 失败: %s（首次 readi 会自动启动）\n",
                   snd_strerror(rc));
        }

        rate_ = cfg_.rate;
        channels_ = cfg_.channels;
        return true;
    }

    if (err_out) *err_out = "所有候选采集设备打开失败（无麦克风或驱动不可用）";
    return false;
}

// =====================================================
// 初始化：打开设备 + 启动采集线程
// =====================================================
bool AudioCapture::init(const Config& cfg, std::string* err_out) {
    cfg_ = cfg;

    if (!openDevice(err_out)) {
        return false;
    }

    if (cfg_.debug_dump) {
        dump_fp_ = fopen("./audio_raw.pcm", "wb");
        printf("[AudioCapture] 调试 dump 开启: ./audio_raw.pcm\n");
    }

    running_ = true;
    thread_ = std::thread(&AudioCapture::captureLoop, this);
    return true;
}

// =====================================================
// 停止：置标志 + snd_pcm_drop 强制解除阻塞的 readi，
// 保证 join 有界（与 arecord 等标准工具同款模式）
// =====================================================
void AudioCapture::shutdown() {
    if (!running_.load()) return;
    running_ = false;
    if (pcm_) snd_pcm_drop(pcm_);   // 中断可能阻塞的 readi（未启动流返回 -EBADFD，忽略）
    if (thread_.joinable()) thread_.join();
}

void AudioCapture::pauseCapture() {
    // 只置标志,不碰 pcm 句柄:ALSA 句柄非线程安全,跨线程 drop 与采集线程
    // 阻塞中的 readi 并发会段错误(12.95s 长播放时实测崩溃于此)。
    // 采集线程在 readi 返回后(≤50ms)看到标志自行挂起,滞后可接受。
    paused_ = true;
}

void AudioCapture::resumeCapture() {
    paused_ = false;
    // 播放流的关闭会破坏 codec 时钟,必须完整重启设备(close+openDevice),
    // 且由采集线程自己执行——此处只置标志
    need_restart_ = true;
}

// =====================================================
// 采集线程：阻塞 readi（流已由 snd_pcm_start 启动，数据连续到达，
// 每次 readi ≤ period 时长即返回）→ 环形缓冲 + sink + 静音统计
// =====================================================
void AudioCapture::captureLoop() {
    pthread_setname_np(pthread_self(), "audio_cap");
    setpriority(PRIO_PROCESS, (pid_t)syscall(SYS_gettid), 10);   // 低优先级，不抢视频
    printf("[AudioCapture] 采集线程启动\n");

    snd_pcm_uframes_t buffer_size = 0, period_size = 0;
    snd_pcm_get_params(pcm_, &buffer_size, &period_size);
    const size_t bytes_per_frame = (size_t)channels_ * 2;

    std::vector<uint8_t> buf(period_size * bytes_per_frame);
    std::vector<uint8_t> chunk_data;
    chunk_data.reserve(period_size * bytes_per_frame);

    int xrun_count = 0;
    int consecutive_failures = 0;
    int64_t last_xrun_log_ms = 0;

    while (running_.load()) {
        // 暂停/重启请求在循环顶部处理(所有 pcm 操作仅本线程执行)
        if (paused_.load()) {
            // 播放暂停中:不读设备,静默轮询等待 resume
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        if (need_restart_.exchange(false)) {
            // 播放结束后的恢复请求:完整重启设备(实测 prepare 恢复的流
            // 会在 ~13s 后 -EIO,只有 close+重新 open 才能重置 codec 时钟)
            snd_pcm_close(pcm_);
            pcm_ = nullptr;
            if (openDevice(nullptr)) {
                consecutive_failures = 0;
                printf("[AudioCapture] 播放后采集设备重启完成\n");
                continue;
            }
            printf("[AudioCapture] 采集设备重启失败，采集线程退出\n");
            break;
        }

        snd_pcm_sframes_t n = snd_pcm_readi(pcm_, buf.data(), period_size);
        if (n < 0) {
            if (!running_.load()) break;   // 关停触发的 drop 让 readi 返回，直接退出
            if (paused_.load()) continue;  // readi 期间 pause 置位:进入暂停轮询
            if (need_restart_.exchange(false)) {
                snd_pcm_close(pcm_);
                pcm_ = nullptr;
                if (openDevice(nullptr)) {
                    consecutive_failures = 0;
                    printf("[AudioCapture] 播放后采集设备重启完成\n");
                    continue;
                }
                printf("[AudioCapture] 采集设备重启失败，采集线程退出\n");
                break;
            }
            if (n == -EPIPE) {
                // xrun：缓冲溢出/欠载，prepare 后继续（下次 readi 自动重启流）
                snd_pcm_prepare(pcm_);
                xrun_count++;
                int64_t now = current_time_ms();
                if (now - last_xrun_log_ms > 1000) {   // 限速打日志
                    printf("[AudioCapture] xrun(%d), prepare 恢复\n", xrun_count);
                    last_xrun_log_ms = now;
                }
            } else if (snd_pcm_recover(pcm_, (int)n, 1) < 0) {
                consecutive_failures++;
                printf("[AudioCapture] 读取失败: %s (连续%d次)\n",
                       snd_strerror((int)n), consecutive_failures);
                if (consecutive_failures >= 3) {
                    // 自愈：采集流被外部操作（如 TTS 播放重配时钟）损坏后，
                    // recover 无效，完整重启设备（实测 -EIO 循环每 ~10s 一次，
                    // 若不重启语音识别将永久失效）
                    printf("[AudioCapture] 连续失败%d次，重启采集设备...\n",
                           consecutive_failures);
                    snd_pcm_close(pcm_);
                    pcm_ = nullptr;
                    if (openDevice(nullptr)) {
                        consecutive_failures = 0;
                        printf("[AudioCapture] 采集设备重启成功\n");
                        continue;
                    }
                    printf("[AudioCapture] 设备重启失败，采集线程退出（视频不受影响）\n");
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
            continue;
        }
        consecutive_failures = 0;
        if (n == 0) continue;
        if (paused_.load()) continue;   // readi 期间 pause 置位:丢弃本块进入暂停

        size_t bytes = (size_t)n * bytes_per_frame;
        // 块起始时刻 = 读取完成时刻 - 块时长
        int64_t now_ms = current_time_ms();
        int64_t chunk_ts = now_ms - (int64_t)((uint64_t)n * 1000 / (unsigned)rate_);

        chunk_data.assign(buf.begin(), buf.begin() + bytes);

        // 0. 调试 dump：原始采集直写文件
        if (dump_fp_) {
            fwrite(chunk_data.data(), 1, bytes, dump_fp_);
        }

        // 1. 多路分发（每路一份拷贝；64KB/s，开销可忽略）
        for (auto& sink : sinks_) {
            AudioChunk chunk;
            chunk.ts_ms = chunk_ts;
            chunk.data = chunk_data;
            sink(std::move(chunk));
        }

        // 2. 静音/直流检测（标准差 < 阈值 → 无有效信号）
        updateSilenceStats(chunk_data);
    }

    snd_pcm_close(pcm_);
    pcm_ = nullptr;
    if (dump_fp_) {
        fflush(dump_fp_);
        fclose(dump_fp_);
        dump_fp_ = nullptr;
    }
    printf("[AudioCapture] 采集线程退出 (xrun=%d)\n", xrun_count);
}

// =====================================================
// 静音检测：每秒统计一次样本标准差（去直流）
// 纯静音和"麦克风未接的恒定直流"两种情况的 std 都≈0
// =====================================================
void AudioCapture::updateSilenceStats(const std::vector<uint8_t>& data) {
    if (!cfg_.silence_warn) return;

    const int16_t* samples = reinterpret_cast<const int16_t*>(data.data());
    size_t count = data.size() / 2;
    for (size_t i = 0; i < count; i++) {
        double s = samples[i];
        acc_sum_ += s;
        acc_sumsq_ += s * s;
        acc_samples_++;
    }

    int64_t now = current_time_ms();
    if (now - last_silence_check_ms_ < 1000) return;
    last_silence_check_ms_ = now;

    if (acc_samples_ < 1000) return;

    double mean = acc_sum_ / acc_samples_;
    double variance = acc_sumsq_ / acc_samples_ - mean * mean;
    double stddev = (variance > 0) ? sqrt(variance) : 0.0;
    silent_ = (stddev < 100.0);   // ≈ -50dBFS 以下视为无有效信号

    acc_sum_ = acc_sumsq_ = 0;
    acc_samples_ = 0;

    if (silent_.load() && now - last_silence_warn_ms_ > 60000) {
        printf("[AudioCapture] ★警告★ 采集信号标准差=%.1f（静音或直流），"
               "可能麦克风未接线或增益为 0，请检查硬件（amixer -c 1 contents，ES8388 声卡1；增益设置见 start.sh）\n", stddev);
        last_silence_warn_ms_ = now;
    }
}
