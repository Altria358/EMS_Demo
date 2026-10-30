#include "ModbusServer.h"
#include "RealTimeDB.h"
#include "Logger.h"

#include <cstring>
#include <thread>
#include <chrono>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cerrno>
#include <algorithm>
#include <thread>

// ============================================================================
// 构造 / 析构
// ============================================================================

/**
 * @brief 创建北向 Modbus TCP 从站。
 * @param port 监听端口。
 * @param maxClients 最大客户端连接数。
 * @return 无。
 */
ModbusServer::ModbusServer(int port, int maxClients)
    : port_(port), maxClients_(maxClients) {
}
/**
 * @brief 停止服务并释放 Modbus 资源。
 * @return 无。
 */
ModbusServer::~ModbusServer() {
    stop();
    if (map_) {
        modbus_mapping_free(map_);
        map_ = nullptr;
    }
    if (ctx_) {
        modbus_close(ctx_);
        modbus_free(ctx_);
        ctx_ = nullptr;
    }
}

// ============================================================================
// 初始化
// ============================================================================

/**
 * @brief 创建 Modbus 上下文和数据映射区。
 * @param nBits 线圈数量。
 * @param nInputBits 离散输入数量。
 * @param nRegs 保持寄存器数量。
 * @param nInputRegs 输入寄存器数量。
 * @return 初始化成功返回 true，否则返回 false。
 */
bool ModbusServer::init(int nBits, int nInputBits,
                        int nRegs, int nInputRegs) {
    ctx_ = modbus_new_tcp("0.0.0.0", port_);
    if (!ctx_) {
        LOG_ERROR("Server", "modbus_new_tcp 失败");
        return false;
    }

    map_ = modbus_mapping_new(nBits, nInputBits, nRegs, nInputRegs);
    if (!map_) {
        LOG_ERROR("Server", "modbus_mapping_new 失败");
        modbus_free(ctx_);
        ctx_ = nullptr;
        return false;
    }

    LOG_INFO("Server", "从站初始化完成，端口 " + std::to_string(port_));
    return true;
}

/**
 * @brief 关联实时数据库。
 * @param rtdb 实时数据库指针。
 * @return 无。
 */
void ModbusServer::attachRealTimeDB(RealTimeDB* rtdb) {
    rtdb_ = rtdb;
}

// ============================================================================
// 运行
// ============================================================================

/**
 * @brief 运行阻塞式 Modbus TCP 服务循环。
 * @return 无。
 */
void ModbusServer::run() {
    if (!ctx_ || !map_) {
        LOG_ERROR("Server", "未初始化");
        return;
    }
    if (stopRequested_) return;

    running_ = true;
    int s = modbus_tcp_listen(ctx_, maxClients_);
    if (s == -1) {
        LOG_ERROR("Server", "监听失败");
        running_ = false;
        return;
    }

    listenFd_ = s;
    if (stopRequested_) {
        running_ = false;
        listenFd_ = -1;
        modbus_set_socket(ctx_, -1);
        close(s);
        return;
    }
    LOG_INFO("Server", "开始监听端口 " + std::to_string(port_));

    while (running_) {
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(s, &readSet);

        timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        int ready = select(s + 1, &readSet, nullptr, nullptr, &tv);
        if (ready < 0) {
            if (errno == EINTR) continue;
            LOG_ERROR("Server", "select 失败");
            break;
        }
        if (ready == 0) {
            continue;   // 超时，回到 while 检查 running_
        }

        sockaddr_in peer{};
        socklen_t peerLength = sizeof(peer);
        const int client = accept(s, reinterpret_cast<sockaddr*>(&peer), &peerLength);
        if (client == -1) {
            if (!running_ || errno == EINTR) continue;
            LOG_WARN("Server", "接受客户端连接失败: " + std::string(std::strerror(errno)));
            continue;
        }

        bool atCapacity = false;
        size_t clientCount = 0;
        {
            std::lock_guard<std::mutex> lock(clientsMutex_);
            atCapacity = activeClientFds_.size() >= static_cast<size_t>(maxClients_);
            if (!atCapacity) activeClientFds_.insert(client);
            clientCount = activeClientFds_.size();
        }
        if (atCapacity) {
            LOG_WARN("Server", "并发客户端已达上限，拒绝 " + std::string(inet_ntoa(peer.sin_addr)));
            close(client);
            continue;
        }

        activeWorkers_.fetch_add(1);
        try {
            std::thread(&ModbusServer::handleClient, this, client).detach();
        } catch (...) {
            {
                std::lock_guard<std::mutex> lock(clientsMutex_);
                activeClientFds_.erase(client);
            }
            activeWorkers_.fetch_sub(1);
            close(client);
            LOG_ERROR("Server", "创建客户端工作线程失败");
            continue;
        }
        LOG_INFO("Server", "主机已连接 " + std::string(inet_ntoa(peer.sin_addr)) +
                           "，当前并发 " + std::to_string(clientCount));
    }

    running_ = false;
    listenFd_ = -1;
    modbus_set_socket(ctx_, -1);
    close(s);
    {
        std::lock_guard<std::mutex> lock(clientsMutex_);
        for (int fd : activeClientFds_) shutdown(fd, SHUT_RDWR);
    }
    std::unique_lock<std::mutex> lock(clientsMutex_);
    workersCv_.wait(lock, [this] { return activeWorkers_.load() == 0; });
    LOG_INFO("Server", "从站停止");
}

