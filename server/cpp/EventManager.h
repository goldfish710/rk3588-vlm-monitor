#ifndef EVENT_MANAGER_H
#define EVENT_MANAGER_H

#include <sqlite3.h>
#include <opencv2/opencv.hpp>
#include <string>
#include <vector>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <future>
#include <memory>

struct AIEvent {
    std::string type;
    std::string device_id;
    float confidence;
    time_t timestamp;
    std::string image_path;
    std::string video_path;
    int upload_status;
    std::string description;     // 场景描述/状态摘要（时间线消息用）
    int vlm_confirm = -1;        // -1=未复核 0=VLM 排除 1=真跌倒 2=无法确认
    std::string vlm_reply;       // VLM 结论原因/摘要原文
};

// 时间线行（周期巡检摘要/历史查询，chat 线程经 queryRecent 阻塞读取）
struct TimelineRow {
    time_t ts = 0;
    std::string event_type;
    std::string text;
    std::string image_path;
    int vlm_confirm = -1;
    std::string vlm_reply;   // 复核结论原因（fall/fire 等事件回填；timeline 行为空）
};

class EventManager {
public:
    static EventManager* getInstance();

    bool init(const std::string& db_path, const std::string& image_root);

    void recordEvent(const AIEvent& event, const cv::Mat& image);

    // 同步保存事件图片（写入 image_root 下按时间命名的文件）
    // 返回完整路径，失败返回空串。
    // 供事件线程使用：先拿路径填进 MQTT 消息，再调 recordEvent 异步落库
    std::string saveEventImageSync(const AIEvent& event, const std::vector<unsigned char>& jpeg_data);

    // 跌倒复核结论回填（fire-and-forget 入队，worker 线程执行；
    // 用 image_path 作键——文件名含全局自增序号天然唯一）
    void updateEventVerdict(const std::string& image_path, int verdict, const std::string& reply);

    // 周期巡检摘要落库（event_type='timeline'，阶段B 用）
    void recordTimeline(const std::string& text, const std::string& image_path, int person_count);

    // 查询最近 N 条记录（阻塞返回；SQLite 单连接，读也走 worker 串行化，
    // 仅 chat 线程调用，阻塞毫秒级）
    // type_equal: 空=全部；"timeline"=只要巡检摘要；"nontimeline"=只要事件(fall/fire/help等)
    // order_asc:  true=时间正序（最新的排最后）；false=最新在前
    bool queryRecent(int limit, const std::string &type_equal, bool order_asc,
                     std::vector<TimelineRow>& out);

    void shutdown();

    ~EventManager();

private:
    EventManager() = default;
    void workerThread();

    bool createTable();
    bool migrateTable();   // 幂等加列迁移（老库缺新列时 ALTER TABLE，不丢数据）
    bool insertEventRecord(const AIEvent& event);
    bool saveImage(const cv::Mat& image, const std::string& filename, std::string& out_path);
    std::string generateImageFilename(const AIEvent& event);

    sqlite3* db_ = nullptr;
    std::string image_root_;
    std::atomic<bool> running_{true};
    std::thread worker_;

    struct Task {
        int kind = 0;                 // 0=recordEvent 1=updateVerdict 2=recordTimeline 3=queryRecent
        AIEvent event;
        cv::Mat image;
        std::string image_path;       // updateVerdict 键
        int verdict = -1;
        std::string reply;            // updateVerdict 结论文本
        std::string text;             // recordTimeline 摘要文本
        int person_count = 0;         // recordTimeline 人数
        int limit = 0;                // queryRecent 条数
        std::string type_filter;      // queryRecent 类型过滤（空/timeline/nontimeline）
        int order_asc = 0;            // queryRecent 排序：1=正序(最新在后)
        std::shared_ptr<std::promise<std::vector<TimelineRow>>> prom;   // queryRecent 结果回传
    };
    std::queue<Task> task_queue_;
    std::mutex queue_mtx_;
    std::condition_variable cv_;

    bool initialized_ = false;
};

#endif