#include "HistoryStore.h"
#include "Logger.h"

#include <algorithm>
#include <utility>

namespace {
const std::time_t kRetryOpenSeconds = 5;
const std::time_t kRetentionCheckSeconds = 3600;

int sqlQuality(Quality quality) {
    switch (quality) {
    case Quality::GOOD: return 1;
    case Quality::BAD: return 0;
    case Quality::STALE: return 2;
    case Quality::UNINIT: return 3;
    }
    return 0;
}
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
        logStorageError("打开共享 SQLite 失败: " + path_ + ": " + error);
        return false;
    }
    db_ = opened;
    sqlite3_busy_timeout(db_, 3000);
    const char* schema =
        "PRAGMA journal_mode=WAL;"
        "PRAGMA synchronous=NORMAL;"
        "PRAGMA wal_autocheckpoint=1000;"
        "CREATE TABLE IF NOT EXISTS realtime ("
        "tag TEXT PRIMARY KEY,value REAL NOT NULL,ts INTEGER NOT NULL,"
        "quality INTEGER NOT NULL DEFAULT 1 CHECK(quality BETWEEN 0 AND 3)) WITHOUT ROWID;"
        "CREATE TABLE IF NOT EXISTS history ("
        "tag TEXT NOT NULL,ts INTEGER NOT NULL,value REAL NOT NULL,"
        "quality INTEGER NOT NULL DEFAULT 1 CHECK(quality BETWEEN 0 AND 3),"
        "PRIMARY KEY(tag,ts)) WITHOUT ROWID;"
        "CREATE INDEX IF NOT EXISTS idx_history_tag_ts ON history(tag,ts);"
        "CREATE TABLE IF NOT EXISTS commands ("
        "command_id TEXT PRIMARY KEY,issued_at_ms INTEGER NOT NULL,expires_at_ms INTEGER NOT NULL,"
        "action TEXT NOT NULL,params TEXT NOT NULL,status TEXT NOT NULL DEFAULT 'pending' "
        "CHECK(status IN ('pending','executing','done','failed')),result TEXT,done_ts INTEGER);"
        "CREATE INDEX IF NOT EXISTS idx_commands_status_id ON commands(status,issued_at_ms);";
    if (!execute(schema)) {
        sqlite3_close(db_);
        db_ = nullptr;
        nextOpenAttempt_ = now + kRetryOpenSeconds;
        return false;
    }

    // Upgrade databases created by the earlier sparse-history-only build.
    sqlite3_stmt* columns = nullptr;
    bool hasQuality = false;
    if (sqlite3_prepare_v2(db_, "PRAGMA table_info(history);", -1, &columns, nullptr) == SQLITE_OK) {
        while (sqlite3_step(columns) == SQLITE_ROW) {
            const unsigned char* name = sqlite3_column_text(columns, 1);
            if (name && std::string(reinterpret_cast<const char*>(name)) == "quality") {
                hasQuality = true;
                break;
            }
        }
    }
    if (columns) sqlite3_finalize(columns);
    if (!hasQuality && !execute("ALTER TABLE history ADD COLUMN quality INTEGER NOT NULL DEFAULT 1;")) {
        sqlite3_close(db_);
        db_ = nullptr;
        nextOpenAttempt_ = now + kRetryOpenSeconds;
        return false;
    }

    LOG_INFO("History", "共享 SQLite 已连接（WAL）: " + path_ +
                         "，历史保留 " + std::to_string(retentionDays_) + " 天");
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
                                 int64_t sampleTimeMs, bool storeHistory) {
    if (!openIfNeeded() || !execute("BEGIN IMMEDIATE;")) return false;
    sqlite3_stmt* liveStmt = nullptr;
    sqlite3_stmt* historyStmt = nullptr;
    const char* liveSql =
        "INSERT INTO realtime(tag,value,ts,quality) VALUES(?,?,?,?) "
        "ON CONFLICT(tag) DO UPDATE SET value=excluded.value,ts=excluded.ts,quality=excluded.quality;";
    const char* historySql =
        "INSERT OR REPLACE INTO history(tag,ts,value,quality) VALUES(?,?,?,?);";
    if (sqlite3_prepare_v2(db_, liveSql, -1, &liveStmt, nullptr) != SQLITE_OK ||
        (storeHistory && sqlite3_prepare_v2(db_, historySql, -1, &historyStmt, nullptr) != SQLITE_OK)) {
        if (liveStmt) sqlite3_finalize(liveStmt);
        if (historyStmt) sqlite3_finalize(historyStmt);
        execute("ROLLBACK;");
        logStorageError("准备实时值/历史值写入失败: " + std::string(sqlite3_errmsg(db_)));
        return false;
    }

    bool ok = true;
    for (const auto& item : snapshot) {
        const int64_t pointTimeMs = item.second.ts > 0
            ? static_cast<int64_t>(item.second.ts) * 1000 : sampleTimeMs;
        const int quality = sqlQuality(item.second.quality);
        sqlite3_reset(liveStmt);
        sqlite3_clear_bindings(liveStmt);
        sqlite3_bind_text(liveStmt, 1, item.first.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(liveStmt, 2, item.second.value);
        sqlite3_bind_int64(liveStmt, 3, pointTimeMs);
        sqlite3_bind_int(liveStmt, 4, quality);
        if (sqlite3_step(liveStmt) != SQLITE_DONE) {
            ok = false;
            break;
        }
        if (storeHistory && quality == 1) {
            sqlite3_reset(historyStmt);
            sqlite3_clear_bindings(historyStmt);
            sqlite3_bind_text(historyStmt, 1, item.first.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(historyStmt, 2, sampleTimeMs);
            sqlite3_bind_double(historyStmt, 3, item.second.value);
            sqlite3_bind_int(historyStmt, 4, quality);
            if (sqlite3_step(historyStmt) != SQLITE_DONE) {
                ok = false;
                break;
            }
        }
    }
    if (liveStmt) sqlite3_finalize(liveStmt);
    if (historyStmt) sqlite3_finalize(historyStmt);
    if (!ok) {
        logStorageError("写入实时值/历史值失败: " + std::string(sqlite3_errmsg(db_)));
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
        if (!execute(("DELETE FROM commands WHERE status IN ('done','failed') AND done_ts < " +
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

bool HistoryStore::fetchPendingCommands(std::vector<DbCommand>& commands, int limit) {
    commands.clear();
    if (!openIfNeeded()) return false;
    limit = std::max(1, std::min(limit, 100));
    if (!execute("BEGIN IMMEDIATE;")) return false;

    sqlite3_stmt* select = nullptr;
    const char* selectSql =
        "SELECT command_id,action,params,issued_at_ms,expires_at_ms FROM commands "
        "WHERE status='pending' ORDER BY issued_at_ms,command_id LIMIT ?;";
    if (sqlite3_prepare_v2(db_, selectSql, -1, &select, nullptr) != SQLITE_OK) {
        execute("ROLLBACK;");
        logStorageError("准备命令队列查询失败: " + std::string(sqlite3_errmsg(db_)));
        return false;
    }
    sqlite3_bind_int(select, 1, limit);
    int rc = SQLITE_ROW;
    while ((rc = sqlite3_step(select)) == SQLITE_ROW) {
        DbCommand command;
        const unsigned char* id = sqlite3_column_text(select, 0);
        const unsigned char* action = sqlite3_column_text(select, 1);
        const unsigned char* params = sqlite3_column_text(select, 2);
        command.commandId = id ? reinterpret_cast<const char*>(id) : "";
        command.action = action ? reinterpret_cast<const char*>(action) : "";
        command.params = params ? reinterpret_cast<const char*>(params) : "{}";
        command.issuedAtMs = sqlite3_column_int64(select, 3);
        command.expiresAtMs = sqlite3_column_int64(select, 4);
        commands.push_back(std::move(command));
    }
    sqlite3_finalize(select);
    if (rc != SQLITE_DONE) {
        execute("ROLLBACK;");
        logStorageError("读取命令队列失败: " + std::string(sqlite3_errmsg(db_)));
        commands.clear();
        return false;
    }

    sqlite3_stmt* claim = nullptr;
    if (sqlite3_prepare_v2(db_,
            "UPDATE commands SET status='executing',result='EMS 已接收，正在执行' "
            "WHERE command_id=? AND status='pending';", -1, &claim, nullptr) != SQLITE_OK) {
        execute("ROLLBACK;");
        logStorageError("准备命令认领失败: " + std::string(sqlite3_errmsg(db_)));
        commands.clear();
        return false;
    }
    for (const auto& command : commands) {
        sqlite3_reset(claim);
        sqlite3_clear_bindings(claim);
        sqlite3_bind_text(claim, 1, command.commandId.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(claim) != SQLITE_DONE || sqlite3_changes(db_) != 1) {
            sqlite3_finalize(claim);
            execute("ROLLBACK;");
            logStorageError("认领命令失败: " + command.commandId);
            commands.clear();
            return false;
        }
    }
    sqlite3_finalize(claim);
    if (!execute("COMMIT;")) {
        execute("ROLLBACK;");
        commands.clear();
        return false;
    }
    return true;
}

bool HistoryStore::completeCommand(const std::string& commandId,
                                   const std::string& status,
                                   const std::string& result,
                                   int64_t doneTimeMs) {
    if (commandId.empty() || (status != "done" && status != "failed") || !openIfNeeded())
        return false;
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "UPDATE commands SET status=?,result=?,done_ts=? "
        "WHERE command_id=? AND status='executing';";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        logStorageError("准备命令结果回写失败: " + std::string(sqlite3_errmsg(db_)));
        return false;
    }
    sqlite3_bind_text(stmt, 1, status.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, result.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, doneTimeMs);
    sqlite3_bind_text(stmt, 4, commandId.c_str(), -1, SQLITE_TRANSIENT);
    const bool ok = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(db_) == 1;
    sqlite3_finalize(stmt);
    if (!ok) logStorageError("回写命令结果失败: " + commandId);
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
