#ifndef POLLER_MANAGER_H
#define POLLER_MANAGER_H

#include "DevicePoller.h"

#include <atomic>
#include <memory>
#include <thread>
#include <vector>
#include <condition_variable>
#include <mutex>

struct DeviceInfo {
    std::string deviceNo;
    std::string deviceId;
    DeviceType type;
    std::string endpoint;
    bool connected = false;
    int failCount = 0;
};

class PollerManager {
public:
    // 创建多设备轮询管理器。
    PollerManager(const std::vector<DeviceInstance>& devices,
                  const std::vector<PointTemplate>& points);
    // 停止所有轮询任务并销毁管理器。
    ~PollerManager();

    PollerManager(const PollerManager&) = delete;
    PollerManager& operator=(const PollerManager&) = delete;

    // 启动所有设备的独立轮询任务。
    bool start();
    // 停止所有设备轮询任务。
    void stop();
    // 获取管理器运行状态。
    bool isRunning() const { return running_.load(); }
    // 获取当前已连接设备数量。
    int connectedCount() const { return connectedCount_.load(); }
    // 按 DeviceN 查询设备信息。
    bool getDeviceInfo(const std::string& deviceNo, DeviceInfo& info) const;
    // 获取全部设备信息。
    std::vector<DeviceInfo> getAllDeviceInfo() const;
    bool writePoint(const std::string& pointId, uint16_t value);
    bool writePoint(const std::string& deviceId, const std::string& pointId, uint16_t value);
    bool writeDeviceCoils(const std::string& deviceId, uint16_t address,
                          const std::vector<uint8_t>& values);
    bool writeDeviceRegisters(const std::string& deviceId, uint16_t address,
                              const std::vector<uint16_t>& values);

private:
    // 在线程中连接并轮询单台设备。
    void runPoller(DevicePoller* poller);

    std::vector<std::unique_ptr<DevicePoller>> pollers_;
    std::vector<std::thread> threads_;
    std::atomic<bool> running_{false};
    std::atomic<int> connectedCount_{0};
    std::mutex stopMutex_;
    std::condition_variable stopCv_;
};

#endif
