#include "HistoryStore.h"
#include "Logger.h"

#include <algorithm>
#include <utility>

namespace {
const std::time_t kRetryOpenSeconds = 30;
const std::time_t kRetentionCheckSeconds = 3600;
}

HistoryStore::HistoryStore(std::string path, int retentionDays)
    : path_(std::move(path)), retentionDays_(std::max(1, std::min(retentionDays, 365))) {}

HistoryStore::~HistoryStore() { close(); }

bool HistoryStore::initialize() { return openIfNeeded(); }

bool HistoryStore::openIfNeeded() {
    if (db_) return true;
    const std::time_t now = std::time(nullptr);
    if (now < nextOpenAttempt_) return false;
    nextOpenAttempt_ = now + kRetryOpenSeconds;

    sqlite3* opened = nullptr;
    const int rc = sqlite3_open_v2(path_.c_str(), &opened,
                                   SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                                   nullptr);
    if (rc != SQLITE_OK) {
        const std::string error = opened ? sqlite3_errmsg(opened) : sqlite3_errstr(rc);
        if (opened) sqlite3_close(opened);
        logStorageError("打开历史数据库失败: " + path_ + ": " + error);
        return false;
    }
    db_ = opened;
    sqlite3_busy_timeout(db_, 3000);
    const char* schema =
        "PRAGMA journal_mode=WAL;"
        "PRAGMA synchronous=NORMAL;"
        "CREATE TABLE IF NOT EXISTS history ("
        "tag TEXT NOT NULL,ts INTEGER NOT NULL,value REAL NOT NULL,"
        "PRIMARY KEY(tag,ts)) WITHOUT ROWID;"
        "CREATE INDEX IF NOT EXISTS idx_history_tag_ts ON history(tag,ts);"
        "CREATE TABLE IF NOT EXISTS command_history ("
        "command_id TEXT PRIMARY KEY,ts INTEGER NOT NULL,done_ts INTEGER,"
        "point_id TEXT NOT NULL DEFAULT '',value REAL NOT NULL DEFAULT 0,"
        "status TEXT NOT NULL,result TEXT);"
        "CREATE INDEX IF NOT EXISTS idx_command_history_ts ON command_history(ts);";
    if (!execute(schema)) {
        sqlite3_close(db_);
        db_ = nullptr;
        nextOpenAttempt_ = now + kRetryOpenSeconds;
        return false;
    }
    LOG_INFO("History", "SQLite 历史库已连接（WAL）: " + path_ +
                         "，保留 " + std::to_string(retentionDays_) + " 天");
    return true;
}

bool HistoryStore::execute(const char* sql) {
    char* error = nullptr;
    const int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &error);
    if (rc == SQLITE_OK) return true;
    const std::string message = error ? error : sqlite3_errmsg(db_);
    sqlite3_free(error);
    logStorageError("SQLite 执行失败: " + message);
    return false;
}

