#include "DeviceInstance.h"
#include "PointTemplate.h"
#include "PollerManager.h"
#include "RealTimeDB.h"
#include "HistoryStore.h"
#include "ModbusServer.h"
#include "Logger.h"

#include <thread>
#include <chrono>
#include <csignal>
#include <atomic>
#include <string>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <algorithm>
#include <map>
#include <vector>
#include <cmath>
#include <cctype>
#include <sys/stat.h>
#include <sys/types.h>
#include <cerrno>

// ============================================================================
// 全局
// ============================================================================
static std::atomic<bool> g_running{true};

/**
 * @brief 处理进程退出信号。
 * @param signalNumber 收到的信号编号。
 * @return 无。
 */
static void onSignal(int signalNumber) {
    (void)signalNumber;
    g_running = false;
}

static bool jsonStringField(const std::string& json, const std::string& key, std::string& value) {
    const std::string marker = "\"" + key + "\"";
    size_t pos = json.find(marker);
    if (pos == std::string::npos) return false;
    pos = json.find(':', pos + marker.size());
    if (pos == std::string::npos) return false;
    pos = json.find_first_not_of(" \t\r\n", pos + 1);
    if (pos == std::string::npos || json[pos++] != '"') return false;
    value.clear();
    while (pos < json.size()) {
        char ch = json[pos++];
        if (ch == '"') return true;
        if (ch == '\\') {
            if (pos >= json.size()) return false;
            char escaped = json[pos++];
            if (escaped != '"' && escaped != '\\' && escaped != '/') return false;
            value.push_back(escaped);
        } else value.push_back(ch);
    }
    return false;
}

static bool jsonNumberField(const std::string& json, const std::string& key, double& value) {
    const std::string marker = "\"" + key + "\"";
    size_t pos = json.find(marker);
    if (pos == std::string::npos) return false;
    pos = json.find(':', pos + marker.size());
    if (pos == std::string::npos) return false;
    const char* begin = json.c_str() + pos + 1;
    while (*begin == ' ' || *begin == '\t' || *begin == '\r' || *begin == '\n') ++begin;
    char* end = nullptr;
    value = std::strtod(begin, &end);
    return end != begin && std::isfinite(value);
}

static bool jsonObjectField(const std::string& json, const std::string& key, std::string& value) {
    const std::string marker = "\"" + key + "\"";
    size_t pos = json.find(marker);
    if (pos == std::string::npos) return false;
    pos = json.find(':', pos + marker.size());
    if (pos == std::string::npos) return false;
    pos = json.find_first_not_of(" \t\r\n", pos + 1);
    if (pos == std::string::npos || json[pos] != '{') return false;
    const size_t begin = pos;
    int depth = 0;
    bool inString = false;
    bool escaped = false;
    for (; pos < json.size(); ++pos) {
        const char ch = json[pos];
        if (inString) {
            if (escaped) escaped = false;
            else if (ch == '\\') escaped = true;
            else if (ch == '"') inString = false;
            continue;
        }
        if (ch == '"') inString = true;
        else if (ch == '{') ++depth;
        else if (ch == '}' && --depth == 0) {
            value = json.substr(begin, pos - begin + 1);
            return true;
        }
    }
    return false;
}

static bool jsonNumberArrayField(const std::string& json, const std::string& key,
                                std::vector<double>& values) {
    const std::string marker = "\"" + key + "\"";
    size_t pos = json.find(marker);
    if (pos == std::string::npos) return false;
    pos = json.find(':', pos + marker.size());
    if (pos == std::string::npos) return false;
    ++pos;
    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) ++pos;
    if (pos == json.size() || json[pos++] != '[') return false;
    values.clear();
    while (pos < json.size()) {
        while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) ++pos;
        if (pos < json.size() && json[pos] == ']') return !values.empty();
        char* end = nullptr;
        const double number = std::strtod(json.c_str() + pos, &end);
        if (end == json.c_str() + pos || !std::isfinite(number)) return false;
        values.push_back(number);
        pos = static_cast<size_t>(end - json.c_str());
        while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) ++pos;
        if (pos < json.size() && json[pos] == ',') ++pos;
        else if (pos < json.size() && json[pos] == ']') return !values.empty();
        else return false;
    }
    return false;
}

