#include "Logger.h"

#include <ctime>
#include <chrono>
#include <sys/stat.h>
#include <cstdio>
#include <cstring>

Logger& Logger::instance() {
    static Logger inst;
    return inst;
}

Logger::~Logger() {
    close();
}

void Logger::close() {
    std::lock_guard<std::mutex> lk(mtx_);
    if (fp_) {
        fclose(fp_);
        fp_ = nullptr;
    }
    for (auto& kv : tagFiles_) {
        if (kv.second) fclose(kv.second);
    }
    tagFiles_.clear();
}

void Logger::setFile(const std::string& path, size_t maxSizeMB, int maxFiles) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (fp_) {
        fclose(fp_);
        fp_ = nullptr;
    }
    path_ = path;
    maxSize_ = maxSizeMB * 1024 * 1024;
    maxFiles_ = maxFiles;
    fp_ = fopen(path_.c_str(), "a");
}

void Logger::addTagFile(const std::string& tag, const std::string& path) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = tagFiles_.find(tag);
    if (it != tagFiles_.end()) {
        if (it->second) fclose(it->second);
        it->second = nullptr;
    }
    FILE* fp = fopen(path.c_str(), "a");
    if (fp) {
        tagFiles_[tag] = fp;
        tagPaths_[tag] = path;
    }
}

void Logger::rotateIfNeeded(FILE*& fp, const std::string& path) {
    if (!fp || path.empty()) return;

    struct stat st;
    if (stat(path.c_str(), &st) != 0) return;
    if (static_cast<size_t>(st.st_size) < maxSize_) return;

    fclose(fp);
    fp = nullptr;

    for (int i = maxFiles_ - 1; i >= 1; --i) {
        std::string from = path + "." + std::to_string(i);
        std::string to   = path + "." + std::to_string(i + 1);
        rename(from.c_str(), to.c_str());
    }
    rename(path.c_str(), (path + ".1").c_str());

    fp = fopen(path.c_str(), "a");
}

void Logger::writeLine(FILE* fp, const char* ts, const char* lvStr,
                       const std::string& tag, const std::string& msg) {
    if (!fp) return;
    fprintf(fp, "[%s][%s][%s] %s\n", ts, lvStr, tag.c_str(), msg.c_str());
    fflush(fp);
}

void Logger::log(LogLevel lv, const std::string& tag, const std::string& msg) {
    if (lv < level_) return;

    const char* lvStr = "INFO";
    switch (lv) {
        case LogLevel::DEBUG: lvStr = "DEBUG"; break;
        case LogLevel::INFO:  lvStr = "INFO "; break;
        case LogLevel::WARN:  lvStr = "WARN "; break;
        case LogLevel::ERROR: lvStr = "ERROR"; break;
    }

    auto now = std::chrono::system_clock::now();
    auto t   = std::chrono::system_clock::to_time_t(now);
    auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(
                   now.time_since_epoch()) % 1000;

    std::tm tm_buf;
    localtime_r(&t, &tm_buf);

    char ts[40];
    std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_buf);
    char tsFull[48];
    snprintf(tsFull, sizeof(tsFull), "%s.%03d", ts, static_cast<int>(ms.count()));

    std::lock_guard<std::mutex> lk(mtx_);

    // 1. 终端
    fprintf(stdout, "[%s][%s][%s] %s\n", tsFull, lvStr, tag.c_str(), msg.c_str());
    fflush(stdout);

    // 2. 全局文件
    if (fp_) {
        rotateIfNeeded(fp_, path_);
        writeLine(fp_, tsFull, lvStr, tag, msg);
    }

    // 3. tag 专属文件
    auto it = tagFiles_.find(tag);
    if (it != tagFiles_.end()) {
        rotateIfNeeded(it->second, tagPaths_[tag]);
        writeLine(it->second, tsFull, lvStr, tag, msg);
    }
}
