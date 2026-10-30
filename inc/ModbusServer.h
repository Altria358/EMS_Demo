#ifndef MODBUS_SERVER_H
#define MODBUS_SERVER_H

#include <modbus/modbus.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <set>
#include "PointTemplate.h"

class RealTimeDB;

// 北向 Modbus TCP 从站，供上位机读写 EMS 数据。
class ModbusServer {
public:
    // 上位机写入回调：area 0=线圈，1=保持寄存器。
    using WriteCallback = std::function<void(int area, int addr, uint16_t value)>;

    // 创建北向 Modbus TCP 从站。
    ModbusServer(int port, int maxClients = 4);

    // 停止服务并释放 Modbus 资源。
    ~ModbusServer();

    // 初始化 Modbus 数据映射区。
    bool init(int nBits, int nInputBits, int nRegs, int nInputRegs);

    // 关联实时数据库。
    void attachRealTimeDB(RealTimeDB* rtdb);
    void setPointTable(const std::vector<PointTemplate>& points) { points_ = points; }

    // 设置上位机写入回调。
    void setWriteCallback(WriteCallback cb) { writeCb_ = cb; }

    // 运行阻塞式 Modbus TCP 服务循环。
    void run();

    // 请求停止 Modbus TCP 服务。
    void stop();

    // 获取 Server 运行状态。
    bool isRunning() const { return running_; }

private:
    void handleClient(int clientFd);
    void handleWriteRequest(const uint8_t* query, int length);
    // 将实时库数据同步到输入寄存器映射区。
    void syncFromRealTimeDB();

    modbus_t* ctx_ = nullptr;
    modbus_mapping_t* map_ = nullptr;
    int port_;
    int maxClients_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopRequested_{false};
    std::atomic<int> listenFd_{-1};
    std::atomic<size_t> activeWorkers_{0};
    mutable std::mutex clientsMutex_;
    std::condition_variable workersCv_;
    std::set<int> activeClientFds_;
    std::mutex mapMutex_;
    RealTimeDB* rtdb_ = nullptr;
    WriteCallback writeCb_;
    std::vector<PointTemplate> points_;
};

#endif
