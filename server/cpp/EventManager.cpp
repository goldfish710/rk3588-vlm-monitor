#include "EventManager.h"
#include "Logger.h"
#include <sys/stat.h>
#include <errno.h>
#include <cstring>
#include <ctime>
#include <sstream>
#include <iomanip>
#include <set>
#include <unistd.h>

// 递归创建目录（C++11兼容）
static bool createDirectoryRecursive(const std::string& path) {
    if (path.empty()) return true;
    struct stat st;
    if (stat(path.c_str(), &st) == 0) {
        return S_ISDIR(st.st_mode);
    }
    size_t pos = path.find_last_of('/');
    if (pos != std::string::npos) {
        std::string parent = path.substr(0, pos);
        if (!createDirectoryRecursive(parent)) return false;
    }
    if (mkdir(path.c_str(), 0755) == 0) return true;
    if (errno == EEXIST) {
        if (stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) return true;
    }
    return false;
}

EventManager* EventManager::getInstance() {
    static EventManager instance;
    return &instance;
}

EventManager::~EventManager() {
    shutdown();
}

bool EventManager::init(const std::string& db_path, const std::string& image_root) {
    if (initialized_) return true;
    image_root_ = image_root;

    std::string db_dir = db_path.substr(0, db_path.find_last_of('/'));
    if (!createDirectoryRecursive(db_dir)) {
        LOG_ERROR("创建数据库目录失败: %s", db_dir.c_str());
        return false;
    }
    if (!createDirectoryRecursive(image_root_)) {
        LOG_ERROR("创建图片目录失败: %s", image_root_.c_str());
        return false;
    }

    if (sqlite3_open(db_path.c_str(), &db_) != SQLITE_OK) {
        LOG_ERROR("无法打开数据库 %s: %s", db_path.c_str(), sqlite3_errmsg(db_));
        return false;
    }
    if (!createTable()) {
        LOG_ERROR("创建表失败");
        sqlite3_close(db_);
        db_ = nullptr;
        return false;
    }
    if (!migrateTable()) {
        LOG_ERROR("表迁移失败");
        sqlite3_close(db_);
        db_ = nullptr;
        return false;
    }

    running_ = true;
    worker_ = std::thread(&EventManager::workerThread, this);
    initialized_ = true;
    LOG_INFO("EventManager 初始化成功");
    return true;
}

void EventManager::shutdown() {
    if (!initialized_) return;
    running_ = false;
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
    initialized_ = false;
    LOG_INFO("EventManager 已关闭");
}

void EventManager::recordEvent(const AIEvent& event, const cv::Mat& image) {
    if (!initialized_) {
        LOG_WARN("EventManager 未初始化，忽略事件");
        return;
    }
    Task task;
    task.kind = 0;
    task.event = event;
    if (!image.empty()) {
        image.copyTo(task.image);
    }
    {
        std::lock_guard<std::mutex> lock(queue_mtx_);
        if (task_queue_.size() < 100) {
            task_queue_.push(std::move(task));
        } else {
            LOG_WARN("事件队列满，丢弃旧事件");
            task_queue_.pop();
            task_queue_.push(std::move(task));
        }
    }
    cv_.notify_one();
}

// 复核结论回填：fire-and-forget（仅回填未复核行，防旧结论覆盖新结论）
void EventManager::updateEventVerdict(const std::string& image_path, int verdict, const std::string& reply) {
    if (!initialized_ || image_path.empty()) return;
    Task task;
    task.kind = 1;
    task.image_path = image_path;
    task.verdict = verdict;
    task.reply = reply;
    {
        std::lock_guard<std::mutex> lock(queue_mtx_);
        task_queue_.push(std::move(task));
    }
    cv_.notify_one();
}

// 周期巡检摘要落库（event_type='timeline'，confidence 复用存人数）
void EventManager::recordTimeline(const std::string& text, const std::string& image_path, int person_count) {
    if (!initialized_) return;
    Task task;
    task.kind = 2;
    task.text = text;
    task.image_path = image_path;
    task.person_count = person_count;
    {
        std::lock_guard<std::mutex> lock(queue_mtx_);
        task_queue_.push(std::move(task));
    }
    cv_.notify_one();
}

// 最近 N 条记录查询：promise 阻塞返回（仅 chat 线程调用；读也走 worker 与写串行）
bool EventManager::queryRecent(int limit, const std::string &type_equal, bool order_asc,
                               std::vector<TimelineRow>& out) {
    if (!initialized_ || limit <= 0) return false;
    auto prom = std::make_shared<std::promise<std::vector<TimelineRow>>>();
    std::future<std::vector<TimelineRow>> fut = prom->get_future();
    {
        std::lock_guard<std::mutex> lock(queue_mtx_);
        Task task;
        task.kind = 3;
        task.limit = limit;
        task.type_filter = type_equal;
        task.order_asc = order_asc ? 1 : 0;
        task.prom = prom;
        task_queue_.push(std::move(task));
    }
    cv_.notify_one();
    out = fut.get();
    return true;
}

