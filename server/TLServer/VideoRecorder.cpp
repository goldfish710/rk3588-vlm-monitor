#include "VideoRecorder.h"
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <dirent.h>
#include <cstring>
#include <algorithm>
#include <iostream>
#include <ctime>
#include <unistd.h>
#include <cerrno>
#include <sys/syscall.h>   // SYS_gettid
#include <sys/resource.h>  // setpriority / PRIO_PROCESS

VideoRecorder g_mainRecorder;

// ==================== 时间工具 ====================
int64_t VideoRecorder::currentTimeMs()
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return timevalToMs(tv);
}

int64_t VideoRecorder::timevalToMs(const timeval& tv)
{
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

std::string VideoRecorder::formatDirName(time_t t)
{
    struct tm tm_buf;
    localtime_r(&t, &tm_buf);
    char buf[16];
    strftime(buf, sizeof(buf), "%Y%m%d", &tm_buf);
    return std::string(buf);
}

std::string VideoRecorder::formatFileName(time_t t)
{
    struct tm tm_buf;
    localtime_r(&t, &tm_buf);
    char buf[16];
    strftime(buf, sizeof(buf), "%H%M%S", &tm_buf);
    return std::string(buf);
}

// ==================== 目录递归创建 ====================
bool VideoRecorder::createDirRecursive(const std::string& path)
{
    if (path.empty()) return true;
    struct stat st;
    if (stat(path.c_str(), &st) == 0) return S_ISDIR(st.st_mode);

    size_t pos = path.find_last_of('/');
    if (pos != std::string::npos) {
        if (!createDirRecursive(path.substr(0, pos))) return false;
    }
    if (mkdir(path.c_str(), 0755) == 0) return true;
    if (errno == EEXIST) {
        if (stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) return true;
    }
    return false;
}

// =====================================================
// 轻量级 IDR 检测（H.265 专用）
// 当 pkt.is_idr 为 false 时，检查 NAL header 判断是否 IRAP。
// 仅扫描前 6 个 NAL。
// =====================================================
static bool detectIdrFromData(const std::vector<uint8_t>& data)
{
    const uint8_t* p = data.data();
    size_t len = data.size();
    size_t i = 0;
    int nal_count = 0;

    while (i + 3 <= len && nal_count < 6) {
        int sc_len = 0;
        if (i + 4 <= len &&
            p[i] == 0x00 && p[i+1] == 0x00 && p[i+2] == 0x00 && p[i+3] == 0x01) {
            sc_len = 4;
        } else if (p[i] == 0x00 && p[i+1] == 0x00 && p[i+2] == 0x01) {
            sc_len = 3;
        }

        if (sc_len == 0) {
            i++;
            continue;
        }

        size_t hdr = i + sc_len;
        if (hdr >= len) break;

        // H.265: NAL type = (byte >> 1) & 0x3F, IRAP 16~21
        uint8_t nal_type = (p[hdr] >> 1) & 0x3F;
        if (nal_type >= 16 && nal_type <= 21) return true;

        bool found = false;
        for (size_t j = hdr; j + 3 <= len; j++) {
            if (j + 4 <= len &&
                p[j] == 0x00 && p[j+1] == 0x00 && p[j+2] == 0x00 && p[j+3] == 0x01) {
                i = j;
                found = true;
                break;
            }
            if (p[j] == 0x00 && p[j+1] == 0x00 && p[j+2] == 0x01) {
                i = j;
                found = true;
                break;
            }
        }
        if (!found) break;
        nal_count++;
    }
    return false;
}

// =====================================================
// 参数集注入（裸 NAL 存储，供 hvcC）
// MPP 返回的 VPS/SPS/PPS 可能带也可能不带起始码，统一剥掉
// =====================================================
static std::vector<uint8_t> stripStartCode(const std::vector<uint8_t>& in)
{
    if (in.size() >= 4 && in[0] == 0x00 && in[1] == 0x00 &&
        in[2] == 0x00 && in[3] == 0x01) {
        return std::vector<uint8_t>(in.begin() + 4, in.end());
    }
    if (in.size() >= 3 && in[0] == 0x00 && in[1] == 0x00 && in[2] == 0x01) {
        return std::vector<uint8_t>(in.begin() + 3, in.end());
    }
    return in;
}

void VideoRecorder::setH265Headers(const std::vector<uint8_t>& vps,
                                   const std::vector<uint8_t>& sps,
                                   const std::vector<uint8_t>& pps)
{
    vps_cache_ = stripStartCode(vps);
    sps_cache_ = stripStartCode(sps);
    pps_cache_ = stripStartCode(pps);
    std::cout << "[VideoRecorder] H265参数集缓存: VPS=" << vps_cache_.size()
              << "B SPS=" << sps_cache_.size() << "B PPS=" << pps_cache_.size()
              << "B (hvcC 注入)" << std::endl;
}

void VideoRecorder::setAacInfo(const std::vector<uint8_t>& asc, int sample_rate, int channels)
{
    aac_asc_ = asc;
    aac_sample_rate_ = sample_rate;
    aac_channels_ = channels;
    aac_info_ok_ = !asc.empty();
    std::cout << "[VideoRecorder] AAC 轨道信息: " << sample_rate << "Hz/"
              << channels << "ch ASC=" << aac_asc_.size() << "B" << std::endl;
}

// ==================== SD卡自检 ====================
bool VideoRecorder::checkStorage(std::string* err_out)
{
    struct statvfs vfs;
    if (statvfs(cfg_.root_dir.c_str(), &vfs) != 0) {
        if (err_out) *err_out = "statvfs失败: " + std::string(strerror(errno));
        return false;
    }

    uint64_t total = (uint64_t)vfs.f_blocks * vfs.f_frsize;
    uint64_t avail = (uint64_t)vfs.f_bavail * vfs.f_frsize;

    const size_t TEST_SIZE = 8 * 1024 * 1024;
    std::string test_file = cfg_.root_dir + "/.sd_selftest_tmp";

    std::vector<uint8_t> wbuf(TEST_SIZE);
    for (size_t k = 0; k < TEST_SIZE; k++) wbuf[k] = (uint8_t)(k * 31 + 7);

    int64_t t0 = currentTimeMs();
    FILE* fp = fopen(test_file.c_str(), "wb");
    if (!fp) {
        if (err_out) *err_out = "无法创建测试文件(只读或权限问题)";
        return false;
    }
    size_t wr = fwrite(wbuf.data(), 1, TEST_SIZE, fp);
    if (wr != TEST_SIZE) {
        fclose(fp);
        remove(test_file.c_str());
        if (err_out) *err_out = "写入测试不完整";
        return false;
    }
    fflush(fp);
    fsync(fileno(fp));
    int64_t t1 = currentTimeMs();
    fclose(fp);

    std::vector<uint8_t> rbuf(TEST_SIZE);
    FILE* rfp = fopen(test_file.c_str(), "rb");
    bool read_ok = false;
    if (rfp) {
        size_t rd = fread(rbuf.data(), 1, TEST_SIZE, rfp);
        fclose(rfp);
        read_ok = (rd == TEST_SIZE) && (memcmp(wbuf.data(), rbuf.data(), TEST_SIZE) == 0);
    }
    remove(test_file.c_str());
    if (!read_ok) {
        if (err_out) *err_out = "回读校验失败(卡损坏或数据线问题)";
        return false;
    }

    double write_mbps = (t1 > t0) ? (TEST_SIZE / 1024.0 / 1024.0) / ((t1 - t0) / 1000.0) : 0.0;
    if (write_mbps < 2.0) {
        char buf[128];
        snprintf(buf, sizeof(buf), "写入速度过低(%.1fMB/s, 要求≥2MB/s)", write_mbps);
        if (err_out) *err_out = buf;
        return false;
    }

    double bytes_per_sec = cfg_.record_bitrate_bps / 8.0;
    double days = (bytes_per_sec > 0) ? (avail / bytes_per_sec / 86400.0) : 0.0;

    std::cout << "[VideoRecorder] SD卡自检通过: 总"
              << (total / 1024.0 / 1024.0 / 1024.0) << "GB 可用"
              << (avail / 1024.0 / 1024.0 / 1024.0) << "GB 写速"
              << write_mbps << "MB/s 按当前码率可录约" << days << "天" << std::endl;
    return true;
}

// ==================== 初始化 ====================
bool VideoRecorder::init(const Config& cfg, std::string* err_out)
{
    if (initialized_) return true;

    cfg_ = cfg;
    continuous_dir_ = cfg_.root_dir + "/continuous";
    event_dir_      = cfg_.root_dir + "/events";

    if (!createDirRecursive(cfg_.root_dir)) {
        if (err_out) *err_out = "无法创建录像根目录";
        return false;
    }
    if (cfg_.enable_continuous && !createDirRecursive(continuous_dir_)) {
        if (err_out) *err_out = "无法创建连续录像目录";
        return false;
    }
    if (cfg_.enable_events && !createDirRecursive(event_dir_)) {
        if (err_out) *err_out = "无法创建事件目录";
        return false;
    }

    if (!checkStorage(err_out)) {
        std::cerr << "[VideoRecorder] SD卡自检失败: " << (err_out ? *err_out : "") << std::endl;
        return false;
    }

    running_ = true;
    worker_ = std::thread(&VideoRecorder::recordLoop, this);
    pthread_setname_np(worker_.native_handle(), "recorder");
    initialized_ = true;

    std::cout << "[VideoRecorder] 启动: MP4(fMP4)"
              << " 连续=" << (cfg_.enable_continuous ? "开" : "关")
              << " 事件=" << (cfg_.enable_events ? "开" : "关")
              << " 切片=" << cfg_.slice_sec << "s"
              << " 预录=" << cfg_.pre_buffer_sec << "s"
              << " 后录=" << cfg_.post_record_sec << "s" << std::endl;
    return true;
}

void VideoRecorder::shutdown()
{
    if (!initialized_) return;
    running_ = false;
    queue_cv_.notify_all();
    if (worker_.joinable()) worker_.join();
    initialized_ = false;
    std::cout << "[VideoRecorder] 已关闭" << std::endl;
}

// ==================== 编码线程旁路（非阻塞） ====================
void VideoRecorder::onVideoPacket(const H265EncodePacket& pkt)
{
    if (!initialized_) return;
    {
        std::lock_guard<std::mutex> lock(queue_mtx_);
        if (packet_queue_.size() < 50) {
            packet_queue_.push_back(pkt);
        }
    }
    queue_cv_.notify_one();
}

// ==================== AAC 帧投递（非阻塞入队） ====================
void VideoRecorder::onAacFrame(const AacFrame& frame)
{
    if (!initialized_) return;
    {
        std::lock_guard<std::mutex> lock(queue_mtx_);
        if (aac_queue_.size() >= 256) {
            aac_queue_.pop_front();   // 丢最旧（SD 长时间卡死时）
        }
        aac_queue_.push_back(frame);
    }
    queue_cv_.notify_one();
}

// ==================== 连续切片开新文件 ====================
void VideoRecorder::openNewSliceFile(int64_t ts_ms)
{
    time_t now = ts_ms / 1000;
    std::string day_dir = continuous_dir_ + "/" + formatDirName(now);
    createDirRecursive(day_dir);

    std::string file_path = day_dir + "/" + formatFileName(now) + ext_;

    Mp4Muxer::Config mc;
    mc.width = cfg_.width;
    mc.height = cfg_.height;
    mc.vps = vps_cache_;
    mc.sps = sps_cache_;
    mc.pps = pps_cache_;
    mc.audio_enable = aac_info_ok_;
    mc.asc = aac_asc_;
    mc.audio_timescale = (uint32_t)aac_sample_rate_;

    std::string err;
    if (!slice_mux_.open(file_path, mc, &err)) {
        std::cerr << "[VideoRecorder] 打开切片失败: " << err << std::endl;
        return;
    }
    slice_start_ms_ = ts_ms;
    cleanupOldFiles();
}

// ==================== 磁盘空间管理 ====================
void VideoRecorder::collectVideoFiles(const std::string& dir, const std::string& ext, std::vector<std::string>& out)
{
    DIR* dp = opendir(dir.c_str());
    if (!dp) return;

    struct dirent* entry;
    while ((entry = readdir(dp)) != NULL) {
        std::string name = entry->d_name;
        if (name == "." || name == "..") continue;

        std::string full = dir + "/" + name;
        struct stat st;
        if (stat(full.c_str(), &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                collectVideoFiles(full, ext, out);
            } else if (name.size() >= ext.size() && name.substr(name.size() - ext.size()) == ext) {
                out.push_back(full);
            }
        }
    }
    closedir(dp);
}

void VideoRecorder::cleanupOldFiles()
{
    struct statvfs vfs;
    if (statvfs(cfg_.root_dir.c_str(), &vfs) != 0 || vfs.f_blocks == 0) return;

    double used_ratio = 1.0 - (double)vfs.f_bavail / (double)vfs.f_blocks;
    if (used_ratio <= cfg_.disk_watermark) return;

    std::cout << "[VideoRecorder] 磁盘占用 " << (int)(used_ratio * 100)
              << "% 超过阈值，开始清理最旧录像" << std::endl;

    // 收集 .mp4 + 历史遗留 .h265/.wav（兼容旧版本录像）
    std::vector<std::string> files;
    collectVideoFiles(continuous_dir_, ".mp4", files);
    collectVideoFiles(continuous_dir_, ".h265", files);
    collectVideoFiles(continuous_dir_, ".wav", files);

    std::sort(files.begin(), files.end(), [](const std::string& a, const std::string& b) {
        struct stat sa, sb;
        if (stat(a.c_str(), &sa) != 0) return false;
        if (stat(b.c_str(), &sb) != 0) return true;
        return sa.st_mtime < sb.st_mtime;
    });

    for (const auto& f : files) {
        if (statvfs(cfg_.root_dir.c_str(), &vfs) != 0 || vfs.f_blocks == 0) break;
        double cur = 1.0 - (double)vfs.f_bavail / (double)vfs.f_blocks;
        if (cur <= cfg_.disk_watermark) break;

        if (remove(f.c_str()) == 0) {
            std::cout << "[VideoRecorder] 已删除旧录像: " << f << std::endl;
        }
    }
}

// ==================== 跌倒触发 ====================
std::string VideoRecorder::triggerFallEvent()
{
    if (!initialized_ || !cfg_.enable_events) return "";

    time_t now = time(NULL);
    std::string day_dir = event_dir_ + "/" + formatDirName(now);
    createDirRecursive(day_dir);

    static std::atomic<uint32_t> seq{0};
    uint32_t s = seq.fetch_add(1) % 1000;
    char base_buf[64];
    snprintf(base_buf, sizeof(base_buf), "%s_%03u_fall", formatFileName(now).c_str(), s);
    std::string event_path = day_dir + "/" + base_buf + ".mp4";

    // 只生成路径+投递命令：预录拷贝+mux+落盘由录像线程异步完成
    // （事件线程零阻塞，MQTT 报警延迟不受影响）
    EventMp4StartCmd cmd;
    cmd.day_dir = day_dir;
    cmd.base_name = base_buf;
    cmd.trigger_ms = currentTimeMs();
    {
        std::lock_guard<std::mutex> lock(queue_mtx_);
        event_cmd_queue_.push_back(cmd);
    }
    queue_cv_.notify_one();

    std::cout << "[VideoRecorder] 事件录制命令已投递: " << event_path << std::endl;
    return event_path;
}

// ==================== 录像线程主循环 ====================
void VideoRecorder::recordLoop()
{
    setpriority(PRIO_PROCESS, (pid_t)syscall(SYS_gettid), 10);

    while (running_) {
        H265EncodePacket pkt;
        bool has_video = false;
        {
            std::unique_lock<std::mutex> lock(queue_mtx_);
            queue_cv_.wait(lock, [this]() {
                return !packet_queue_.empty() || !aac_queue_.empty() ||
                       !event_cmd_queue_.empty() || !running_;
            });
            if (!running_ && packet_queue_.empty() && aac_queue_.empty() &&
                event_cmd_queue_.empty()) break;

            if (!packet_queue_.empty()) {
                pkt = std::move(packet_queue_.front());
                packet_queue_.pop_front();
                has_video = true;
            }

            // ---------- AAC 帧：预录环 + 切片 + 会话续写 ----------
            while (!aac_queue_.empty()) {
                AacFrame f = std::move(aac_queue_.front());
                aac_queue_.pop_front();
                processAacFrame(f);
            }
            // ---------- 事件命令：预录 mux + 落盘 ----------
            while (!event_cmd_queue_.empty()) {
                EventMp4StartCmd cmd = std::move(event_cmd_queue_.front());
                event_cmd_queue_.pop_front();
                processEventMp4Start(cmd);
            }
        }

        if (!has_video) continue;

        int64_t ts_ms = timevalToMs(pkt.timestamp);

        // IDR 检测（H.265）
        bool idr = pkt.is_idr;
        if (!idr) {
            idr = detectIdrFromData(pkt.data);
        }

        // ---------- 1. 视频预录环形缓冲（始终维护，用于事件） ----------
        if (cfg_.enable_events) {
            std::lock_guard<std::mutex> lock(ring_mtx_);
            RingEntry entry;
            entry.data = pkt.data;
            entry.ts_ms = ts_ms;
            entry.is_idr = idr;
            ring_buffer_.push_back(std::move(entry));

            int64_t cutoff = ts_ms - (int64_t)cfg_.pre_buffer_sec * 1000;
            while (!ring_buffer_.empty() && ring_buffer_.front().ts_ms < cutoff) {
                ring_buffer_.pop_front();
            }
        }

        // ---------- 2. 连续切片 MP4 ----------
        if (cfg_.enable_continuous) {
            if (slice_mux_.active() &&
                ts_ms - slice_start_ms_ >= (int64_t)cfg_.slice_sec * 1000) {
                slice_mux_.close();   // 轮转（fMP4 关闭成本极低，无待写索引）
            }
            if (!slice_mux_.active() && idr) {
                openNewSliceFile(ts_ms);
            }
            if (slice_mux_.active()) {
                slice_mux_.writeVideo(pkt.data.data(), pkt.data.size(), idr,
                                      (ts_ms - slice_start_ms_) * 90);   // ms→90kHz
            }
        }

        // ---------- 3. 事件会话续写 ----------
        if (cfg_.enable_events) {
            std::lock_guard<std::mutex> lock(sessions_mtx_);
            for (auto it = sessions_.begin(); it != sessions_.end(); ) {
                if (ts_ms > it->end_ms) {
                    it->mux->close();
                    std::cout << "[VideoRecorder] 事件录制完成: " << it->path << std::endl;
                    it = sessions_.erase(it);
                } else {
                    if (it->mux->active()) {
                        it->mux->writeVideo(pkt.data.data(), pkt.data.size(), idr,
                                            (ts_ms - it->start_ms) * 90);
                    }
                    ++it;
                }
            }
        }

        // ---------- 4. 周期性同步（每5秒，掉电保护） ----------
        int64_t now_sync = currentTimeMs();
        if (now_sync - last_sync_ms_ >= 5000) {
            if (slice_mux_.active()) slice_mux_.flush();
            {
                std::lock_guard<std::mutex> lock(sessions_mtx_);
                for (auto& s : sessions_) {
                    if (s.mux->active()) s.mux->flush();
                }
            }
            last_sync_ms_ = now_sync;
        }
    }

    // ---------- 线程退出前清理 ----------
    if (slice_mux_.active()) slice_mux_.close();
    {
        std::lock_guard<std::mutex> lock(sessions_mtx_);
        for (auto& s : sessions_) {
            if (s.mux->active()) s.mux->close();
        }
        sessions_.clear();
    }
    std::cout << "[VideoRecorder] 录像线程退出" << std::endl;
}

// ==================== AAC 帧处理（录像线程内） ====================
void VideoRecorder::processAacFrame(const AacFrame& f)
{
    // 1. AAC 预录环（事件用，按 pre_buffer_sec 时间窗裁剪）
    if (cfg_.enable_events) {
        aac_ring_.push_back(f);
        int64_t cutoff = f.ts_ms - (int64_t)cfg_.pre_buffer_sec * 1000;
        while (!aac_ring_.empty() && aac_ring_.front().ts_ms < cutoff) {
            aac_ring_.pop_front();
        }
    }

    // 2. 连续切片
    if (slice_mux_.active()) {
        slice_mux_.writeAudio(f.data.data(), f.data.size());
    }

    // 3. 事件会话续写
    {
        std::lock_guard<std::mutex> lock(sessions_mtx_);
        for (auto& s : sessions_) {
            if (s.mux->active() && f.ts_ms <= s.end_ms) {
                s.mux->writeAudio(f.data.data(), f.data.size());
            }
        }
    }
}

// ==================== 事件 MP4 启动（录像线程内） ====================
void VideoRecorder::processEventMp4Start(const EventMp4StartCmd& cmd)
{
    // 1. 拷贝视频预录段（从首个 IDR 帧起）
    std::vector<RingEntry> pre_video;
    int64_t trigger_ms = cmd.trigger_ms;
    {
        std::lock_guard<std::mutex> lock(ring_mtx_);
        if (!ring_buffer_.empty()) trigger_ms = ring_buffer_.back().ts_ms;

        size_t idr_idx = 0;
        for (size_t i = 0; i < ring_buffer_.size(); i++) {
            if (ring_buffer_[i].is_idr) { idr_idx = i; break; }
        }
        for (size_t i = idr_idx; i < ring_buffer_.size(); i++) {
            pre_video.push_back(ring_buffer_[i]);
        }
    }
    int64_t start_ms = pre_video.empty() ? trigger_ms : pre_video.front().ts_ms;

    // 2. 拷贝预录 AAC 帧（同一时间窗）
    std::vector<AacFrame> pre_audio;
    for (const auto& f : aac_ring_) {
        if (f.ts_ms >= start_ms && f.ts_ms <= trigger_ms) {
            pre_audio.push_back(f);
        }
    }

    // 3. 打开事件 MP4（预录段先 mux 即 t=0，无负 PTS 问题）
    std::string path = cmd.day_dir + "/" + cmd.base_name + ".mp4";
    std::unique_ptr<Mp4Muxer> mux(new Mp4Muxer());
    Mp4Muxer::Config mc;
    mc.width = cfg_.width;
    mc.height = cfg_.height;
    mc.vps = vps_cache_;
    mc.sps = sps_cache_;
    mc.pps = pps_cache_;
    mc.audio_enable = aac_info_ok_;
    mc.asc = aac_asc_;
    mc.audio_timescale = (uint32_t)aac_sample_rate_;

    std::string err;
    if (!mux->open(path, mc, &err)) {
        std::cerr << "[VideoRecorder] 事件 MP4 打开失败: " << err << std::endl;
        return;
    }

    // 4. 预录视频（PTS 相对 start_ms）
    for (const auto& e : pre_video) {
        mux->writeVideo(e.data.data(), e.data.size(), e.is_idr,
                        (e.ts_ms - start_ms) * 90);
    }
    // 5. 预录音频
    for (const auto& f : pre_audio) {
        mux->writeAudio(f.data.data(), f.data.size());
    }
    mux->flush();   // 预录段立即落盘（掉电保护，与旧裸流方案一致）

    // 6. 挂会话（实时续写直到 end_ms）
    EventSession session;
    session.mux = std::move(mux);
    session.start_ms = start_ms;
    session.path = path;
    session.end_ms = trigger_ms + (int64_t)cfg_.post_record_sec * 1000;
    {
        std::lock_guard<std::mutex> lock(sessions_mtx_);
        sessions_.push_back(std::move(session));
    }

    std::cout << "[VideoRecorder] 事件录制启动: " << path
              << "（预录视频" << pre_video.size() << "帧 音频" << pre_audio.size()
              << "帧，后续" << cfg_.post_record_sec << "s）" << std::endl;
}
