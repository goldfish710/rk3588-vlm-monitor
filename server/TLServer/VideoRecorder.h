#ifndef VIDEO_RECORDER_H
#define VIDEO_RECORDER_H

#include "config.h"
#include "AacEncoder.h"
#include "Mp4Muxer.h"
#include <string>
#include <vector>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdint>

// =====================================================
// 录像模块（单实例：主码流 H.265 + AAC → fMP4 分片 MP4）
//   功能1：循环连续录像（enable_continuous，slice_sec 一个 .mp4）
//   功能2：跌倒事件片段（enable_events，前N秒+后M秒，含预录音频）
// 音频：AAC 帧由 TLmain 的 AacEncoder 编码后经 onAacFrame() 投递，
//       mux 与视频在录像线程（唯一写线程）内完成。
// 掉电保护：fMP4 每样本一个 moof 分片 + moov 在文件头 + 5s 周期 fsync。
// =====================================================
class VideoRecorder
{
public:
    struct Config {
        std::string root_dir;             // 录像根目录（SD卡挂载点）
        bool enable_continuous = true;    // 循环连续录像
        bool enable_events = false;       // 事件片段（预录+后录）
        uint32_t record_bitrate_bps = 4000000;
        int pre_buffer_sec = 5;
        int post_record_sec = 5;
        int slice_sec = 300;              // 连续录像切片时长
        double disk_watermark = 0.85;     // 磁盘占用阈值
        int width = 2592;                 // 视频轨尺寸（MP4 元数据）
        int height = 1944;
    };

    bool init(const Config& cfg, std::string* err_out = nullptr);
    void shutdown();

    // 编码线程旁路（非阻塞）
    void onVideoPacket(const H265EncodePacket& pkt);

    // AAC 帧投递（编码线程→录像线程，非阻塞入队）
    void onAacFrame(const AacFrame& frame);

    // 触发跌倒事件片段（仅 enable_events=true 时有效），返回事件文件路径
    // 事件线程调用：只生成路径+投递命令，mux 由录像线程完成（零阻塞）
    std::string triggerFallEvent();

    // ============ 参数集注入（H.265 主码流使用） ============
    void setH265Headers(const std::vector<uint8_t>& vps,
                        const std::vector<uint8_t>& sps,
                        const std::vector<uint8_t>& pps);

    // ============ AAC 轨道元信息注入（TLmain 在 AacEncoder init 后调用） ============
    void setAacInfo(const std::vector<uint8_t>& asc, int sample_rate, int channels);

private:
    void recordLoop();

    // 工具函数
    static int64_t timevalToMs(const timeval& tv);
    static int64_t currentTimeMs();
    static std::string formatDirName(time_t t);
    static std::string formatFileName(time_t t);
    static bool createDirRecursive(const std::string& path);
    static void collectVideoFiles(const std::string& dir, const std::string& ext,
                                  std::vector<std::string>& out);
    void openNewSliceFile(int64_t ts_ms);
    void cleanupOldFiles();

    // SD卡自检
    bool checkStorage(std::string* err_out);

    // ==================== 事件 MP4 ====================
    struct EventMp4StartCmd {
        std::string day_dir;        // events/YYYYMMDD
        std::string base_name;      // HHMMSS_NNN_fall（不含扩展名）
        int64_t trigger_ms;         // 触发时刻（投递时打点）
    };
    void processAacFrame(const AacFrame& f);
    void processEventMp4Start(const EventMp4StartCmd& cmd);

    Config cfg_;
    std::string ext_ = ".mp4";
    std::string continuous_dir_;
    std::string event_dir_;

    // ==================== 参数集缓存（裸 NAL，hvcC 注入用） ====================
    std::vector<uint8_t> vps_cache_;
    std::vector<uint8_t> sps_cache_;
    std::vector<uint8_t> pps_cache_;

    // ==================== 预录环形缓冲 ====================
    struct RingEntry {
        std::vector<uint8_t> data;
        int64_t ts_ms;
        bool is_idr;
    };
    std::deque<RingEntry> ring_buffer_;          // 视频预录（录像线程写，事件命令处理读）
    std::mutex ring_mtx_;
    std::deque<AacFrame> aac_ring_;              // AAC 预录（仅录像线程访问，无需锁）

    // ==================== 事件会话 ====================
    struct EventSession {
        std::unique_ptr<Mp4Muxer> mux;
        int64_t start_ms = 0;                    // 视频预录起点（PTS 基准）
        int64_t end_ms = 0;
        std::string path;
    };
    std::vector<EventSession> sessions_;
    std::mutex sessions_mtx_;

    // ==================== 编码线程→录像线程队列 ====================
    std::deque<H265EncodePacket> packet_queue_;
    std::deque<AacFrame> aac_queue_;
    std::deque<EventMp4StartCmd> event_cmd_queue_;
    std::mutex queue_mtx_;
    std::condition_variable queue_cv_;

    // ==================== 连续切片状态（仅录像线程访问） ====================
    Mp4Muxer slice_mux_;
    int64_t slice_start_ms_ = 0;

    // ==================== AAC 轨道元信息 ====================
    std::vector<uint8_t> aac_asc_;
    int aac_sample_rate_ = 16000;
    int aac_channels_ = 1;
    std::atomic<bool> aac_info_ok_{false};

    // 周期同步
    int64_t last_sync_ms_ = 0;

    std::thread worker_;
    std::atomic<bool> running_{false};
    std::atomic<bool> initialized_{false};
};

// 唯一全局实例
extern VideoRecorder g_mainRecorder;

#endif
