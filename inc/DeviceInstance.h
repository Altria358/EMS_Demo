#ifndef DEVICE_INSTANCE_H
#define DEVICE_INSTANCE_H

#include <string>
#include <vector>
#include <cstdint>

enum class DeviceType { TCP, RTU };

struct DeviceInstance {
    std::string deviceId;
    DeviceType  type;
    int         slaveId = 1;

    // TCP
    std::string ip;
    int         port = 502;

    // RTU
    std::string serialDev;
    int         baud     = 9600;
    char        parity   = 'N';
    int         dataBit  = 8;
    int         stopBit  = 1;
};

// 从配置文件加载设备列表。
std::vector<DeviceInstance> loadDeviceInstances(const std::string& path);

// 按 Device1、Device2... 的顺序修改配置文件中的设备。
bool updateDeviceConfig(const std::string& path,
                        int deviceNo,
                        const DeviceInstance& config);

// 将设备类型转换为字符串。
const char* deviceTypeStr(DeviceType t);

#endif