static std::string processCommand(const DbCommand& command,
                                  const std::vector<PointTemplate>& points,
                                  PollerManager& pollerManager) {
    if (command.action != "device.write") return "不支持的命令 action: " + command.action;
    std::string deviceId;
    double functionCodeValue = 0;
    double addressValue = 0;
    double rawValue = 0;
    std::vector<double> requestedValues;
    if (!jsonStringField(command.params, "deviceId", deviceId) ||
        !jsonNumberField(command.params, "functionCode", functionCodeValue) ||
        !jsonNumberField(command.params, "address", addressValue)) {
        return "device.write 参数无效，需要 deviceId/functionCode/address";
    }
    if (functionCodeValue != 5 && functionCodeValue != 6 &&
        functionCodeValue != 15 && functionCodeValue != 16) {
        return "device.write 仅支持 FC05/06/15/16";
    }
    const int functionCode = static_cast<int>(functionCodeValue);
    const bool coil = functionCode == 5 || functionCode == 15;
    const bool multiple = functionCode == 15 || functionCode == 16;
    if (addressValue < 0 || addressValue > 65535 || std::floor(addressValue) != addressValue)
        return "Modbus 地址无效";
    if (multiple) {
        if (!jsonNumberArrayField(command.params, "values", requestedValues))
            return "批量写命令需要 values 数组";
        const size_t maxCount = functionCode == 15 ? 1968 : 123;
        if (requestedValues.size() > maxCount) return "批量写数量超过 Modbus 协议上限";
    } else {
        if (!jsonNumberField(command.params, "value", rawValue)) return "单点写命令缺少数值 value";
        requestedValues.push_back(rawValue);
    }
    const uint16_t address = static_cast<uint16_t>(addressValue);
    if (requestedValues.empty() || static_cast<uint32_t>(address) + requestedValues.size() > 65536u)
        return "写入地址范围超出 Modbus 16 位地址空间";
    std::vector<uint16_t> words;
    std::vector<uint8_t> bits;
    words.reserve(requestedValues.size());
    bits.reserve(requestedValues.size());
    for (size_t i = 0; i < requestedValues.size(); ++i) {
        const double requested = requestedValues[i];
        if (requested < 0 || requested > (coil ? 1 : 65535) || std::floor(requested) != requested)
            return "写入值不符合 Modbus 数据范围";
        const uint16_t currentAddress = static_cast<uint16_t>(address + i);
        const PointType expected = coil ? PointType::YK : PointType::YT;
        auto point = std::find_if(points.begin(), points.end(), [&](const PointTemplate& candidate) {
            return candidate.deviceId == deviceId && candidate.type == expected &&
                   candidate.southAddr <= currentAddress &&
                   static_cast<uint32_t>(candidate.southAddr) + candidate.southCount > currentAddress;
        });
        if (point == points.end()) return "写入范围包含未配置的可写地址: " + std::to_string(currentAddress);
        const uint16_t raw = static_cast<uint16_t>(requested);
        double engineeringValue = raw;
        if (coil) {
            engineeringValue = requested;
            bits.push_back(static_cast<uint8_t>(requested));
        } else {
            if (point->dataType == PointDataType::S16 && raw >= 0x8000)
                engineeringValue = static_cast<int16_t>(raw);
            engineeringValue = engineeringValue * point->scale + point->offset;
            words.push_back(raw);
        }
        if (!std::isfinite(engineeringValue) || engineeringValue < point->minValue ||
            engineeringValue > point->maxValue) return "控制值超出点表限值: " + point->pointId;
        if (!point->interlockPoint.empty()) {
            RTV feedback;
            if (!RealTimeDB::instance().get(point->interlockPoint, feedback) ||
                feedback.quality != Quality::GOOD ||
                std::fabs(feedback.value - point->interlockValue) > 1e-4f)
                return "联锁未满足或反馈无效: " + point->interlockPoint;
        }
    }
    bool ok = false;
    if (multiple && coil) ok = pollerManager.writeDeviceCoils(deviceId, address, bits);
    else if (multiple) ok = pollerManager.writeDeviceRegisters(deviceId, address, words);
    else {
        const PointType expected = coil ? PointType::YK : PointType::YT;
        auto point = std::find_if(points.begin(), points.end(), [&](const PointTemplate& candidate) {
            return candidate.deviceId == deviceId && candidate.type == expected && candidate.southAddr == address;
        });
        const uint16_t raw = coil ? (bits[0] ? 1 : 0) : words[0];
        ok = point != points.end() && pollerManager.writePoint(deviceId, point->pointId, raw);
    }
    if (!ok)
        return "下端设备写入失败或设备离线";
    return "命令已执行写操作；设备实际动作请以反馈遥信/遥测为准";
}

