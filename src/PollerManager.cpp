#include "PollerManager.h"
#include "Logger.h"

#include <chrono>
#include <algorithm>

namespace {
std::string endpointFor(const DeviceInstance& device) {
    if (device.type == DeviceType::TCP) {
        return device.ip + ":" + std::to_string(device.port);
    }
    return device.serialDev + " @ " + std::to_string(device.baud) +
           "," + device.parity + "," + std::to_string(device.dataBit) +
           "," + std::to_string(device.stopBit);
}

DeviceInfo makeDeviceInfo(const DevicePoller& poller) {
    const auto& device = poller.device();
    DeviceInfo info;
    info.deviceNo = "Device" + std::to_string(poller.deviceNo());
    info.deviceId = device.deviceId;
    info.type = device.type;
    info.endpoint = endpointFor(device);
    info.connected = poller.isConnected();
    info.failCount = poller.failCount();
    return info;
}
}

PollerManager::PollerManager(const std::vector<DeviceInstance>& devices,
                             const std::vector<PointTemplate>& points) {
    pollers_.reserve(devices.size());
    int deviceNo = 1;
    for (const auto& device : devices) {
        std::string tag = "Device" + std::to_string(deviceNo);
        LOG_INFO(tag, device.deviceId + " (" + deviceTypeStr(device.type) +
                      ") 配置加载");
        pollers_.push_back(std::make_unique<DevicePoller>(deviceNo, device, points));
        ++deviceNo;
    }
}

PollerManager::~PollerManager() {
    stop();
}

bool PollerManager::getDeviceInfo(const std::string& deviceNo,
                                  DeviceInfo& info) const {
    for (const auto& poller : pollers_) {
        DeviceInfo candidate = makeDeviceInfo(*poller);
        if (candidate.deviceNo == deviceNo) {
            info = candidate;
            return true;
        }
    }
    return false;
}

std::vector<DeviceInfo> PollerManager::getAllDeviceInfo() const {
    std::vector<DeviceInfo> infos;
    infos.reserve(pollers_.size());
    for (const auto& poller : pollers_) {
        infos.push_back(makeDeviceInfo(*poller));
    }
    return infos;
}

bool PollerManager::writePoint(const std::string& pointId, uint16_t value) {
    bool found = false;
    bool ok = true;
    for (const auto& poller : pollers_) {
        for (const auto& point : poller->points()) {
            if (point.pointId == pointId && (point.type == PointType::YK || point.type == PointType::YT)) {
                found = true;
                ok = poller->writePoint(point, value) && ok;
            }
        }
    }
    if (!found) LOG_WARN("PollerManager", "未找到可写控制点: " + pointId);
    return found && ok;
}

bool PollerManager::writePoint(const std::string& deviceId,
                               const std::string& pointId, uint16_t value) {
    for (const auto& poller : pollers_) {
        if (poller->deviceId() != deviceId) continue;
        for (const auto& point : poller->points()) {
            if (point.pointId == pointId &&
                (point.type == PointType::YK || point.type == PointType::YT))
                return poller->writePoint(point, value);
        }
    }
    LOG_WARN("PollerManager", "设备/控制点不存在: " + deviceId + "." + pointId);
    return false;
}

bool PollerManager::writeDeviceCoils(const std::string& deviceId, uint16_t address,
                                     const std::vector<uint8_t>& values) {
    for (const auto& poller : pollers_)
        if (poller->deviceId() == deviceId) return poller->writeCoils(address, values);
    LOG_WARN("PollerManager", "未找到批量写线圈的设备: " + deviceId);
    return false;
}

bool PollerManager::writeDeviceRegisters(const std::string& deviceId, uint16_t address,
                                         const std::vector<uint16_t>& values) {
    for (const auto& poller : pollers_)
        if (poller->deviceId() == deviceId) return poller->writeRegisters(address, values);
    LOG_WARN("PollerManager", "未找到批量写寄存器的设备: " + deviceId);
    return false;
}

bool PollerManager::start() {
    if (running_.exchange(true)) {
        LOG_WARN("PollerManager", "设备轮询管理器已运行");
        return false;
    }

    if (pollers_.empty()) {
        running_ = false;
        LOG_ERROR("PollerManager", "没有可启动的设备");
        return false;
    }

    threads_.reserve(pollers_.size());
    for (auto& poller : pollers_) {
        threads_.emplace_back(&PollerManager::runPoller, this, poller.get());
    }

    LOG_INFO("PollerManager", "已启动 " +
             std::to_string(threads_.size()) + " 个设备任务");
    return true;
}

void PollerManager::stop() {
    if (!running_.exchange(false)) {
        return;
    }
    stopCv_.notify_all();

    for (auto& thread : threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    threads_.clear();
    LOG_INFO("PollerManager", "所有设备任务已停止");
}

void PollerManager::runPoller(DevicePoller* poller) {
    const auto pollPeriod = std::chrono::milliseconds(100);

    // 日志 tag 带设备号，写入专属文件
    const std::string tag = "Device" + std::to_string(poller->deviceNo());

    int reconnectDelay  = 2;
    const int maxDelay  = 10;
    int lastLoggedDelay = -1;

    auto nextTick = std::chrono::steady_clock::now();

    while (running_) {
        if (!poller->isConnected()) {
            if (!poller->connect(0)) {
                if (reconnectDelay != lastLoggedDelay) {
                    LOG_WARN(tag, poller->deviceId() +
                             " 未连接，" + std::to_string(reconnectDelay) +
                             " 秒后重试");
                    lastLoggedDelay = reconnectDelay;
                }

                std::unique_lock<std::mutex> lock(stopMutex_);
                if (stopCv_.wait_for(lock, std::chrono::seconds(reconnectDelay),
                                     [this] { return !running_.load(); })) {
                    break;
                }

                reconnectDelay = std::min(reconnectDelay * 2, maxDelay);
                continue;
            }

            LOG_INFO(tag, poller->deviceId() + " 已连接，开始轮询");
            nextTick = std::chrono::steady_clock::now();
        }

        bool ok = poller->pollOnce();

        if (ok) {
            reconnectDelay  = 2;
            lastLoggedDelay = -1;
        }

        nextTick += pollPeriod;
        auto now = std::chrono::steady_clock::now();
        if (nextTick > now) {
            std::unique_lock<std::mutex> lock(stopMutex_);
            stopCv_.wait_until(lock, nextTick,
                               [this] { return !running_.load(); });
        } else {
            nextTick = now;
        }
    }

    poller->disconnect();
    LOG_INFO(tag, poller->deviceId() + " 线程退出");
}
