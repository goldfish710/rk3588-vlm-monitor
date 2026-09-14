#include "Logger.h"
#include <cstdarg>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <cstring>
#include <cstdio>

// ==================== 递归创建目录 ====================
static bool createDirRecursive(const std::string& path) {
    if (path.empty()) return true;

    struct stat st;
    if (stat(path.c_str(), &st) == 0) {
        return S_ISDIR(st.st_mode);
    }

    size_t pos = path.find_last_of('/');
    if (pos != std::string::npos) {
        std::string parent = path.substr(0, pos);
        if (!parent.empty() && !createDirRecursive(parent)) {
            return false;
        }
    }

    if (mkdir(path.c_str(), 0755) == 0) return true;

    if (errno == EEXIST) {
        if (stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) return true;
    }

    return false;
}

Logger* Logger::getInstance() {
    static Logger instance;                 //该局部静态变量只在第一次调用 getInstance() 时被初始化一次，之后每次调用都返回同一个对象的指针。这保证了整个进程中只存在一个 Logger 实例，符合单例模式的设计。
    return &instance;
}

Logger::~Logger() {
    if (fp_ && fp_ != stderr) {
        fclose(fp_);
        fp_ = nullptr;
    }
}

void Logger::init(const std::string& log_file, LogLevel min_level, size_t max_size) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (initialized_) return;

    log_file_ = log_file;
    min_level_ = min_level;
    max_size_ = max_size;
    bytes_written_ = 0;

    // ---------- 确保日志文件的父目录存在 ----------
    size_t pos = log_file_.find_last_of('/');
    if (pos != std::string::npos) {
        std::string parent = log_file_.substr(0, pos);
        if (!parent.empty() && !createDirRecursive(parent)) {
            fprintf(stderr, "[Logger] 无法创建日志目录: %s (%s)，回退到 stderr\n",
                    parent.c_str(), strerror(errno));
            fp_ = stderr;
            initialized_ = true;
            return;
        }
    }

    // ---------- 启动时若历史日志已超限，先轮转 ----------
    struct stat st;
    if (stat(log_file_.c_str(), &st) == 0 && st.st_size >= (long)max_size_) {
        std::string old = log_file_ + ".old";
        remove(old.c_str());
        rename(log_file_.c_str(), old.c_str());
    }

    // ---------- 打开日志文件 ----------
    fp_ = fopen(log_file_.c_str(), "a");
    if (!fp_) {
        fprintf(stderr, "[Logger] 无法打开日志文件: %s (%s)，回退到 stderr\n",
                log_file_.c_str(), strerror(errno));
        fp_ = stderr;
    } else {
        fprintf(fp_, "========== Logger initialized ==========\n");
        fflush(fp_);
    }

    initialized_ = true;
}

// ==================== 轮转（单份：.log -> .log.old） ====================
void Logger::rotateIfNeeded() {
    if (fp_ == stderr || fp_ == nullptr) return;
    if (bytes_written_ < max_size_) return;

    fflush(fp_);
    fclose(fp_);
    fp_ = nullptr;

    std::string old = log_file_ + ".old";
    remove(old.c_str());
    rename(log_file_.c_str(), old.c_str());

    fp_ = fopen(log_file_.c_str(), "a");
    if (!fp_) {
        fp_ = stderr;   // 轮转失败回退 stderr，避免崩溃
        fprintf(stderr, "[Logger] 轮转后重新打开失败，回退到 stderr\n");
    }
    bytes_written_ = 0;
}

void Logger::log(LogLevel level, const char* format, ...) {
    if (!initialized_) return;
    if (level < min_level_) return;

    // 获取当前时间
    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  now.time_since_epoch()) % 1000;

    std::tm tm_buf;
    localtime_r(&in_time_t, &tm_buf);

    char time_buf[32];
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &tm_buf);

    const char* level_str[] = {"DEBUG", "INFO", "WARN", "ERROR"};

    std::lock_guard<std::mutex> lock(mutex_);

    // 写前检查是否需要轮转
    rotateIfNeeded();

    int n = fprintf(fp_, "[%s.%03ld] %s: ", time_buf, ms.count(), level_str[level]);
    if (n > 0) bytes_written_ += n;

    va_list args;
    va_start(args, format);
    n = vfprintf(fp_, format, args);
    va_end(args);
    if (n > 0) bytes_written_ += n;

    n = fputc('\n', fp_);
    if (n > 0) bytes_written_ += 1;

    fflush(fp_);
}