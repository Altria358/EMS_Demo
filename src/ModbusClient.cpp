#include "ModbusClient.h"
#include <cerrno>
#include <cstring>

// ============================================================================
// 构造 / 析构
// ============================================================================

/**
 * @brief 创建 Modbus TCP 客户端。
 * @param ip 远端设备 IP 地址。
 * @param port 远端 TCP 端口。
 * @return 无。
 */
ModbusClient::ModbusClient(const std::string& ip, int port) {
    ctx_ = modbus_new_tcp(ip.c_str(), port);
    if (!ctx_) {
        lastErr_ = "modbus_new_tcp 失败";
    }
}

/**
 * @brief 创建 Modbus RTU 客户端。
 * @param device 串口设备路径。
 * @param baud 波特率。
 * @param parity 校验方式。
 * @param dataBit 数据位数量。
 * @param stopBit 停止位数量。
 * @return 无。
 */
ModbusClient::ModbusClient(const std::string& device, int baud, char parity,
                           int dataBit, int stopBit) {
    ctx_ = modbus_new_rtu(device.c_str(), baud, parity, dataBit, stopBit);
    if (!ctx_) {
        lastErr_ = "modbus_new_rtu 失败";
    }
}

/**
 * @brief 关闭连接并释放 Modbus 上下文。
 * @return 无。
 */
ModbusClient::~ModbusClient() {
    if (ctx_) {
        // modbus_connect() can fail after allocating/opening a socket. Close
        // unconditionally; libmodbus safely ignores an unset socket.
        modbus_close(ctx_);
        modbus_free(ctx_);
        ctx_ = nullptr;
    }
}

// ============================================================================
// 错误处理
// ============================================================================

/**
 * @brief 保存最近一次错误信息。
 * @param e 错误信息。
 * @return 无。
 */
void ModbusClient::setError(const std::string& e) {
    lastErr_ = e;
}

/**
 * @brief 清除最近一次错误信息。
 * @return 无。
 */
void ModbusClient::clearError() {
    lastErr_.clear();
}

// ============================================================================
// 连接管理
// ============================================================================

/**
 * @brief 建立 Modbus 连接。
 * @return 连接成功返回 true，否则返回 false。
 */
bool ModbusClient::connect() {
    if (!ctx_) {
        setError("上下文未创建");
        return false;
    }
    if (connected_) return true;

    if (modbus_connect(ctx_) == -1) {
        setError(std::string("连接失败: ") + modbus_strerror(errno));
        modbus_close(ctx_);
        return false;
    }
    connected_ = true;
    clearError();
    return true;
}

/**
 * @brief 关闭 Modbus 连接。
 * @return 无。
 */
void ModbusClient::disconnect() {
    if (ctx_ && connected_) {
        modbus_close(ctx_);
        connected_ = false;
    }
}

// ============================================================================
// 配置
// ============================================================================

/**
 * @brief 设置 Modbus 从站地址。
 * @param slaveId Modbus 从站地址。
 * @return 无。
 */
void ModbusClient::setSlave(int slaveId) {
    if (ctx_) modbus_set_slave(ctx_, slaveId);
}

/**
 * @brief 设置响应超时时间。
 * @param sec 秒数。
 * @param usec 微秒数。
 * @return 无。
 */
void ModbusClient::setResponseTimeout(uint32_t sec, uint32_t usec) {
    if (ctx_) modbus_set_response_timeout(ctx_, sec, usec);
}

/**
 * @brief 设置字节间超时时间。
 * @param sec 秒数。
 * @param usec 微秒数。
 * @return 无。
 */
void ModbusClient::setByteTimeout(uint32_t sec, uint32_t usec) {
#ifdef LIBMODBUS_VERSION_CHECK
    if (ctx_) modbus_set_byte_timeout(ctx_, sec, usec);
#endif
}

// ============================================================================
// 读操作
// ============================================================================

/**
 * @brief 读取保持寄存器。
 * @param addr 起始地址。
 * @param nb 读取数量。
 * @param dest 接收数据的输出容器。
 * @return 成功读取数量，失败返回 -1。
 */
int ModbusClient::readHoldingRegisters(uint16_t addr, uint16_t nb,
                                       std::vector<uint16_t>& dest) {
    if (!ctx_) { setError("上下文未创建"); return -1; }
    if (!connected_) { setError("未连接"); return -1; }

    dest.resize(nb);
    int rc = modbus_read_registers(ctx_, addr, nb, dest.data());
    if (rc != nb) {
        setError(std::string("读保持寄存器失败: ") + modbus_strerror(errno));
        dest.clear();
        return -1;
    }
    clearError();
    return rc;
}

/**
 * @brief 读取输入寄存器。
 * @param addr 起始地址。
 * @param nb 读取数量。
 * @param dest 接收数据的输出容器。
 * @return 成功读取数量，失败返回 -1。
 */