// ============================================================================
// 解析命令行参数
// ============================================================================
/**
 * @brief 解析程序启动参数。
 * @param argc 命令行参数数量。
 * @param argv 命令行参数数组。
 * @param configDir 输出配置目录。
 * @param dumpRtdb 输出是否启用实时库打印。
 * @param updateDeviceNo 输出待修改设备序号。
 * @param updateConfig 输出待写入设备配置。
 * @return 无。
 */
static void parseArgs(int argc, char* argv[],
                      std::string& configDir,
                      std::string& historyDbPath,
                      int& historyIntervalSeconds,
                      int& historyRetentionDays,
                      bool& dumpRtdb,
                      int& updateDeviceNo,
                      DeviceInstance& updateConfig) {
    configDir = "config";
    const char* dbOverride = std::getenv("EMS_DB_FILE");
    historyDbPath = (dbOverride && *dbOverride) ? dbOverride : "data/ems.db";
    historyIntervalSeconds = 60;
    historyRetentionDays = 7;
    dumpRtdb  = false;
    updateDeviceNo = 0;
    updateConfig.type = DeviceType::TCP;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            configDir = argv[++i];
        } else if (std::strcmp(argv[i], "--dump") == 0) {
            dumpRtdb = true;
        } else if (std::strcmp(argv[i], "--db") == 0 && i + 1 < argc) {
            historyDbPath = argv[++i];
        } else if (std::strcmp(argv[i], "--history-interval-sec") == 0 && i + 1 < argc) {
            historyIntervalSeconds = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--history-days") == 0 && i + 1 < argc) {
            historyRetentionDays = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--set-device") == 0 && i + 5 < argc) {
            updateDeviceNo = std::stoi(argv[++i]);
            updateConfig.deviceId = argv[++i];
            std::string type = argv[++i];
            if (type == "TCP") {
                updateConfig.type = DeviceType::TCP;
                updateConfig.ip = argv[++i];
                updateConfig.port = std::stoi(argv[++i]);
            } else if (type == "RTU" && i + 5 < argc) {
                updateConfig.type = DeviceType::RTU;
                updateConfig.serialDev = argv[++i];
                updateConfig.baud = std::stoi(argv[++i]);
                updateConfig.parity = argv[++i][0];
                updateConfig.dataBit = std::stoi(argv[++i]);
                updateConfig.stopBit = (i + 1 < argc) ? std::stoi(argv[++i]) : 1;
            } else {
                updateDeviceNo = -1;
            }
        } else if (std::strcmp(argv[i], "--help") == 0) {
            printf("用法: %s [--config DIR] [--db FILE] [--history-interval-sec N] [--dump]\n", argv[0]);
            printf("  --config DIR   配置目录，默认 config\n");
            printf("  --db FILE      EMS 与网页后端共用的 SQLite 文件，默认 data/ems.db\n");
            printf("  --history-interval-sec N  历史采样周期，默认 60 秒\n");
            printf("  --history-days N 保留天数，默认 7 天\n");
            printf("  --dump         每次轮询后打印实时库\n");
            printf("  --set-device N ID TCP IP PORT\n");
            printf("  --set-device N ID RTU DEV BAUD PARITY DATA_BITS STOP_BITS\n");
            exit(0);
        }
    }
}

// ============================================================================
// main
// ============================================================================
/**
 * @brief 启动 EMS 网关并管理各服务生命周期。
 */
