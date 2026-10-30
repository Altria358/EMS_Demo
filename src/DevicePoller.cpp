#include "DevicePoller.h"
#include "RealTimeDB.h"
#include "Logger.h"

#include <thread>
#include <chrono>

// ============================================================================
// 构造 / 析构
// ============================================================================

DevicePoller::DevicePoller(int deviceNo,
                           const DeviceInstance& dev,
                           const std::vector<PointTemplate>& points)
    : deviceNo_(deviceNo), dev_(dev) {
    for (const auto& point : points)
        if (point.deviceId.empty() || point.deviceId == dev.deviceId) points_.push_back(point);

    createClient();

    // 点表分组
    ycGroups_ = groupPoints(points_, PointType::YC);
    yxGroups_ = groupPoints(points_, PointType::YX);

    LOG_INFO(logTag(), dev_.deviceId + " 点表分组: YC " +
                      std::to_string(ycGroups_.size()) + " 组, YX " +
                      std::to_string(yxGroups_.size()) + " 组");
}

void DevicePoller::createClient() {
    if (dev_.type == DeviceType::TCP) {
        client_ = std::make_unique<ModbusClient>(dev_.ip, dev_.port);
        LOG_INFO(logTag(), dev_.deviceId + " 创建 TCP 客户端 " +
                          dev_.ip + ":" + std::to_string(dev_.port));
    } else {
        client_ = std::make_unique<ModbusClient>(dev_.serialDev, dev_.baud,
                                                 dev_.parity, dev_.dataBit,
                                                 dev_.stopBit);
        LOG_INFO(logTag(), dev_.deviceId + " 创建 RTU 客户端 " +
                          dev_.serialDev);
    }

    client_->setSlave(dev_.slaveId);
    client_->setResponseTimeout(3, 0);
}

DevicePoller::~DevicePoller() {
    disconnect();
}

// ============================================================================
// 连接：只连一次，重试交给 PollerManager
// ============================================================================

bool DevicePoller::connect(int /*maxRetry*/) {
    std::lock_guard<std::mutex> lock(clientMutex_);
    // Recreate libmodbus context after a disconnect so no stale socket state
    // is carried into the next TCP/RTU connection attempt.
    client_.reset();
    createClient();
    if (client_->connect()) {
        connected_ = true;
        failCount_ = 0;
        for (auto& group : ycGroups_) group.consecutiveFailures = 0;
        for (auto& group : yxGroups_) group.consecutiveFailures = 0;
        LOG_INFO(logTag(), dev_.deviceId + " 连接成功");
        return true;
    }
    connected_ = false;
    return false;
}

void DevicePoller::disconnect() {
    std::lock_guard<std::mutex> lock(clientMutex_);
    if (connected_.load()) {
        client_->disconnect();
        connected_ = false;
        LOG_INFO(logTag(), dev_.deviceId + " 已断开");
    }
}

void DevicePoller::onGroupFail(const std::string& name, const std::string& err) {
    LOG_ERROR(logTag(), name + " 读失败: " + err);
}

// ============================================================================
// YC 组读
// ============================================================================

bool DevicePoller::pollYCGroup(const PointGroup& g) {
    std::vector<uint16_t> regs;
    int rc = client_->readInputRegisters(g.startAddr, g.count, regs);
    if (rc != g.count) {
        onGroupFail("YC组[" + std::to_string(g.startAddr) + "+" +
                    std::to_string(g.count) + "]",
                    client_->lastError());
        for (size_t idx : g.pointIdx) {
            std::string key = dev_.deviceId + "." + points_[idx].pointId;
            RealTimeDB::instance().markQuality(key, Quality::BAD);
        }
        return false;
    }

    for (size_t idx : g.pointIdx) {
        const PointTemplate& pt = points_[idx];
        int offset = pt.southAddr - g.startAddr;
        if (offset < 0 || offset + pt.southCount > static_cast<int>(regs.size())) {
            LOG_ERROR(logTag(), "越界: " + pt.pointId);
            continue;
        }

        float raw = pt.dataType == PointDataType::S16
            ? static_cast<float>(static_cast<int16_t>(regs[offset]))
            : static_cast<float>(regs[offset]);
        float v = raw * pt.scale + pt.offset;

        std::string key = dev_.deviceId + "." + pt.pointId;
        RealTimeDB::instance().updateIfChanged(key, v, Quality::GOOD, pt.deadband);
        LOG_DEBUG(logTag(), "YC " + key + " = " + std::to_string(v) + " " + pt.unit);
    }
    return true;
}

// ============================================================================
// YX 组读
// ============================================================================

