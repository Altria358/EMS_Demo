#include "MqttBridge.h"
#include "Logger.h"

#include <chrono>
#include <cmath>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <utility>

namespace {
std::once_flag g_mosquittoInitFlag;
int g_mosquittoInitResult = -1;

const char* qualityName(Quality quality) {
    switch (quality) {
        case Quality::GOOD: return "good";
        case Quality::BAD: return "bad";
        case Quality::STALE: return "stale";
        case Quality::UNINIT: return "uninitialized";
    }
    return "bad";
}

}

MqttBridge::MqttBridge(std::string host, int port, std::string baseTopic,
                       std::string username, std::string password)
    : host_(std::move(host)), port_(port), baseTopic_(std::move(baseTopic)),
      username_(std::move(username)), password_(std::move(password)) {
    while (!baseTopic_.empty() && baseTopic_.back() == '/') baseTopic_.pop_back();
    while (!baseTopic_.empty() && baseTopic_.front() == '/') baseTopic_.erase(baseTopic_.begin());
    if (baseTopic_.empty()) baseTopic_ = "ems/site1";
    telemetryTopic_ = baseTopic_ + "/telemetry";
    commandTopic_ = baseTopic_ + "/command";
    resultTopic_ = baseTopic_ + "/command/result";
    statusTopic_ = baseTopic_ + "/status";
}

MqttBridge::~MqttBridge() { stop(); }

