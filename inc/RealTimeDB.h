#ifndef REAL_TIME_DB_H
#define REAL_TIME_DB_H

#include <string>
#include <map>
#include <mutex>
#include <cstdint>
#include <ctime>
#include <vector>

// 质量码
enum class Quality : uint8_t {
    GOOD     = 0,   // 正常
    BAD      = 1,   // 通信失败/无效
    STALE    = 2,   // 数据陈旧（超时未更新）
    UNINIT   = 3    // 未初始化
};

// 实时值
struct RTV {
    float    value    = 0.0f;
    Quality  quality  = Quality::UNINIT;
    time_t   ts       = 0;      // 最后更新时间
};

class RealTimeDB {
public:
    // 获取实时数据库单例。
    static RealTimeDB& instance();

    // 更新数据点并覆盖旧值。
    void update(const std::string& id, float v,
                Quality q = Quality::GOOD);

    // 仅在数值或质量变化时更新数据点。
    bool updateIfChanged(const std::string& id, float v,
                         Quality q = Quality::GOOD,
                         float epsilon = 1e-6f);
    // 更新数据点质量码。
    void markQuality(const std::string& id, Quality q);
    // 获取单个实时数据点。
    bool get(const std::string& id, RTV& out) const;

    // 获取全部实时数据快照。
    std::map<std::string, RTV> snapshot() const;

    // 按点名前缀查询实时数据。
    std::vector<std::pair<std::string, RTV>>
    queryPrefix(const std::string& prefix) const;

    // 打印当前实时数据库内容。
    void dump() const;

    // 清空全部实时数据。
    void clear();

private:
    RealTimeDB() = default;
    mutable std::mutex mtx_;
    std::map<std::string, RTV> db_;
};

#endif