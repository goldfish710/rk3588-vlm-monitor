#ifndef LOGGER_H
#define LOGGER_H

#include <string>
#include <cstdio>
#include <ctime>
#include <mutex>

enum LogLevel {
    LOG_DEBUG = 0,
    LOG_INFO  = 1,
    LOG_WARN  = 2,
    LOG_ERROR = 3
};

class Logger {
public:
    static Logger* getInstance();

    void init(const std::string& log_file, LogLevel min_level = LOG_INFO,
              size_t max_size = 10 * 1024 * 1024);   // 默认单文件 10MB
    void log(LogLevel level, const char* format, ...);

    void setMinLevel(LogLevel level) { min_level_ = level; }

private:
    Logger() = default;
    ~Logger();

    void rotateIfNeeded();

    std::string log_file_;
    LogLevel min_level_ = LOG_INFO;
    size_t max_size_ = 10 * 1024 * 1024;
    size_t bytes_written_ = 0;
    std::mutex mutex_;
    FILE* fp_ = nullptr;
    bool initialized_ = false;
};

// 便捷宏
#define LOG_DEBUG(fmt, ...) Logger::getInstance()->log(LOG_DEBUG, fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...)  Logger::getInstance()->log(LOG_INFO,  fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  Logger::getInstance()->log(LOG_WARN,  fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) Logger::getInstance()->log(LOG_ERROR, fmt, ##__VA_ARGS__)

#endif