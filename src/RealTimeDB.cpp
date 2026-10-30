#include "RealTimeDB.h"
#include "Logger.h"

#include <cstdio>
#include <cmath>

/**
 * @brief 返回实时数据库单例。
 * @return 实时数据库单例引用。
 */
RealTimeDB& RealTimeDB::instance() {
    static RealTimeDB inst;
    return inst;
}

/**
 * @brief 更新指定数据点的质量码。
 * @param id 数据点 ID。
 * @param q 新质量码。
 * @return 无。
 */
void RealTimeDB::markQuality(const std::string& id, Quality q) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = db_.find(id);
    if (it != db_.end()) {
        it->second.quality = q;
    } else {
        // 点还没建过，初始化一个
        db_[id] = RTV{0.0f, q, std::time(nullptr)};
    }
}

/**
 * @brief 覆盖更新指定实时数据点。
 * @param id 数据点 ID。
 * @param v 数据值。
 * @param q 数据质量码。
 * @return 无。
 */
void RealTimeDB::update(const std::string& id, float v, Quality q) {
    std::lock_guard<std::mutex> lk(mtx_);
    RTV& r = db_[id];
    r.value   = v;
    r.quality = q;
    r.ts      = std::time(nullptr);
}

/**
 * @brief 仅在值或质量变化时更新数据点。
 * @param id 数据点 ID。
 * @param v 数据值。
 * @param q 数据质量码。
 * @param epsilon 数值变化判断阈值。
 * @return 数据发生变化并完成更新返回 true，否则返回 false。
 */
bool RealTimeDB::updateIfChanged(const std::string& id, float v,
                                 Quality q, float epsilon) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = db_.find(id);

    // 新点 或 值变了
    if (it == db_.end() ||
        std::fabs(it->second.value - v) > epsilon ||
        it->second.quality != q) {

        RTV& r = db_[id];
        r.value   = v;
        r.quality = q;
        r.ts      = std::time(nullptr);
        return true;
    }

    // 采集时间仍需刷新，即使死区内的值没有变化。
    it->second.ts = std::time(nullptr);
    return false;
}

/**
 * @brief 查询单个实时数据点。
 * @param id 数据点 ID。
 * @param out 接收实时值的输出对象。
 * @return 找到数据点返回 true，否则返回 false。
 */
bool RealTimeDB::get(const std::string& id, RTV& out) const {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = db_.find(id);
    if (it == db_.end()) return false;
    out = it->second;
    return true;
}

/**
 * @brief 获取全部实时数据的副本。
 * @return 实时数据副本。
 */
std::map<std::string, RTV> RealTimeDB::snapshot() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return db_;
}

/**
 * @brief 查询指定前缀下的实时数据。
 * @param prefix 点名筛选前缀。
 * @return 匹配前缀的数据点列表。
 */
std::vector<std::pair<std::string, RTV>>
RealTimeDB::queryPrefix(const std::string& prefix) const {
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<std::pair<std::string, RTV>> out;
    for (auto& kv : db_) {
        if (kv.first.compare(0, prefix.size(), prefix) == 0) {
            out.push_back(kv);
        }
    }
    return out;
}

/**
 * @brief 输出实时数据库内容。
 * @return 无。
 */
void RealTimeDB::dump() const {
    std::lock_guard<std::mutex> lk(mtx_);
    LOG_INFO("RTDB", "---- 实时库 (" + std::to_string(db_.size()) + " 点) ----");
    for (auto& kv : db_) {
        char buf[128];
        snprintf(buf, sizeof(buf), "%-24s = %10.2f  q=%d",
                 kv.first.c_str(), kv.second.value,
                 static_cast<int>(kv.second.quality));
        LOG_INFO("RTDB", buf);
    }
}

/**
 * @brief 清空实时数据库。
 * @return 无。
 */
void RealTimeDB::clear() {
    std::lock_guard<std::mutex> lk(mtx_);
    db_.clear();
}
