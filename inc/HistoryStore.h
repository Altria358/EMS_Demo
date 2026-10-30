#ifndef HISTORY_STORE_H
#define HISTORY_STORE_H

#include "RealTimeDB.h"

#include <sqlite3.h>
#include <cstdint>
#include <ctime>
#include <map>
#include <string>
#include <vector>

struct DbCommand {
    std::string commandId;
    std::string action;
    std::string params;
    int64_t issuedAtMs = 0;
    int64_t expiresAtMs = 0;
};

// Shared local SQLite contract for the EMS process and web backend.
class HistoryStore {
public:
    explicit HistoryStore(std::string path, int retentionDays = 7);
    ~HistoryStore();

    HistoryStore(const HistoryStore&) = delete;
    HistoryStore& operator=(const HistoryStore&) = delete;

    bool initialize();
    bool storeSnapshot(const std::map<std::string, RTV>& snapshot,
                       int64_t sampleTimeMs, bool storeHistory);
    bool fetchPendingCommands(std::vector<DbCommand>& commands, int limit = 20);
    bool completeCommand(const std::string& commandId, const std::string& status,
                         const std::string& result, int64_t doneTimeMs);
    void close();

private:
    bool openIfNeeded();
    bool execute(const char* sql);
    void logStorageError(const std::string& message);

    std::string path_;
    int retentionDays_;
    sqlite3* db_ = nullptr;
    std::time_t nextOpenAttempt_ = 0;
    std::time_t nextRetention_ = 0;
    std::time_t nextErrorLog_ = 0;
};

#endif