void ModbusServer::handleClient(int clientFd) {
    modbus_t* clientCtx = modbus_new_tcp("0.0.0.0", port_);
    if (!clientCtx) {
        LOG_ERROR("Server", "创建客户端 Modbus 上下文失败");
        {
            std::lock_guard<std::mutex> lock(clientsMutex_);
            activeClientFds_.erase(clientFd);
        }
        close(clientFd);
        if (activeWorkers_.fetch_sub(1) == 1) workersCv_.notify_all();
        return;
    }
    modbus_set_socket(clientCtx, clientFd);
    modbus_set_byte_timeout(clientCtx, 1, 0);

    uint8_t query[MODBUS_TCP_MAX_ADU_LENGTH];
    while (running_) {
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(clientFd, &readSet);
        timeval timeout{};
        timeout.tv_sec = 1;
        const int ready = select(clientFd + 1, &readSet, nullptr, nullptr, &timeout);
        if (ready < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (ready == 0) continue;

        const int rc = modbus_receive(clientCtx, query);
        if (rc <= 0) break;

        syncFromRealTimeDB();
        {
            std::lock_guard<std::mutex> lock(mapMutex_);
            if (modbus_reply(clientCtx, query, rc, map_) == -1) break;
        }
        handleWriteRequest(query, rc);
    }

    {
        std::lock_guard<std::mutex> lock(clientsMutex_);
        activeClientFds_.erase(clientFd);
    }
    modbus_close(clientCtx);
    modbus_free(clientCtx);
    LOG_INFO("Server", "主机断开");
    if (activeWorkers_.fetch_sub(1) == 1) workersCv_.notify_all();
}

void ModbusServer::handleWriteRequest(const uint8_t* query, int length) {
    if (!writeCb_ || length < 12) return;
    const int function = query[7];
    const int start = (static_cast<int>(query[8]) << 8) | query[9];
    if (function == 5 || function == 6) {
        const int area = function == 5 ? 0 : 1;
        const uint16_t value = static_cast<uint16_t>((query[10] << 8) | query[11]);
        LOG_INFO("Server", std::string("主机写") + (area == 0 ? "线圈[" : "寄存器[") +
                           std::to_string(start) + "] = " + std::to_string(value));
        writeCb_(area, start, value);
    } else if (function == 15 && length >= 13) {
        const int quantity = (static_cast<int>(query[10]) << 8) | query[11];
        const int byteCount = query[12];
        if (quantity <= 0 || byteCount < (quantity + 7) / 8 || length < 13 + byteCount) return;
        for (int i = 0; i < quantity; ++i) {
            const uint16_t value = (query[13 + i / 8] & (1u << (i % 8))) ? 0xFF00 : 0;
            LOG_INFO("Server", "主机写线圈[" + std::to_string(start + i) + "] = " +
                               std::to_string(value != 0));
            writeCb_(0, start + i, value);
        }
    } else if (function == 16 && length >= 13) {
        const int quantity = (static_cast<int>(query[10]) << 8) | query[11];
        const int byteCount = query[12];
        if (quantity <= 0 || byteCount < quantity * 2 || length < 13 + byteCount) return;
        for (int i = 0; i < quantity; ++i) {
            const uint16_t value = static_cast<uint16_t>((query[13 + i * 2] << 8) |
                                                         query[14 + i * 2]);
            LOG_INFO("Server", "主机写寄存器[" + std::to_string(start + i) + "] = " +
                               std::to_string(value));
            writeCb_(1, start + i, value);
        }
    }
}

/**
 * @brief 请求停止 Modbus TCP 服务。
 * @return 无。
 */
void ModbusServer::stop() {
    stopRequested_ = true;
    running_ = false;
    const int listener = listenFd_.load();
    if (listener >= 0) shutdown(listener, SHUT_RDWR);
    std::lock_guard<std::mutex> lock(clientsMutex_);
    for (int fd : activeClientFds_) shutdown(fd, SHUT_RDWR);
}

// ============================================================================
// 同步实时库 → 映射
// ============================================================================

/**
 * @brief 将实时库数据同步到输入寄存器映射区。
 * @return 无。
 */
void ModbusServer::syncFromRealTimeDB() {
    if (!rtdb_ || !map_) return;

    auto snap = rtdb_->snapshot();
    std::fill(map_->tab_input_registers,
              map_->tab_input_registers + map_->nb_input_registers, 0);
    std::fill(map_->tab_input_bits,
              map_->tab_input_bits + map_->nb_input_bits, 0);
    for (const auto& point : points_) {
        const std::string suffix = "." + point.pointId;
        auto it = std::find_if(snap.begin(), snap.end(), [&](const auto& kv) {
            return kv.first.size() >= suffix.size() &&
                   kv.first.compare(kv.first.size() - suffix.size(), suffix.size(), suffix) == 0;
        });
        if (it == snap.end() || it->second.quality != Quality::GOOD) continue;
        if (point.type == PointType::YC && point.northAddr < map_->nb_input_registers) {
            const float raw = (it->second.value - point.offset) / point.scale;
            map_->tab_input_registers[point.northAddr] = static_cast<uint16_t>(std::max(0.0f, std::min(raw, 65535.0f)));
        } else if (point.type == PointType::YX && point.northAddr < map_->nb_input_bits) {
            map_->tab_input_bits[point.northAddr] = it->second.value != 0.0f;
        }
    }
}