int main(int argc, char* argv[]) {
    // 1. 信号处理
    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);

    // 2. 参数
    std::string configDir;
    std::string historyDbPath;
    int historyIntervalSeconds = 60;
    int historyRetentionDays = 7;
    bool dumpRtdb = false;
    int updateDeviceNo = 0;
    DeviceInstance updateConfig;
    parseArgs(argc, argv, configDir, historyDbPath, historyIntervalSeconds,
              historyRetentionDays, dumpRtdb, updateDeviceNo, updateConfig);
    if (historyIntervalSeconds < 1 || historyIntervalSeconds > 86400 ||
        historyRetentionDays < 1 || historyRetentionDays > 365) {
        std::fprintf(stderr, "历史采样参数超出范围\n");
        return 2;
    }

    // 3. 创建日志目录
    mkdir("logs", 0755);
    if (historyDbPath == "data/ems.db") mkdir("data", 0755);

    // 4. 日志
    Logger::instance().setLevel(LogLevel::DEBUG);
    Logger::instance().setFile("logs/ems.log", 10, 5);
    LOG_INFO("Main", "EMS 启动");
    LOG_INFO("Main", "配置目录: " + configDir);
    LOG_INFO("Database", "EMS 与网页后端共用 SQLite 文件: " + historyDbPath);

    HistoryStore historyStore(historyDbPath, historyRetentionDays);
    if (!historyStore.initialize()) {
        LOG_ERROR("Main", "共享数据库暂不可用；Modbus 采集仍继续，后续会定期重试写入");
    }

    // 5. 加载配置
    std::string pointsFile  = configDir + "/points.conf";
    std::string devicesFile = configDir + "/devices.conf";

    LOG_INFO("Config", "设备和点表配置仅在启动时加载，修改配置后请手动重启服务");

    auto points  = loadPointTemplates(pointsFile);
    auto devices = loadDeviceInstances(devicesFile);

    bool pointReferencesValid = true;
    for (const auto& point : points) {
        if (!point.deviceId.empty() && std::none_of(devices.begin(), devices.end(), [&](const DeviceInstance& d) {
                return d.deviceId == point.deviceId;
            })) {
            LOG_ERROR("Config", "点 " + point.pointId + " 引用了不存在的设备 " + point.deviceId);
            pointReferencesValid = false;
        }
        if (!point.interlockPoint.empty()) {
            const bool exists = std::any_of(points.begin(), points.end(), [&](const PointTemplate& candidate) {
                return candidate.type == PointType::YX &&
                       candidate.deviceId + "." + candidate.pointId == point.interlockPoint;
            });
            if (!exists) {
                LOG_ERROR("Config", "控制点 " + point.pointId + " 的联锁反馈点不存在: " + point.interlockPoint);
                pointReferencesValid = false;
            }
        }
    }

    if (updateDeviceNo != 0) {
        if (updateDeviceNo < 0 || updateDeviceNo > static_cast<int>(devices.size())) {
            LOG_ERROR("Config", "设备修改参数无效");
            return 1;
        }

        updateConfig.slaveId = devices[updateDeviceNo - 1].slaveId;
        if (!updateDeviceConfig(devicesFile, updateDeviceNo, updateConfig)) {
            LOG_ERROR("Config", "设备配置修改失败: Device" +
                                 std::to_string(updateDeviceNo));
            return 1;
        }

        LOG_INFO("Config", "设备配置已修改，程序将使用新配置继续启动");
        devices = loadDeviceInstances(devicesFile);
    }

    if (!pointReferencesValid) {
        LOG_ERROR("Main", "点表引用校验失败，拒绝启动");
        return 1;
    }
    if (points.empty()) {
        LOG_ERROR("Main", "点表为空，退出");
        return 1;
    }
    if (devices.empty()) {
        LOG_ERROR("Main", "无设备，退出");
        return 1;
    }
    // 6. 为每台设备注册独立日志文件
    for (size_t i = 0; i < devices.size(); ++i) {
        std::string tag  = "Device" + std::to_string(i + 1);
        std::string path = "logs/" + tag + ".log";
        Logger::instance().addTagFile(tag, path);
    }

    // 7. 从站（北向）
    ModbusServer server(5020);
    // Northbound addresses come directly from the point table (including
    // article-style addresses such as coil 1000 and holding register 40000).
    // Mapping size is a count, so allocate the full 16-bit Modbus address space.
    if (!server.init(65536, 65536, 65536, 65536)) {
        LOG_ERROR("Main", "从站初始化失败");
        return 1;
    }
    server.attachRealTimeDB(&RealTimeDB::instance());

    server.setPointTable(points);

    // 8. 启动多主站轮询管理器
    PollerManager pollerManager(devices, points);
    if (!pollerManager.start()) {
        LOG_ERROR("Main", "设备轮询管理器启动失败，退出");
        return 1;
    }
    server.setWriteCallback([&](int area, int addr, uint16_t value) {
        const PointType expected = area == 0 ? PointType::YK : PointType::YT;
        auto point = std::find_if(points.begin(), points.end(), [&](const PointTemplate& p) {
            return p.type == expected && p.northAddr == addr;
        });
        if (point == points.end()) {
            LOG_WARN("Main", "北向控制地址未配置: area=" + std::to_string(area) +
                             " addr=" + std::to_string(addr));
            return;
        }
        const float rawEngineering = expected == PointType::YT && point->dataType == PointDataType::S16
            ? static_cast<float>(static_cast<int16_t>(value)) : static_cast<float>(value);
        const float engineeringValue = expected == PointType::YK
            ? (value == 0 ? 0.0f : 1.0f)
            : rawEngineering * point->scale + point->offset;
        if (!std::isfinite(engineeringValue) || engineeringValue < point->minValue ||
            engineeringValue > point->maxValue) {
            LOG_ERROR("Main", "控制值超出点表限值: " + point->pointId +
                              " value=" + std::to_string(engineeringValue));
            return;
        }
        if (!point->interlockPoint.empty()) {
            RTV interlock;
            if (!RealTimeDB::instance().get(point->interlockPoint, interlock) ||
                interlock.quality != Quality::GOOD ||
                std::fabs(interlock.value - point->interlockValue) > 1e-4f) {
                LOG_ERROR("Main", "控制联锁未满足或状态无效: " + point->interlockPoint);
                return;
            }
        }
        // Forward the raw Modbus value unchanged. The same point scale is
        // applied when converting telemetry for northbound reads; applying it
        // here as well would scale a command twice.
        if (!pollerManager.writePoint(point->pointId, value))
            LOG_ERROR("Main", "北向控制转发失败: " + point->pointId);
        else
            LOG_INFO("Main", "控制写请求已发送，等待设备状态确认: " + point->pointId);
    });

    // 北向 Modbus TCP 从站仍保留，供 EMS 主站/调试工具兼容接入。
    std::thread t_server([&]{ server.run(); });
    LOG_INFO("Main", "从站线程已启动");

    // 验证设备信息查询接口，并输出当前设备状态。
    for (const auto& info : pollerManager.getAllDeviceInfo()) {
        LOG_INFO("DeviceQuery", info.deviceNo +
                 " id=" + info.deviceId +
                 " type=" + deviceTypeStr(info.type) +
                 " endpoint=" + info.endpoint +
                 " connected=" + (info.connected ? "true" : "false") +
                 " failCount=" + std::to_string(info.failCount));
    }

    // 9. EMS 与网页后端通过共享 SQLite 交换实时值、历史值和控制命令。
    auto nextSample = std::chrono::steady_clock::now();
    auto nextHistory = nextSample;
    auto nextCommandPoll = nextSample;
    while (g_running) {
        const auto steadyNow = std::chrono::steady_clock::now();
        if (steadyNow >= nextCommandPoll) {
            std::vector<DbCommand> commands;
            if (historyStore.fetchPendingCommands(commands, 20)) {
                const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                for (const auto& command : commands) {
                    const std::string result = command.expiresAtMs <= nowMs
                        ? "命令已过期，未执行"
                        : processCommand(command, points, pollerManager);
                    const std::string status = result.find("命令已执行写操作") == 0 ? "done" : "failed";
                    if (!historyStore.completeCommand(command.commandId, status, result, nowMs))
                        LOG_ERROR("Command", "命令执行后无法写回结果，请人工核对设备状态: " + command.commandId);
                    LOG_INFO("Command", "SQLite 命令 " + command.commandId + " " + status + ": " + result);
                }
            }
            nextCommandPoll = steadyNow + std::chrono::milliseconds(100);
        }
        if (steadyNow >= nextSample) {
            if (dumpRtdb) RealTimeDB::instance().dump();
            const auto sampleTime = std::chrono::system_clock::now();
            const auto sampleTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                sampleTime.time_since_epoch()).count();
            const auto snapshot = RealTimeDB::instance().snapshot();
            const bool writeHistory = steadyNow >= nextHistory;
            historyStore.storeSnapshot(snapshot, sampleTimeMs, writeHistory);
            if (writeHistory) nextHistory = steadyNow + std::chrono::seconds(historyIntervalSeconds);
            nextSample = steadyNow + std::chrono::seconds(1);
        }
        std::this_thread::sleep_until(std::min(nextCommandPoll, nextSample));
    }

    // 10. 优雅退出
    LOG_INFO("Main", "开始退出...");
    server.stop();
    pollerManager.stop();
    if (t_server.joinable()) t_server.join();
    Logger::instance().close();
    LOG_INFO("Main", "EMS 退出");

    return 0;
}