void EventManager::workerThread() {
    while (running_) {
        Task task;
        {
            std::unique_lock<std::mutex> lock(queue_mtx_);
            cv_.wait(lock, [this]() { return !task_queue_.empty() || !running_; });
            if (!running_ && task_queue_.empty()) break;
            task = std::move(task_queue_.front());
            task_queue_.pop();
        }

        switch (task.kind) {
        case 1: {   // updateVerdict
            const char* sql = "UPDATE events SET vlm_confirm=?, vlm_reply=? "
                              "WHERE image_path=? AND vlm_confirm=-1";
            sqlite3_stmt* stmt = nullptr;
            if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
                LOG_ERROR("准备UPDATE失败: %s", sqlite3_errmsg(db_));
                break;
            }
            sqlite3_bind_int(stmt, 1, task.verdict);
            sqlite3_bind_text(stmt, 2, task.reply.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 3, task.image_path.c_str(), -1, SQLITE_STATIC);
            if (sqlite3_step(stmt) != SQLITE_DONE)
                LOG_ERROR("复核结论回填失败: %s", sqlite3_errmsg(db_));
            else
                LOG_INFO("复核结论回填: %s → verdict=%d", task.image_path.c_str(), task.verdict);
            sqlite3_finalize(stmt);
            break;
        }
        case 2: {   // recordTimeline
            AIEvent ev;
            ev.type = "timeline";
            ev.device_id = "";
            ev.confidence = (float)task.person_count;
            ev.timestamp = time(nullptr);
            ev.image_path = task.image_path;
            ev.video_path = "";
            ev.upload_status = 0;
            ev.description = task.text;
            ev.vlm_confirm = -1;
            if (insertEventRecord(ev))
                LOG_INFO("时间线记录: %s", task.text.c_str());
            else
                LOG_ERROR("时间线插入失败");
            break;
        }
        case 3: {   // queryRecent
            std::vector<TimelineRow> rows;
            // 动态拼 SQL：类型过滤（空=全部 / timeline / nontimeline=事件）。
            // 排序必须先"取最新 N 条"(DESC LIMIT)再按需正序——直接 ORDER BY ASC LIMIT
            // 会从最旧开始取（2026-09-13 踩过：时间线拉出 9-09 的远古数据）。
            std::string sql = "SELECT * FROM (SELECT timestamp, event_type, description, "
                              "image_path, vlm_confirm, vlm_reply FROM events";
            bool bind_type = false;
            if (task.type_filter == "nontimeline") {
                sql += " WHERE event_type != 'timeline'";
            } else if (!task.type_filter.empty()) {
                sql += " WHERE event_type = ?";
                bind_type = true;
            }
            sql += " ORDER BY id DESC LIMIT ?)";
            if (task.order_asc)
                sql += " ORDER BY timestamp ASC";   // 子查询取到最新 N 条后再正序
            sqlite3_stmt* stmt = nullptr;
            if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
                int idx = 1;
                if (bind_type)
                    sqlite3_bind_text(stmt, idx++, task.type_filter.c_str(), -1, SQLITE_STATIC);
                sqlite3_bind_int(stmt, idx, task.limit);
                while (sqlite3_step(stmt) == SQLITE_ROW) {
                    TimelineRow row;
                    row.ts = (time_t)sqlite3_column_int64(stmt, 0);
                    const unsigned char* t1 = sqlite3_column_text(stmt, 1);
                    const unsigned char* t2 = sqlite3_column_text(stmt, 2);
                    const unsigned char* t3 = sqlite3_column_text(stmt, 3);
                    const unsigned char* t4 = sqlite3_column_text(stmt, 5);
                    if (t1) row.event_type = (const char*)t1;
                    if (t2) row.text = (const char*)t2;
                    if (t3) row.image_path = (const char*)t3;
                    if (t4) row.vlm_reply = (const char*)t4;
                    row.vlm_confirm = sqlite3_column_int(stmt, 4);
                    rows.push_back(row);
                }
                sqlite3_finalize(stmt);
            } else {
                LOG_ERROR("查询历史失败: %s", sqlite3_errmsg(db_));
            }
            if (task.prom) task.prom->set_value(rows);
            break;
        }
        default: {   // recordEvent（原逻辑）
            if (!task.image.empty()) {
                std::string filename = generateImageFilename(task.event);
                std::string img_path;
                if (saveImage(task.image, filename, img_path)) {
                    task.event.image_path = img_path;
                    LOG_DEBUG("图片保存: %s", img_path.c_str());
                } else {
                    LOG_ERROR("保存图片失败");
                }
            }

            if (insertEventRecord(task.event)) {
                LOG_INFO("事件记录成功: type=%s, confidence=%.2f", task.event.type.c_str(), task.event.confidence);
            } else {
                LOG_ERROR("插入事件失败");
            }
            break;
        }
        }
    }
}

