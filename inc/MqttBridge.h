#ifndef MQTT_BRIDGE_H
#define MQTT_BRIDGE_H

#include "RealTimeDB.h"

#include <mosquitto.h>
#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

class MqttBridge {
public:
    MqttBridge(std::string host, int port, std::string baseTopic,
               std::string username = {}, std::string password = {});
    ~MqttBridge();

    MqttBridge(const MqttBridge&) = delete;
    MqttBridge& operator=(const MqttBridge&) = delete;

    bool start();
    void stop();
    bool connected() const { return connected_.load(); }
    bool publishTelemetry(const std::map<std::string, RTV>& snapshot,
                          int64_t timestampMs);
    bool publishCommandResult(const std::string& commandId,
                              const std::string& status,
                              const std::string& result,
                              int64_t timestampMs);
    bool takeCommands(std::vector<std::string>& payloads);

private:
    struct PendingResult {
        std::string payload;
    };
    static void onConnect(struct mosquitto* client, void* userdata, int rc);
    static void onDisconnect(struct mosquitto* client, void* userdata, int rc);
    static void onMessage(struct mosquitto* client, void* userdata,
                          const struct mosquitto_message* message);
    void handleConnect(int rc);
    void handleDisconnect(int rc);
    void handleMessage(const struct mosquitto_message* message);
    bool publish(const std::string& topic, const std::string& payload,
                 int qos, bool retain);
    void flushResults();
    std::string telemetryJson(const std::map<std::string, RTV>& snapshot,
                              int64_t timestampMs) const;
    static std::string escapeJson(const std::string& value);

    std::string host_;
    int port_;
    std::string baseTopic_;
    std::string telemetryTopic_;
    std::string commandTopic_;
    std::string resultTopic_;
    std::string statusTopic_;
    std::string username_;
    std::string password_;
    struct mosquitto* client_ = nullptr;
    std::atomic<bool> connected_{false};
    std::atomic<bool> started_{false};
    std::mutex queueMutex_;
    std::deque<std::string> commandQueue_;
    std::mutex outboxMutex_;
    std::deque<PendingResult> resultOutbox_;
};

#endif
