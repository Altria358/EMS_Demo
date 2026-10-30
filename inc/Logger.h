#ifndef LOGGER_H
#define LOGGER_H

#include <string>
#include <mutex>
#include <map>
#include <cstdio>

enum class LogLevel { DEBUG = 0, INFO = 1, WARN = 2, ERROR = 3 };

class Logger {
public:
    static Logger& instance();

    void setLevel(LogLevel lv) { level_ = lv; }
    LogLevel level() const { return level_; }

    // 全局日志文件
    void setFile(const std::string& path, size_t maxSizeMB = 10, int maxFiles = 5);

    // 为某个 tag 单独开文件（如 "Device1" → logs/Device1.log）
    void addTagFile(const std::string& tag, const std::string& path);

    // 写日志
    void log(LogLevel lv, const std::string& tag, const std::string& msg);

    void close();

private:
    Logger() = default;
    ~Logger();
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    void rotateIfNeeded(FILE*& fp, const std::string& path);
    void writeLine(FILE* fp, const char* ts, const char* lvStr,
                   const std::string& tag, const std::string& msg);

    std::mutex mtx_;
    LogLevel level_ = LogLevel::INFO;

    FILE* fp_ = nullptr;             // 全局
    std::string path_;
    size_t maxSize_ = 10 * 1024 * 1024;
    int maxFiles_ = 5;

    // tag → 文件
    std::map<std::string, FILE*> tagFiles_;
    std::map<std::string, std::string> tagPaths_;
};

#define LOG_DEBUG(tag, msg) Logger::instance().log(LogLevel::DEBUG, tag, msg)
#define LOG_INFO(tag, msg)  Logger::instance().log(LogLevel::INFO,  tag, msg)
#define LOG_WARN(tag, msg)  Logger::instance().log(LogLevel::WARN,  tag, msg)
#define LOG_ERROR(tag, msg) Logger::instance().log(LogLevel::ERROR, tag, msg)

#endif