int ModbusClient::readInputRegisters(uint16_t addr, uint16_t nb,
                                     std::vector<uint16_t>& dest) {
    if (!ctx_) { setError("上下文未创建"); return -1; }
    if (!connected_) { setError("未连接"); return -1; }

    dest.resize(nb);
    int rc = modbus_read_input_registers(ctx_, addr, nb, dest.data());
    if (rc != nb) {
        setError(std::string("读输入寄存器失败: ") + modbus_strerror(errno));
        dest.clear();
        return -1;
    }
    clearError();
    return rc;
}

/**
 * @brief 读取线圈状态。
 * @param addr 起始地址。
 * @param nb 读取数量。
 * @param dest 接收数据的输出容器。
 * @return 成功读取数量，失败返回 -1。
 */
int ModbusClient::readCoils(uint16_t addr, uint16_t nb,
                            std::vector<uint8_t>& dest) {
    if (!ctx_) { setError("上下文未创建"); return -1; }
    if (!connected_) { setError("未连接"); return -1; }

    dest.resize(nb);
    int rc = modbus_read_bits(ctx_, addr, nb, dest.data());
    if (rc != nb) {
        setError(std::string("读线圈失败: ") + modbus_strerror(errno));
        dest.clear();
        return -1;
    }
    clearError();
    return rc;
}

/**
 * @brief 读取离散输入状态。
 * @param addr 起始地址。
 * @param nb 读取数量。
 * @param dest 接收数据的输出容器。
 * @return 成功读取数量，失败返回 -1。
 */
int ModbusClient::readDiscreteInputs(uint16_t addr, uint16_t nb,
                                     std::vector<uint8_t>& dest) {
    if (!ctx_) { setError("上下文未创建"); return -1; }
    if (!connected_) { setError("未连接"); return -1; }

    dest.resize(nb);
    int rc = modbus_read_input_bits(ctx_, addr, nb, dest.data());
    if (rc != nb) {
        setError(std::string("读离散输入失败: ") + modbus_strerror(errno));
        dest.clear();
        return -1;
    }
    clearError();
    return rc;
}

// ============================================================================
// 写操作
// ============================================================================

/**
 * @brief 写入单个保持寄存器。
 * @param addr 寄存器地址。
 * @param value 要写入的值。
 * @return 成功返回 1，失败返回 -1。
 */
int ModbusClient::writeSingleRegister(uint16_t addr, uint16_t value) {
    if (!ctx_) { setError("上下文未创建"); return -1; }
    if (!connected_) { setError("未连接"); return -1; }

    int rc = modbus_write_register(ctx_, addr, value);
    if (rc != 1) {
        setError(std::string("写单寄存器失败: ") + modbus_strerror(errno));
        return -1;
    }
    clearError();
    return rc;
}

/**
 * @brief 写入多个保持寄存器。
 * @param addr 起始地址。
 * @param values 要写入的寄存器值列表。
 * @return 成功写入数量，失败返回 -1。
 */
int ModbusClient::writeMultipleRegisters(uint16_t addr,
                                         const std::vector<uint16_t>& values) {
    if (!ctx_) { setError("上下文未创建"); return -1; }
    if (!connected_) { setError("未连接"); return -1; }
    if (values.empty()) { setError("写入数据为空"); return -1; }

    int rc = modbus_write_registers(ctx_, addr,
                                    static_cast<int>(values.size()),
                                    values.data());
    if (rc != static_cast<int>(values.size())) {
        setError(std::string("写多寄存器失败: ") + modbus_strerror(errno));
        return -1;
    }
    clearError();
    return rc;
}

/**
 * @brief 写入单个线圈。
 * @param addr 线圈地址。
 * @param value 要写入的线圈值。
 * @return 成功返回 1，失败返回 -1。
 */
int ModbusClient::writeSingleCoil(uint16_t addr, bool value) {
    if (!ctx_) { setError("上下文未创建"); return -1; }
    if (!connected_) { setError("未连接"); return -1; }

    int rc = modbus_write_bit(ctx_, addr, value ? 1 : 0);
    if (rc != 1) {
        setError(std::string("写单线圈失败: ") + modbus_strerror(errno));
        return -1;
    }
    clearError();
    return rc;
}

/**
 * @brief 写入多个线圈。
 * @param addr 起始地址。
 * @param values 要写入的线圈值列表。
 * @return 成功写入数量，失败返回 -1。
 */
int ModbusClient::writeMultipleCoils(uint16_t addr,
                                     const std::vector<uint8_t>& values) {
    if (!ctx_) { setError("上下文未创建"); return -1; }
    if (!connected_) { setError("未连接"); return -1; }
    if (values.empty()) { setError("写入数据为空"); return -1; }

    int rc = modbus_write_bits(ctx_, addr,
                               static_cast<int>(values.size()),
                               values.data());
    if (rc != static_cast<int>(values.size())) {
        setError(std::string("写多线圈失败: ") + modbus_strerror(errno));
        return -1;
    }
    clearError();
    return rc;
}
