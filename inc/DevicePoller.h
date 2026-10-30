#ifndef DEVICE_POLLER_H
#define DEVICE_POLLER_H

#include "ModbusClient.h"
#include "DeviceInstance.h"
#include "PointTemplate.h"
#include "PointGroup.h"

#include <memory>
#include <vector>
#include <string>
#include <atomic>
#include <mutex>

class DevicePoller {
public:
    DevicePoller(int deviceNo,
                 const DeviceInstance& dev,
                 const std::vector<PointTemplate>& points);
    ~DevicePoller();

    bool connect(int maxRetry = 3);
    void disconnect();
    bool pollOnce();
    bool writePoint(const PointTemplate& point, uint16_t value);
    bool writeCoils(uint16_t address, const std::vector<uint8_t>& values);
    bool writeRegisters(uint16_t address, const std::vector<uint16_t>& values);

    const std::string& deviceId() const { return dev_.deviceId; }
    int deviceNo() const { return deviceNo_; }
    const DeviceInstance& device() const { return dev_; }
    const std::vector<PointTemplate>& points() const { return points_; }

    bool isConnected() const { return connected_.load(); }
    int failCount() const { return failCount_.load(); }

private:
    std::string logTag() const { return "Device" + std::to_string(deviceNo_); }
    void createClient();

    int deviceNo_;
    DeviceInstance dev_;
    std::vector<PointTemplate> points_;
    std::unique_ptr<ModbusClient> client_;
    mutable std::mutex clientMutex_;

    // 分组：YC 和 YX 分开
    std::vector<PointGroup> ycGroups_;
    std::vector<PointGroup> yxGroups_;

    std::atomic<bool> connected_{false};
    std::atomic<int>  failCount_{0};
    int  maxFailBeforeReconnect_ = 3;

    // 按组读
    bool pollYCGroup(const PointGroup& g);
    bool pollYXGroup(const PointGroup& g);

    void onGroupFail(const std::string& name, const std::string& err);
};

#endif