bool HistoryStore::storeSnapshot(const std::map<std::string, RTV>& snapshot,
                                 int64_t sampleTimeMs) {
    if (!openIfNeeded() || !execute("BEGIN IMMEDIATE;")) return false;
    sqlite3_stmt* stmt = nullptr;
    const char* insertSql = "INSERT OR REPLACE INTO history(tag,ts,value) VALUES(?,?,?);";
    if (sqlite3_prepare_v2(db_, insertSql, -1, &stmt, nullptr) != SQLITE_OK) {
        execute("ROLLBACK;");
        logStorageError("准备历史采样写入失败: " + std::string(sqlite3_errmsg(db_)));
        return false;
    }

    bool ok = true;
    for (const auto& item : snapshot) {
        if (item.second.quality != Quality::GOOD) continue;
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
        sqlite3_bind_text(stmt, 1, item.first.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 2, sampleTimeMs);
        sqlite3_bind_double(stmt, 3, item.second.value);
        if (sqlite3_step(stmt) != SQLITE_DONE) {
            ok = false;
            break;
        }
    }
    sqlite3_finalize(stmt);
    if (!ok) {
        logStorageError("写入历史采样失败: " + std::string(sqlite3_errmsg(db_)));
        execute("ROLLBACK;");
        return false;
    }

    if (std::time(nullptr) >= nextRetention_) {
        const int64_t cutoffMs = sampleTimeMs -
            static_cast<int64_t>(retentionDays_) * 24 * 60 * 60 * 1000;
        if (!execute(("DELETE FROM history WHERE ts < " + std::to_string(cutoffMs) + ";").c_str())) {
            execute("ROLLBACK;");
            return false;
        }
        const int64_t commandCutoffMs = sampleTimeMs - 30LL * 24 * 60 * 60 * 1000;
        if (!execute(("DELETE FROM command_history WHERE ts < " +
                      std::to_string(commandCutoffMs) + ";").c_str())) {
            execute("ROLLBACK;");
            return false;
        }
        nextRetention_ = std::time(nullptr) + kRetentionCheckSeconds;
    }
    if (!execute("COMMIT;")) {
        execute("ROLLBACK;");
        return false;
    }
    return true;
}

bool HistoryStore::findCommandResult(const std::string& commandId,
                                     std::string& status, std::string& result) {
    status.clear();
    result.clear();
    if (!openIfNeeded()) return false;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_,
            "SELECT status,result FROM command_history WHERE command_id=?;",
            -1, &stmt, nullptr) != SQLITE_OK) {
        logStorageError("准备命令去重查询失败: " + std::string(sqlite3_errmsg(db_)));
        return false;
    }
    sqlite3_bind_text(stmt, 1, commandId.c_str(), -1, SQLITE_TRANSIENT);
    const int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        const unsigned char* statusText = sqlite3_column_text(stmt, 0);
        const unsigned char* resultText = sqlite3_column_text(stmt, 1);
        status = statusText ? reinterpret_cast<const char*>(statusText) : "";
        result = resultText ? reinterpret_cast<const char*>(resultText) : "";
        if (status != "done" && status != "failed") {
            status.clear();
            result.clear();
        }
    }
    sqlite3_finalize(stmt);
    if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
        logStorageError("查询命令去重记录失败: " + std::string(sqlite3_errmsg(db_)));
        return false;
    }
    return true;
}

bool HistoryStore::recordCommandResult(const std::string& commandId,
                                       const std::string& status,
                                       const std::string& result,
                                       int64_t doneTimeMs) {
    if (commandId.empty() || (status != "done" && status != "failed") || !openIfNeeded())
        return false;
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "INSERT INTO command_history(command_id,ts,done_ts,point_id,value,status,result) "
        "VALUES(?,?,?,'',0,?,?) ON CONFLICT(command_id) DO UPDATE SET "
        "done_ts=excluded.done_ts,status=excluded.status,result=excluded.result;";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        logStorageError("准备命令去重记录失败: " + std::string(sqlite3_errmsg(db_)));
        return false;
    }
    sqlite3_bind_text(stmt, 1, commandId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, doneTimeMs);
    sqlite3_bind_int64(stmt, 3, doneTimeMs);
    sqlite3_bind_text(stmt, 4, status.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, result.c_str(), -1, SQLITE_TRANSIENT);
    const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    if (!ok) logStorageError("写入命令去重记录失败: " + std::string(sqlite3_errmsg(db_)));
    return ok;
}

void HistoryStore::logStorageError(const std::string& message) {
    const std::time_t now = std::time(nullptr);
    if (now >= nextErrorLog_) {
        LOG_ERROR("History", message);
        nextErrorLog_ = now + 30;
    }
}

void HistoryStore::close() {
    if (!db_) return;
    sqlite3_wal_checkpoint_v2(db_, nullptr, SQLITE_CHECKPOINT_PASSIVE, nullptr, nullptr);
    sqlite3_close(db_);
    db_ = nullptr;
}