bool MqttBridge::start() {
    if (started_.load()) return true;
    if (baseTopic_.find_first_of("+#") != std::string::npos) {
        LOG_ERROR("MQTT", "Topic 根路径不能包含通配符 + 或 #");
        return false;
    }
    std::call_once(g_mosquittoInitFlag, [] { g_mosquittoInitResult = mosquitto_lib_init(); });
    if (g_mosquittoInitResult != MOSQ_ERR_SUCCESS) {
        LOG_ERROR("MQTT", "libmosquitto 初始化失败");
        return false;
    }

    std::string clientId = "ems-gateway";
    for (char ch : baseTopic_)
        clientId.push_back((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                           (ch >= '0' && ch <= '9') ? ch : '-');
    client_ = mosquitto_new(clientId.c_str(), false, this);
    if (!client_) {
        LOG_ERROR("MQTT", "创建 MQTT 客户端失败");
        return false;
    }

    mosquitto_connect_callback_set(client_, &MqttBridge::onConnect);
    mosquitto_disconnect_callback_set(client_, &MqttBridge::onDisconnect);
    mosquitto_message_callback_set(client_, &MqttBridge::onMessage);
    mosquitto_reconnect_delay_set(client_, 1, 30, true);
    const char* will = "{\"online\":false}";
    int rc = mosquitto_will_set(client_, statusTopic_.c_str(),
                                static_cast<int>(std::char_traits<char>::length(will)),
                                will, 1, true);
    if (rc == MOSQ_ERR_SUCCESS && !username_.empty())
        rc = mosquitto_username_pw_set(client_, username_.c_str(), password_.c_str());
    if (rc != MOSQ_ERR_SUCCESS) {
        LOG_ERROR("MQTT", "配置 MQTT 客户端失败: " + std::string(mosquitto_strerror(rc)));
        mosquitto_destroy(client_);
        client_ = nullptr;
        return false;
    }

    rc = mosquitto_connect_async(client_, host_.c_str(), port_, 30);
    if (rc == MOSQ_ERR_SUCCESS) rc = mosquitto_loop_start(client_);
    if (rc != MOSQ_ERR_SUCCESS) {
        LOG_ERROR("MQTT", "启动 MQTT 连接失败: " + std::string(mosquitto_strerror(rc)));
        mosquitto_destroy(client_);
        client_ = nullptr;
        return false;
    }
    started_ = true;
    LOG_INFO("MQTT", "连接中 " + host_ + ":" + std::to_string(port_) +
                      " topic=" + baseTopic_);
    return true;
}

void MqttBridge::stop() {
    if (!client_) return;
    // Close the transport without a graceful MQTT DISCONNECT so the broker
    // publishes the configured retained Last Will (offline) status.
    if (started_.exchange(false)) mosquitto_loop_stop(client_, true);
    connected_ = false;
    mosquitto_destroy(client_);
    client_ = nullptr;
}

bool MqttBridge::publish(const std::string& topic, const std::string& payload,
                         int qos, bool retain) {
    if (!client_ || !connected_.load()) return false;
    const int rc = mosquitto_publish(client_, nullptr, topic.c_str(),
                                     static_cast<int>(payload.size()), payload.data(),
                                     qos, retain);
    return rc == MOSQ_ERR_SUCCESS;
}

bool MqttBridge::publishTelemetry(const std::map<std::string, RTV>& snapshot,
                                  int64_t timestampMs) {
    return publish(telemetryTopic_, telemetryJson(snapshot, timestampMs), 0, true);
}

bool MqttBridge::publishCommandResult(const std::string& commandId,
                                      const std::string& status,
                                      const std::string& result,
                                      int64_t timestampMs) {
    std::ostringstream json;
    json << "{\"schema_version\":1,\"type\":\"command_result\",\"command_id\":\""
         << escapeJson(commandId) << "\",\"status\":\"" << escapeJson(status)
         << "\",\"result\":\"" << escapeJson(result)
         << "\",\"timestamp_ms\":" << timestampMs << '}';
    const std::string payload = json.str();
    if (publish(resultTopic_, payload, 1, false)) return true;
    if (!started_.load()) return false;
    std::lock_guard<std::mutex> lock(outboxMutex_);
    if (resultOutbox_.size() >= 512) {
        LOG_ERROR("MQTT", "命令结果暂存队列已满，无法暂存结果");
        return false;
    }
    resultOutbox_.push_back(PendingResult{payload});
    return true;
}

bool MqttBridge::takeCommands(std::vector<std::string>& payloads) {
    payloads.clear();
    std::lock_guard<std::mutex> lock(queueMutex_);
    while (!commandQueue_.empty()) {
        payloads.push_back(std::move(commandQueue_.front()));
        commandQueue_.pop_front();
    }
    return !payloads.empty();
}

std::string MqttBridge::telemetryJson(const std::map<std::string, RTV>& snapshot,
                                      int64_t timestampMs) const {
    std::ostringstream json;
    json << "{\"schema_version\":1,\"type\":\"telemetry\",\"timestamp_ms\":"
         << timestampMs << ",\"points\":[";
    bool first = true;
    for (const auto& item : snapshot) {
        if (!first) json << ',';
        first = false;
        json << "{\"id\":\"" << escapeJson(item.first) << "\",\"value\":";
        if (item.second.quality == Quality::GOOD && std::isfinite(item.second.value))
            json << std::setprecision(8) << item.second.value;
        else
            json << "null";
        json << ",\"quality\":\"" << qualityName(item.second.quality)
             << "\",\"timestamp_ms\":" << timestampMs << '}';
    }
    json << "]}";
    return json.str();
}

std::string MqttBridge::escapeJson(const std::string& value) {
    std::ostringstream escaped;
    for (unsigned char ch : value) {
        switch (ch) {
            case '"': escaped << "\\\""; break;
            case '\\': escaped << "\\\\"; break;
            case '\b': escaped << "\\b"; break;
            case '\f': escaped << "\\f"; break;
            case '\n': escaped << "\\n"; break;
            case '\r': escaped << "\\r"; break;
            case '\t': escaped << "\\t"; break;
            default:
                if (ch < 0x20) escaped << "\\u00" << std::hex << std::setw(2)
                                          << std::setfill('0') << static_cast<int>(ch);
                else escaped << static_cast<char>(ch);
        }
    }
    return escaped.str();
}

void MqttBridge::onConnect(struct mosquitto*, void* userdata, int rc) {
    static_cast<MqttBridge*>(userdata)->handleConnect(rc);
}

void MqttBridge::onDisconnect(struct mosquitto*, void* userdata, int rc) {
    static_cast<MqttBridge*>(userdata)->handleDisconnect(rc);
}

void MqttBridge::onMessage(struct mosquitto*, void* userdata,
                           const struct mosquitto_message* message) {
    static_cast<MqttBridge*>(userdata)->handleMessage(message);
}

void MqttBridge::handleConnect(int rc) {
    if (rc != 0) {
        connected_ = false;
        LOG_WARN("MQTT", "连接 Broker 被拒绝，错误码=" + std::to_string(rc));
        return;
    }
    connected_ = true;
    const int subRc = mosquitto_subscribe(client_, nullptr, commandTopic_.c_str(), 1);
    if (subRc != MOSQ_ERR_SUCCESS)
        LOG_ERROR("MQTT", "订阅控制命令失败: " + std::string(mosquitto_strerror(subRc)));
    publish(statusTopic_, "{\"online\":true}", 1, true);
    flushResults();
    LOG_INFO("MQTT", "Broker 已连接，已订阅 " + commandTopic_);
}

void MqttBridge::flushResults() {
    std::lock_guard<std::mutex> lock(outboxMutex_);
    while (!resultOutbox_.empty()) {
        if (!publish(resultTopic_, resultOutbox_.front().payload, 1, false)) break;
        resultOutbox_.pop_front();
    }
}

void MqttBridge::handleDisconnect(int rc) {
    connected_ = false;
    if (started_.load())
        LOG_WARN("MQTT", "Broker 连接断开，客户端将自动重连，错误码=" + std::to_string(rc));
}

void MqttBridge::handleMessage(const struct mosquitto_message* message) {
    if (!message || !message->topic || commandTopic_ != message->topic ||
        message->payloadlen <= 0 || message->payloadlen > 65536) return;
    const char* bytes = static_cast<const char*>(message->payload);
    std::string payload(bytes, static_cast<size_t>(message->payloadlen));
    std::lock_guard<std::mutex> lock(queueMutex_);
    if (commandQueue_.size() >= 256) {
        LOG_ERROR("MQTT", "控制命令本地队列已满，拒绝新消息");
        return;
    }
    commandQueue_.push_back(std::move(payload));
}
