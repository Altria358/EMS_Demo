#ifndef MODBUS_CLIENT_H
#define MODBUS_CLIENT_H

#include <cstdint>
#include <string>
#include <vector>
#include <mutex>
#include <modbus/modbus.h> 

// Modbus TCP/RTU 客户端封装。
class ModbusClient {
public:
    // 创建 Modbus TCP 客户端。
    ModbusClient(const std::string& ip, int port = 502);
    // 创建 Modbus RTU 客户端。
    ModbusClient(const std::string& device, int baud, char parity,
                 int dataBit, int stopBit);

    // 释放客户端资源并关闭连接。
    ~ModbusClient();

    ModbusClient(const ModbusClient&) = delete;
    ModbusClient& operator=(const ModbusClient&) = delete;

    // 建立 Modbus 连接。
    bool connect();
    // 关闭 Modbus 连接。
    void disconnect();
    // 获取连接状态。
    bool isConnected() const { return connected_; }

    // 设置从站地址。
    void setSlave(int slaveId);
    // 设置响应超时时间。
    void setResponseTimeout(uint32_t sec, uint32_t usec);
    // 设置字节间超时时间。
    void setByteTimeout(uint32_t sec, uint32_t usec);

    // 读取保持寄存器。
    int readHoldingRegisters(uint16_t addr, uint16_t nb, std::vector<uint16_t>& dest);
    // 读取输入寄存器。
    int readInputRegisters(uint16_t addr, uint16_t nb, std::vector<uint16_t>& dest);
    // 读取线圈状态。
    int readCoils(uint16_t addr, uint16_t nb, std::vector<uint8_t>& dest);
    // 读取离散输入状态。
    int readDiscreteInputs(uint16_t addr, uint16_t nb, std::vector<uint8_t>& dest);

    // 写入单个保持寄存器。
    int writeSingleRegister(uint16_t addr, uint16_t value);
    // 写入多个保持寄存器。
    int writeMultipleRegisters(uint16_t addr, const std::vector<uint16_t>& values);
    // 写入单个线圈。
    int writeSingleCoil(uint16_t addr, bool value);
    // 写入多个线圈。
    int writeMultipleCoils(uint16_t addr, const std::vector<uint8_t>& values);

    // 获取最近一次错误信息。
    const char* lastError() const { return lastErr_.c_str(); }

private:
    modbus_t* ctx_ = nullptr;
    bool connected_ = false;
    std::string lastErr_;

    // 设置客户端错误信息。
    void setError(const std::string& e);
    // 清除客户端错误信息。
    void clearError();
};

#endif