bool DevicePoller::pollYXGroup(const PointGroup& g) {
    std::vector<uint8_t> bits;
    int rc = client_->readDiscreteInputs(g.startAddr, g.count, bits);
    if (rc != g.count) {
        onGroupFail("YX组[" + std::to_string(g.startAddr) + "+" +
                    std::to_string(g.count) + "]",
                    client_->lastError());
        for (size_t idx : g.pointIdx) {
            std::string key = dev_.deviceId + "." + points_[idx].pointId;
            RealTimeDB::instance().markQuality(key, Quality::BAD);
        }
        return false;
    }

    for (size_t idx : g.pointIdx) {
        const PointTemplate& pt = points_[idx];
        int offset = pt.southAddr - g.startAddr;
        if (offset < 0 || offset >= static_cast<int>(bits.size())) {
            LOG_ERROR(logTag(), "越界: " + pt.pointId);
            continue;
        }

        float v = bits[offset] ? 1.0f : 0.0f;
        std::string key = dev_.deviceId + "." + pt.pointId;
        RealTimeDB::instance().updateIfChanged(key, v, Quality::GOOD, pt.deadband);
        LOG_DEBUG(logTag(), "YX " + key + " = " + std::to_string((int)bits[offset]));
    }
    return true;
}

// ============================================================================
// 轮询一次
// ============================================================================

bool DevicePoller::pollOnce() {
    std::lock_guard<std::mutex> lock(clientMutex_);
    if (!connected_.load()) {
        return false;
    }

    bool allOk = true;
    bool reconnectRequired = false;
    int groupsPolled = 0;

    const auto now = std::chrono::steady_clock::now();
    for (auto& g : ycGroups_) {
        if (now - g.lastPoll < std::chrono::milliseconds(g.pollMs)) continue;
        g.lastPoll = now;
        ++groupsPolled;
        if (!pollYCGroup(g)) {
            allOk = false;
            ++g.consecutiveFailures;
            if (g.consecutiveFailures >= static_cast<unsigned>(maxFailBeforeReconnect_)) reconnectRequired = true;
        } else {
            g.consecutiveFailures = 0;
        }
    }

    for (auto& g : yxGroups_) {
        if (now - g.lastPoll < std::chrono::milliseconds(g.pollMs)) continue;
        g.lastPoll = now;
        ++groupsPolled;
        if (!pollYXGroup(g)) {
            allOk = false;
            ++g.consecutiveFailures;
            if (g.consecutiveFailures >= static_cast<unsigned>(maxFailBeforeReconnect_)) reconnectRequired = true;
        } else {
            g.consecutiveFailures = 0;
        }
    }

    // No group due this tick is not a successful communication round.
    if (groupsPolled == 0) return true;

    if (reconnectRequired) {
        LOG_ERROR(logTag(), dev_.deviceId + " 采集组连续失败达到阈值，重建连接");
        client_->disconnect();
        connected_ = false;
        failCount_ = 0;
        return false;
    }

    if (allOk) failCount_ = 0;
    else failCount_++;

    return allOk;
}

bool DevicePoller::writePoint(const PointTemplate& point, uint16_t value) {
    std::lock_guard<std::mutex> lock(clientMutex_);
    if (!connected_.load()) return false;
    int rc = -1;
    if (point.type == PointType::YK)
        rc = client_->writeSingleCoil(point.southAddr, value != 0);
    else if (point.type == PointType::YT)
        rc = client_->writeSingleRegister(point.southAddr, value);
    else
        return false;
    if (rc < 0) {
        LOG_ERROR(logTag(), "控制点写入失败 " + point.pointId + ": " + client_->lastError());
        client_->disconnect();
        connected_ = false;
        return false;
    }
    LOG_INFO(logTag(), "控制点写入成功 " + point.pointId + " raw=" + std::to_string(value));
    return true;
}

bool DevicePoller::writeCoils(uint16_t address, const std::vector<uint8_t>& values) {
    std::lock_guard<std::mutex> lock(clientMutex_);
    if (!connected_.load() || values.empty()) return false;
    if (client_->writeMultipleCoils(address, values) < 0) {
        LOG_ERROR(logTag(), dev_.deviceId + " 批量写线圈失败: " + client_->lastError());
        client_->disconnect();
        connected_ = false;
        return false;
    }
    LOG_INFO(logTag(), dev_.deviceId + " 批量写线圈成功 address=" +
             std::to_string(address) + " count=" + std::to_string(values.size()));
    return true;
}

bool DevicePoller::writeRegisters(uint16_t address, const std::vector<uint16_t>& values) {
    std::lock_guard<std::mutex> lock(clientMutex_);
    if (!connected_.load() || values.empty()) return false;
    if (client_->writeMultipleRegisters(address, values) < 0) {
        LOG_ERROR(logTag(), dev_.deviceId + " 批量写寄存器失败: " + client_->lastError());
        client_->disconnect();
        connected_ = false;
        return false;
    }
    LOG_INFO(logTag(), dev_.deviceId + " 批量写寄存器成功 address=" +
             std::to_string(address) + " count=" + std::to_string(values.size()));
    return true;
}