bool EventManager::createTable() {
    const char* sql = "CREATE TABLE IF NOT EXISTS events ("
                      "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                      "event_type TEXT,"
                      "device_id TEXT,"
                      "confidence REAL,"
                      "timestamp INTEGER,"
                      "image_path TEXT,"
                      "video_path TEXT,"
                      "upload_status INTEGER DEFAULT 0,"
                      "description TEXT,"
                      "vlm_confirm INTEGER DEFAULT -1,"
                      "vlm_reply TEXT"
                      ");";
    char* errmsg = nullptr;
    int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &errmsg);
    if (rc != SQLITE_OK) {
        LOG_ERROR("创建表失败: %s", errmsg);
        sqlite3_free(errmsg);
        return false;
    }
    return true;
}

// 幂等加列迁移：老库（无 description/vlm_confirm/vlm_reply 列）自动补列，历史数据不丢
bool EventManager::migrateTable() {
    struct { const char* name; const char* ddl; } cols[] = {
        {"description", "ALTER TABLE events ADD COLUMN description TEXT"},
        {"vlm_confirm", "ALTER TABLE events ADD COLUMN vlm_confirm INTEGER DEFAULT -1"},
        {"vlm_reply",   "ALTER TABLE events ADD COLUMN vlm_reply TEXT"},
    };
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "PRAGMA table_info(events)", -1, &stmt, nullptr) != SQLITE_OK)
        return false;
    std::set<std::string> existing;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char* name = sqlite3_column_text(stmt, 1);
        if (name) existing.insert((const char*)name);
    }
    sqlite3_finalize(stmt);
    for (const auto& c : cols) {
        if (existing.count(c.name)) continue;
        char* errmsg = nullptr;
        if (sqlite3_exec(db_, c.ddl, nullptr, nullptr, &errmsg) != SQLITE_OK) {
            LOG_ERROR("加列失败 %s: %s", c.name, errmsg ? errmsg : "");
            if (errmsg) sqlite3_free(errmsg);
            return false;
        }
        LOG_INFO("events 表迁移: 新增列 %s", c.name);
    }
    return true;
}

bool EventManager::insertEventRecord(const AIEvent& event) {
    const char* sql = "INSERT INTO events (event_type, device_id, confidence, timestamp, image_path, video_path, upload_status, description, vlm_confirm, vlm_reply) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        LOG_ERROR("准备SQL语句失败: %s", sqlite3_errmsg(db_));
        return false;
    }
    sqlite3_bind_text(stmt, 1, event.type.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, event.device_id.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_double(stmt, 3, event.confidence);
    sqlite3_bind_int64(stmt, 4, event.timestamp);
    sqlite3_bind_text(stmt, 5, event.image_path.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 6, event.video_path.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 7, event.upload_status);
    sqlite3_bind_text(stmt, 8, event.description.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 9, event.vlm_confirm);
    sqlite3_bind_text(stmt, 10, event.vlm_reply.c_str(), -1, SQLITE_STATIC);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool EventManager::saveImage(const cv::Mat& image, const std::string& filename, std::string& out_path) {
    std::string full_path = image_root_ + "/" + filename;
    std::vector<int> params = {cv::IMWRITE_JPEG_QUALITY, 85};
    if (cv::imwrite(full_path, image, params)) {
        out_path = full_path;
        return true;
    }
    return false;
}

std::string EventManager::saveEventImageSync(const AIEvent& event, const std::vector<unsigned char>& jpeg_data) {
    if (!initialized_ || jpeg_data.empty()) return "";
    std::string filename = generateImageFilename(event);
    std::string full_path = image_root_ + "/" + filename;
    FILE* fp = fopen(full_path.c_str(), "wb");
    if (!fp) {
        LOG_ERROR("打开图片文件失败: %s", full_path.c_str());
        return "";
    }
    if (fwrite(jpeg_data.data(), 1, jpeg_data.size(), fp) != jpeg_data.size()) {
        LOG_ERROR("写入图片失败: %s", full_path.c_str());
        fclose(fp);
        remove(full_path.c_str());
        return "";
    }
    fclose(fp);
    LOG_DEBUG("图片同步保存: %s", full_path.c_str());
    return full_path;
}

std::string EventManager::generateImageFilename(const AIEvent& event) {
    char buf[32];
    struct tm* tm_info = localtime(&event.timestamp);
    strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", tm_info);
    static std::atomic<uint32_t> counter{0};
    uint32_t seq = counter.fetch_add(1) % 1000;
    return event.type + "_" + std::string(buf) + "_" + std::to_string(seq) + ".jpg";
}