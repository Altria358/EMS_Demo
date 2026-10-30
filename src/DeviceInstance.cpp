#include "DeviceInstance.h"
#include "Logger.h"

#include <fstream>
#include <sstream>
#include <cstdio>

// ============================================================================
// 工具：trim
// ============================================================================
/**
 * @brief 去除字符串首尾空白字符。
 * @param s 待处理字符串。
 * @return 去除首尾空白后的字符串。
 */
static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// ============================================================================
// 类型转字符串
// ============================================================================
/**
 * @brief 将设备类型转换为文本表示。
 * @param t 设备类型。
 * @return 设备类型字符串。
 */
const char* deviceTypeStr(DeviceType t) {
    return (t == DeviceType::TCP) ? "TCP" : "RTU";
}

/**
 * @brief 将设备配置序列化为配置文件行。
 * @param device 待序列化的设备配置。
 * @return 配置文件格式的单行字符串。
 */
static std::string deviceConfigLine(const DeviceInstance& device) {
    std::ostringstream line;
    line << deviceTypeStr(device.type) << ','
         << device.deviceId << ',' << device.slaveId << ',';

    if (device.type == DeviceType::TCP) {
        line << device.ip << ',' << device.port;
    } else {
        line << device.serialDev << ',' << device.baud << ','
             << device.parity << ',' << device.dataBit << ','
             << device.stopBit;
    }
    return line.str();
}

/**
 * @brief 按设备序号更新设备配置文件中的一行。
 * @param path 设备配置文件路径。
 * @param deviceNo 要修改的设备序号，从 1 开始。
 * @param config 新的设备配置。
 * @return 修改成功返回 true，否则返回 false。
 */
bool updateDeviceConfig(const std::string& path,
                        int deviceNo,
                        const DeviceInstance& config) {
    if (deviceNo <= 0 || config.deviceId.empty() || config.slaveId < 0 ||
        config.slaveId > 247) {
        return false;
    }

    if (config.type == DeviceType::TCP) {
        if (config.ip.empty() || config.port <= 0 || config.port > 65535) {
            return false;
        }
    } else {
        if (config.serialDev.empty() || config.baud <= 0 ||
            (config.parity != 'N' && config.parity != 'O' && config.parity != 'E') ||
            (config.dataBit != 7 && config.dataBit != 8) ||
            (config.stopBit != 1 && config.stopBit != 2)) {
            return false;
        }
    }

    std::ifstream input(path);
    if (!input.is_open()) {
        return false;
    }

    std::vector<std::string> lines;
    std::string line;
    int currentDeviceNo = 0;
    bool updated = false;

    while (std::getline(input, line)) {
        std::string trimmed = trim(line);
        if (!trimmed.empty() && trimmed[0] != '#') {
            std::stringstream fields(trimmed);
            std::string type;
            std::getline(fields, type, ',');
            if (type == "TCP" || type == "RTU") {
                ++currentDeviceNo;
                if (currentDeviceNo == deviceNo) {
                    line = deviceConfigLine(config);
                    updated = true;
                }
            }
        }
        lines.push_back(line);
    }
    input.close();

    if (!updated) {
        return false;
    }

    const std::string tempPath = path + ".tmp";
    std::ofstream output(tempPath, std::ios::trunc);
    if (!output.is_open()) {
        return false;
    }
    for (const auto& savedLine : lines) {
        output << savedLine << '\n';
    }
    output.close();

    if (std::rename(tempPath.c_str(), path.c_str()) != 0) {
        std::remove(tempPath.c_str());
        return false;
    }
    return true;
}

// ============================================================================
// 加载设备
// ============================================================================
/**
 * @brief 解析设备配置文件并返回设备列表。
 * @param path 设备配置文件路径。
 * @return 已成功解析的设备列表。
 */
std::vector<DeviceInstance> loadDeviceInstances(const std::string& path) {
    std::vector<DeviceInstance> devs;
    std::ifstream in(path);
    if (!in) {
        LOG_WARN("Config", "无法打开设备配置: " + path);
        return devs;
    }

    std::string line;
    int lineNo = 0;
    int okCount = 0, failCount = 0;

    while (std::getline(in, line)) {
        lineNo++;

        std::string t = trim(line);
        if (t.empty() || t[0] == '#') continue;

        std::stringstream ss(t);
        std::string tok;
        std::vector<std::string> parts;
        while (std::getline(ss, tok, ',')) {
            parts.push_back(trim(tok));
        }

        if (parts.empty()) continue;

        DeviceInstance d;
        bool ok = false;

        try {
            if (parts[0] == "TCP") {
                if (parts.size() < 5) {
                    LOG_WARN("Config", "设备配置第 " + std::to_string(lineNo) +
                                      " 行 TCP 字段不足，跳过");
                    failCount++;
                    continue;
                }
                d.type     = DeviceType::TCP;
                d.deviceId = parts[1];
                d.slaveId  = std::stoi(parts[2]);
                d.ip       = parts[3];
                d.port     = std::stoi(parts[4]);

                // 校验
                if (d.deviceId.empty() || d.ip.empty()) {
                    LOG_WARN("Config", "设备配置第 " + std::to_string(lineNo) +
                                      " 行设备名或 IP 为空，跳过");
                    failCount++;
                    continue;
                }
                if (d.port <= 0 || d.port > 65535) {
                    LOG_WARN("Config", "设备配置第 " + std::to_string(lineNo) +
                                      " 行端口无效: " + std::to_string(d.port));
                    failCount++;
                    continue;
                }
                ok = true;
            }
            else if (parts[0] == "RTU") {
                if (parts.size() < 7) {
                    LOG_WARN("Config", "设备配置第 " + std::to_string(lineNo) +
                                      " 行 RTU 字段不足，跳过");
                    failCount++;
                    continue;
                }
                d.type      = DeviceType::RTU;
                d.deviceId  = parts[1];
                d.slaveId   = std::stoi(parts[2]);
                d.serialDev = parts[3];
                d.baud      = std::stoi(parts[4]);
                d.parity    = parts[5].empty() ? 'N' : parts[5][0];
                d.dataBit   = std::stoi(parts[6]);
                d.stopBit   = (parts.size() > 7) ? std::stoi(parts[7]) : 1;

                // 校验
                if (d.deviceId.empty() || d.serialDev.empty()) {
                    LOG_WARN("Config", "设备配置第 " + std::to_string(lineNo) +
                                      " 行设备名或串口为空，跳过");
                    failCount++;
                    continue;
                }
                if (d.baud <= 0) {
                    LOG_WARN("Config", "设备配置第 " + std::to_string(lineNo) +
                                      " 行波特率无效: " + std::to_string(d.baud));
                    failCount++;
                    continue;
                }
                ok = true;
            }
            else {
                LOG_WARN("Config", "设备配置第 " + std::to_string(lineNo) +
                                  " 行类型无效: " + parts[0]);
                failCount++;
                continue;
            }
        }
        catch (const std::exception& e) {
            LOG_ERROR("Config", "设备配置第 " + std::to_string(lineNo) +
                               " 行解析错误: " + e.what());
            failCount++;
            continue;
        }

        // 从站号范围校验
        if (d.slaveId < 0 || d.slaveId > 247) {
            LOG_WARN("Config", "设备配置第 " + std::to_string(lineNo) +
                              " 行从站号超范围: " + std::to_string(d.slaveId));
            // 不跳过，只警告
        }

        if (ok) {
            devs.push_back(d);
            okCount++;
        }
    }

    LOG_INFO("Config", "设备加载: " + std::to_string(okCount) +
                      " 台 (失败 " + std::to_string(failCount) + " 行)");
    return devs;
}