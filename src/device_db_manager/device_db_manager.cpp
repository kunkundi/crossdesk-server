#include "device_db_manager.h"

#include <openssl/sha.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <random>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

#include "log.h"

namespace {

std::string ColumnText(sqlite3_stmt* stmt, int column) {
  const unsigned char* text = sqlite3_column_text(stmt, column);
  return text ? reinterpret_cast<const char*>(text) : "";
}

std::string SqliteExecError(sqlite3* db, char* err_msg) {
  return err_msg ? std::string(err_msg) : std::string(sqlite3_errmsg(db));
}

std::string EscapeLikePattern(const std::string& value) {
  std::string escaped;
  escaped.reserve(value.size());
  for (char ch : value) {
    if (ch == '%' || ch == '_' || ch == '\\') {
      escaped.push_back('\\');
    }
    escaped.push_back(ch);
  }
  return escaped;
}

std::string ToLower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char ch) {
                   return static_cast<char>(std::tolower(ch));
                 });
  return value;
}

std::string Trim(std::string value) {
  auto is_space = [](unsigned char ch) { return std::isspace(ch); };
  value.erase(value.begin(),
              std::find_if(value.begin(), value.end(),
                           [&](unsigned char ch) { return !is_space(ch); }));
  value.erase(std::find_if(value.rbegin(), value.rend(),
                           [&](unsigned char ch) { return !is_space(ch); })
                  .base(),
              value.end());
  return value;
}

bool ContainsText(const std::string& value, const std::string& pattern) {
  return value.find(pattern) != std::string::npos;
}

bool IsChinaCountry(const std::string& country) {
  std::string normalized = ToLower(Trim(country));
  return normalized == "china" || normalized == "cn" ||
         ContainsText(country, "中国");
}

std::string NormalizeChinaProvince(const std::string& region,
                                   const std::string& location) {
  std::string value = ToLower(region + " " + location);
  const std::vector<std::pair<std::string, std::vector<std::string>>> matchers =
      {
          {"anhui", {"anhui", "安徽"}},
          {"beijing", {"beijing", "北京"}},
          {"chongqing", {"chongqing", "重庆"}},
          {"fujian", {"fujian", "福建"}},
          {"gansu", {"gansu", "甘肃"}},
          {"guangdong", {"guangdong", "广东"}},
          {"guangxi", {"guangxi", "广西"}},
          {"guizhou", {"guizhou", "贵州"}},
          {"hainan", {"hainan", "海南"}},
          {"hebei", {"hebei", "河北"}},
          {"heilongjiang", {"heilongjiang", "黑龙江"}},
          {"henan", {"henan", "河南"}},
          {"hongkong", {"hong kong", "hongkong", "香港"}},
          {"hubei", {"hubei", "湖北"}},
          {"hunan", {"hunan", "湖南"}},
          {"inner_mongolia",
           {"inner mongolia", "neimenggu", "内蒙古"}},
          {"jiangsu", {"jiangsu", "江苏"}},
          {"jiangxi", {"jiangxi", "江西"}},
          {"jilin", {"jilin", "吉林"}},
          {"liaoning", {"liaoning", "辽宁"}},
          {"macau", {"macau", "macao", "澳门"}},
          {"ningxia", {"ningxia", "宁夏"}},
          {"qinghai", {"qinghai", "青海"}},
          {"shaanxi", {"shaanxi", "shanxi sheng", "陕西"}},
          {"shandong", {"shandong", "山东"}},
          {"shanghai", {"shanghai", "上海"}},
          {"shanxi", {"shanxi", "山西"}},
          {"sichuan", {"sichuan", "四川"}},
          {"taiwan", {"taiwan", "台湾"}},
          {"tianjin", {"tianjin", "天津"}},
          {"tibet", {"tibet", "xizang", "西藏"}},
          {"xinjiang", {"xinjiang", "新疆"}},
          {"yunnan", {"yunnan", "云南"}},
          {"zhejiang", {"zhejiang", "浙江"}},
      };

  for (const auto& matcher : matchers) {
    for (const auto& pattern : matcher.second) {
      if (ContainsText(value, pattern) ||
          ContainsText(region + " " + location, pattern)) {
        return matcher.first;
      }
    }
  }
  return "";
}

std::string NormalizeRemoteDeviceId(const std::string& device_id) {
  return device_id.rfind("C-", 0) == 0 ? device_id.substr(2) : device_id;
}

std::vector<std::string> SplitCommaSeparatedIds(const std::string& value) {
  std::vector<std::string> result;
  size_t start = 0;
  while (start <= value.size()) {
    size_t end = value.find(',', start);
    std::string item =
        value.substr(start, end == std::string::npos ? std::string::npos
                                                     : end - start);
    if (!item.empty()) {
      result.push_back(item);
    }
    if (end == std::string::npos) {
      break;
    }
    start = end + 1;
  }
  std::sort(result.begin(), result.end());
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}

std::string DevicePresenceKindClause(const std::string& kind) {
  std::string normalized = ToLower(kind);
  if (normalized == "web") {
    return "device_id LIKE 'web-%' ";
  }
  if (normalized == "all") {
    return "device_id NOT LIKE 'C-%' ";
  }
  return "device_id NOT LIKE 'web-%' "
         "AND device_id NOT LIKE 'C-%' ";
}

std::string DevicePresenceFilterClause(const std::string& filter,
                                       const std::string& kind) {
  std::string normalized = ToLower(filter);
  if (normalized == "web") {
    return "device_id LIKE 'web-%' ";
  }

  std::string clause = DevicePresenceKindClause(kind);
  if (normalized == "online") {
    clause += "AND online = 1 ";
  } else if (normalized == "offline") {
    clause += "AND online = 0 ";
  } else if (normalized == "active") {
    clause +=
        "AND EXISTS ("
        "SELECT 1 FROM remote_control_sessions "
        "WHERE normalized_host_id = device_presence.device_id) ";
  }
  return clause;
}

std::string DevicePresenceSortClause(const std::string& sort,
                                     const std::string& order) {
  const std::string direction = ToLower(order) == "asc" ? "ASC" : "DESC";
  const std::string normalized = ToLower(sort);

  if (normalized == "status") {
    return "online " + direction + ", updated_at DESC, device_id ASC ";
  }

  std::string expression = "updated_at";
  if (normalized == "device_id" || normalized == "id") {
    expression = "device_id";
  } else if (normalized == "online_since") {
    expression = "online_since";
  } else if (normalized == "current_online") {
    expression = "online_duration_seconds";
  } else if (normalized == "total_online") {
    expression = "total_online_seconds";
  } else if (normalized == "total_control") {
    expression = "total_control_seconds";
  } else if (normalized == "total_controlled") {
    expression = "total_controlled_seconds";
  } else if (normalized == "active_sessions") {
    expression = "active_control_count";
  } else if (normalized == "location") {
    expression = "CASE WHEN TRIM(COALESCE(geo_location, '')) = '' "
                 "THEN 0 ELSE 1 END";
  }

  return expression + " " + direction +
         ", online DESC, updated_at DESC, device_id ASC ";
}

bool ColumnExists(sqlite3* db, const std::string& table,
                  const std::string& column) {
  std::string sql = "PRAGMA table_info(" + table + ");";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
    return false;
  }

  bool found = false;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    if (ColumnText(stmt, 1) == column) {
      found = true;
      break;
    }
  }
  sqlite3_finalize(stmt);
  return found;
}

void EnsureIntegerColumn(sqlite3* db, const std::string& table,
                         const std::string& column) {
  if (ColumnExists(db, table, column)) {
    return;
  }

  std::string sql = "ALTER TABLE " + table + " ADD COLUMN " + column +
                    " INTEGER NOT NULL DEFAULT 0;";
  char* err_msg = nullptr;
  if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err_msg) != SQLITE_OK) {
    std::string error = SqliteExecError(db, err_msg);
    sqlite3_free(err_msg);
    throw std::runtime_error("Failed to add " + table + "." + column +
                             " column: " + error);
  }
}

void EnsureTextColumn(sqlite3* db, const std::string& table,
                      const std::string& column) {
  if (ColumnExists(db, table, column)) {
    return;
  }

  std::string sql = "ALTER TABLE " + table + " ADD COLUMN " + column +
                    " TEXT NOT NULL DEFAULT '';";
  char* err_msg = nullptr;
  if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err_msg) != SQLITE_OK) {
    std::string error = SqliteExecError(db, err_msg);
    sqlite3_free(err_msg);
    throw std::runtime_error("Failed to add " + table + "." + column +
                             " column: " + error);
  }
}

void ExecuteSchemaStatement(sqlite3* db, const std::string& sql,
                            const std::string& description) {
  char* err_msg = nullptr;
  if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err_msg) != SQLITE_OK) {
    std::string error = SqliteExecError(db, err_msg);
    sqlite3_free(err_msg);
    throw std::runtime_error("Failed to " + description + ": " + error);
  }
}

int64_t NowSeconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

constexpr int64_t kRemoteControlRecoveryWindowSeconds = 120;

}  // namespace

DeviceDBManager::DeviceDBManager(const std::string& db_path, OpenMode mode)
    : db_(nullptr) {
  try {
    std::filesystem::path path(db_path);
    if (mode == OpenMode::ReadWrite && !path.parent_path().empty()) {
      std::filesystem::create_directories(path.parent_path());
    }
  } catch (const std::exception& e) {
    throw std::runtime_error("Failed to create parent directory for DB: " +
                             std::string(e.what()));
  }

  const int flags = mode == OpenMode::ReadOnly
                        ? SQLITE_OPEN_READONLY
                        : SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
  int rc = sqlite3_open_v2(db_path.c_str(), &db_, flags, nullptr);
  if (rc != SQLITE_OK) {
    std::string error =
        db_ ? sqlite3_errmsg(db_) : std::string(sqlite3_errstr(rc));
    LOG_ERROR("Failed to open database, {}", error);
    if (db_) {
      sqlite3_close(db_);
    }
    db_ = nullptr;
    throw std::runtime_error("Failed to open database: " + error);
  }
  try {
    sqlite3_busy_timeout(db_, 1000);
    if (mode == OpenMode::ReadWrite) {
      ExecuteSchemaStatement(db_, "PRAGMA journal_mode=WAL;", "enable WAL");
      InitDB();
    }
  } catch (...) {
    sqlite3_close(db_);
    db_ = nullptr;
    throw;
  }
}

void DeviceDBManager::SetReadDeadline(
    std::chrono::steady_clock::time_point deadline) {
  read_deadline_ = deadline;
  sqlite3_progress_handler(
      db_, 1000,
      [](void* data) -> int {
        return std::chrono::steady_clock::now() >=
               static_cast<DeviceDBManager*>(data)->read_deadline_;
      },
      this);
}

bool DeviceDBManager::ClearReadDeadline() {
  const bool expired = std::chrono::steady_clock::now() >= read_deadline_;
  sqlite3_progress_handler(db_, 0, nullptr, nullptr);
  read_deadline_ = std::chrono::steady_clock::time_point::max();
  return expired;
}

DeviceDBManager::~DeviceDBManager() {
  if (db_) sqlite3_close(db_);
}

void DeviceDBManager::InitDB() {
  std::lock_guard<std::recursive_mutex> lock(db_mutex_);
  if (db_ == nullptr) {
    LOG_ERROR("Database is not initialized in InitDB.");
    throw std::runtime_error("Database is not initialized in InitDB.");
  }

  const char* sql_devices =
      "CREATE TABLE IF NOT EXISTS devices ("
      "id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "device_id TEXT UNIQUE NOT NULL,"
      "password_salt TEXT NOT NULL,"
      "password_hash TEXT NOT NULL);";

  const char* sql_seq =
      "CREATE TABLE IF NOT EXISTS device_id_seq ("
      "next_id INTEGER NOT NULL);";

  const char* sql_seq_init =
      "INSERT INTO device_id_seq (next_id) "
      "SELECT 1 WHERE NOT EXISTS (SELECT 1 FROM device_id_seq);";

  const char* sql_presence =
      "CREATE TABLE IF NOT EXISTS device_presence ("
      "device_id TEXT PRIMARY KEY,"
      "online INTEGER NOT NULL,"
      "updated_at INTEGER NOT NULL,"
      "online_since INTEGER NOT NULL DEFAULT 0,"
      "total_online_seconds INTEGER NOT NULL DEFAULT 0,"
      "total_control_seconds INTEGER NOT NULL DEFAULT 0,"
      "total_controlled_seconds INTEGER NOT NULL DEFAULT 0"
      ");";

  const char* sql_user_devices =
      "CREATE TABLE IF NOT EXISTS user_devices ("
      "user_id TEXT NOT NULL,"
      "device_id TEXT NOT NULL,"
      "PRIMARY KEY (user_id, device_id)"
      ");";

  const char* sql_remote_control_sessions =
      "CREATE TABLE IF NOT EXISTS remote_control_sessions ("
      "transmission_id TEXT NOT NULL,"
      "guest_id TEXT NOT NULL,"
      "host_id TEXT NOT NULL,"
      "normalized_guest_id TEXT NOT NULL DEFAULT '',"
      "normalized_host_id TEXT NOT NULL DEFAULT '',"
      "started_at INTEGER NOT NULL,"
      "PRIMARY KEY (transmission_id, guest_id)"
      ");";

  const char* sql_server_runtime =
      "CREATE TABLE IF NOT EXISTS server_runtime ("
      "id INTEGER PRIMARY KEY CHECK(id = 1),"
      "last_seen_at INTEGER NOT NULL DEFAULT 0"
      ");";

  const char* sql_server_runtime_init =
      "INSERT INTO server_runtime (id, last_seen_at) "
      "SELECT 1, 0 WHERE NOT EXISTS "
      "(SELECT 1 FROM server_runtime WHERE id = 1);";

  char* err_msg = nullptr;

  if (sqlite3_exec(db_, sql_devices, nullptr, nullptr, &err_msg) != SQLITE_OK) {
    std::string error = SqliteExecError(db_, err_msg);
    LOG_ERROR("Failed to create devices table: {}", error);
    sqlite3_free(err_msg);
    throw std::runtime_error("Failed to create devices table: " + error);
  }

  if (sqlite3_exec(db_, sql_seq, nullptr, nullptr, &err_msg) != SQLITE_OK) {
    std::string error = SqliteExecError(db_, err_msg);
    LOG_ERROR("Failed to create device_id_seq table: {}", error);
    sqlite3_free(err_msg);
    throw std::runtime_error("Failed to create device_id_seq table: " + error);
  }

  if (sqlite3_exec(db_, sql_seq_init, nullptr, nullptr, &err_msg) !=
      SQLITE_OK) {
    std::string error = SqliteExecError(db_, err_msg);
    LOG_ERROR("Failed to initialize device_id_seq: {}", error);
    sqlite3_free(err_msg);
    throw std::runtime_error("Failed to initialize device_id_seq: " + error);
  }

  if (sqlite3_exec(db_, sql_presence, nullptr, nullptr, &err_msg) !=
      SQLITE_OK) {
    std::string error = SqliteExecError(db_, err_msg);
    LOG_ERROR("Failed to create device_presence table: {}", error);
    sqlite3_free(err_msg);
    throw std::runtime_error("Failed to create device_presence table: " +
                             error);
  }

  EnsureIntegerColumn(db_, "device_presence", "online_since");
  EnsureIntegerColumn(db_, "device_presence", "total_online_seconds");
  EnsureIntegerColumn(db_, "device_presence", "total_control_seconds");
  EnsureIntegerColumn(db_, "device_presence", "total_controlled_seconds");
  EnsureTextColumn(db_, "device_presence", "client_ip");
  EnsureTextColumn(db_, "device_presence", "geo_country");
  EnsureTextColumn(db_, "device_presence", "geo_region");
  EnsureTextColumn(db_, "device_presence", "geo_city");
  EnsureTextColumn(db_, "device_presence", "geo_location");
  EnsureTextColumn(db_, "device_presence", "client_version");
  EnsureTextColumn(db_, "device_presence", "client_platform");

  const char* sql_presence_backfill =
      "UPDATE device_presence SET online_since = updated_at "
      "WHERE online = 1 AND online_since = 0;";
  if (sqlite3_exec(db_, sql_presence_backfill, nullptr, nullptr, &err_msg) !=
      SQLITE_OK) {
    std::string error = SqliteExecError(db_, err_msg);
    LOG_ERROR("Failed to backfill device_presence online_since: {}", error);
    sqlite3_free(err_msg);
    throw std::runtime_error("Failed to backfill device_presence: " + error);
  }

  if (sqlite3_exec(db_, sql_user_devices, nullptr, nullptr, &err_msg) !=
      SQLITE_OK) {
    std::string error = SqliteExecError(db_, err_msg);
    LOG_ERROR("Failed to create user_devices table: {}", error);
    sqlite3_free(err_msg);
    throw std::runtime_error("Failed to create user_devices table: " + error);
  }

  if (sqlite3_exec(db_, sql_remote_control_sessions, nullptr, nullptr,
                   &err_msg) != SQLITE_OK) {
    std::string error = SqliteExecError(db_, err_msg);
    LOG_ERROR("Failed to create remote_control_sessions table: {}", error);
    sqlite3_free(err_msg);
    throw std::runtime_error("Failed to create remote_control_sessions table: " +
                             error);
  }
  EnsureTextColumn(db_, "remote_control_sessions", "normalized_guest_id");
  EnsureTextColumn(db_, "remote_control_sessions", "normalized_host_id");
  ExecuteSchemaStatement(
      db_,
      "UPDATE remote_control_sessions SET "
      "normalized_guest_id = CASE WHEN guest_id LIKE 'C-%' "
      "THEN substr(guest_id, 3) ELSE guest_id END "
      "WHERE normalized_guest_id = '';",
      "backfill remote_control_sessions.normalized_guest_id");
  ExecuteSchemaStatement(
      db_,
      "UPDATE remote_control_sessions SET "
      "normalized_host_id = CASE WHEN host_id LIKE 'C-%' "
      "THEN substr(host_id, 3) ELSE host_id END "
      "WHERE normalized_host_id = '';",
      "backfill remote_control_sessions.normalized_host_id");

  const std::vector<std::pair<std::string, std::string>> indexes = {
      {"CREATE INDEX IF NOT EXISTS idx_device_presence_online_updated_at "
       "ON device_presence(online, updated_at);",
       "create idx_device_presence_online_updated_at"},
      {"CREATE INDEX IF NOT EXISTS idx_device_presence_updated_at "
       "ON device_presence(updated_at);",
       "create idx_device_presence_updated_at"},
      {"CREATE INDEX IF NOT EXISTS idx_device_presence_geo "
       "ON device_presence(geo_country, geo_region, geo_location);",
       "create idx_device_presence_geo"},
      {"CREATE INDEX IF NOT EXISTS idx_remote_control_sessions_norm_guest "
       "ON remote_control_sessions(normalized_guest_id);",
       "create idx_remote_control_sessions_norm_guest"},
      {"CREATE INDEX IF NOT EXISTS idx_remote_control_sessions_norm_host "
       "ON remote_control_sessions(normalized_host_id);",
       "create idx_remote_control_sessions_norm_host"},
      {"CREATE INDEX IF NOT EXISTS idx_remote_control_sessions_host_tx "
       "ON remote_control_sessions(host_id, transmission_id);",
       "create idx_remote_control_sessions_host_tx"}};
  for (const auto& index : indexes) {
    ExecuteSchemaStatement(db_, index.first, index.second);
  }

  if (sqlite3_exec(db_, sql_server_runtime, nullptr, nullptr, &err_msg) !=
      SQLITE_OK) {
    std::string error = SqliteExecError(db_, err_msg);
    LOG_ERROR("Failed to create server_runtime table: {}", error);
    sqlite3_free(err_msg);
    throw std::runtime_error("Failed to create server_runtime table: " +
                             error);
  }

  if (sqlite3_exec(db_, sql_server_runtime_init, nullptr, nullptr,
                   &err_msg) != SQLITE_OK) {
    std::string error = SqliteExecError(db_, err_msg);
    LOG_ERROR("Failed to initialize server_runtime: {}", error);
    sqlite3_free(err_msg);
    throw std::runtime_error("Failed to initialize server_runtime: " + error);
  }

  int64_t now = NowSeconds();
  int64_t runtime_last_seen = GetRuntimeLastSeen();
  int64_t stale_cutoff =
      runtime_last_seen > 0 ? std::min(runtime_last_seen, now) : 0;
  bool recover_remote_sessions =
      stale_cutoff > 0 &&
      now - stale_cutoff <= kRemoteControlRecoveryWindowSeconds;

  if (!recover_remote_sessions) {
    if (stale_cutoff > 0) {
      std::string cutoff = std::to_string(stale_cutoff);
      std::string sql_finalize_remote =
          "UPDATE device_presence SET "
          "total_control_seconds = total_control_seconds + COALESCE(("
          "SELECT SUM(MAX(0, " +
          cutoff +
          " - started_at)) "
          "FROM remote_control_sessions "
          "WHERE normalized_guest_id = device_presence.device_id), 0), "
          "total_controlled_seconds = total_controlled_seconds + COALESCE(("
          "SELECT SUM(MAX(0, " +
          cutoff +
          " - started_at)) "
          "FROM remote_control_sessions "
          "WHERE normalized_host_id = device_presence.device_id), 0) "
          "WHERE EXISTS ("
          "SELECT 1 FROM remote_control_sessions "
          "WHERE normalized_guest_id = device_presence.device_id "
          "OR normalized_host_id = device_presence.device_id);";
      if (sqlite3_exec(db_, sql_finalize_remote.c_str(), nullptr, nullptr,
                       &err_msg) != SQLITE_OK) {
        std::string error = SqliteExecError(db_, err_msg);
        LOG_ERROR("Failed to finalize stale remote control sessions: {}",
                  error);
        sqlite3_free(err_msg);
        throw std::runtime_error(
            "Failed to finalize stale remote control sessions: " + error);
      }
    }

    if (sqlite3_exec(db_, "DELETE FROM remote_control_sessions;", nullptr,
                     nullptr, &err_msg) != SQLITE_OK) {
      std::string error = SqliteExecError(db_, err_msg);
      LOG_ERROR("Failed to clear remote_control_sessions: {}", error);
      sqlite3_free(err_msg);
      throw std::runtime_error("Failed to clear remote_control_sessions: " +
                               error);
    }
  } else {
    LOG_INFO("Recovering persisted remote control sessions from last {}s",
             now - stale_cutoff);
  }

  int64_t offline_time = stale_cutoff > 0 ? stale_cutoff : now;
  std::string sql_presence_startup_reset = "UPDATE device_presence SET ";
  if (stale_cutoff > 0) {
    std::string cutoff = std::to_string(stale_cutoff);
    sql_presence_startup_reset +=
        "total_online_seconds = total_online_seconds + "
        "CASE WHEN online_since > 0 THEN MAX(0, " +
        cutoff +
        " - online_since) ELSE 0 END, ";
  }
  sql_presence_startup_reset +=
      "online = 0, "
      "updated_at = " +
      std::to_string(offline_time) +
      ", "
      "online_since = 0 "
      "WHERE online = 1;";
  if (sqlite3_exec(db_, sql_presence_startup_reset.c_str(), nullptr, nullptr,
                   &err_msg) != SQLITE_OK) {
    std::string error = SqliteExecError(db_, err_msg);
    LOG_ERROR("Failed to reset stale device presence: {}", error);
    sqlite3_free(err_msg);
    throw std::runtime_error("Failed to reset stale device presence: " +
                             error);
  }

  if (!RecordRuntimeHeartbeat()) {
    throw std::runtime_error("Failed to record runtime heartbeat");
  }
}

std::string DeviceDBManager::Sha256(const std::string& str) {
  unsigned char hash[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const unsigned char*>(str.c_str()), str.size(), hash);

  std::stringstream ss;
  for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i)
    ss << std::hex << std::setw(2) << std::setfill('0') << (int)hash[i];

  return ss.str();
}

std::string DeviceDBManager::GenerateSalt() {
  static const char charset[] = "0123456789ABCDEF";
  static std::mt19937 rng(static_cast<unsigned>(
      std::chrono::steady_clock::now().time_since_epoch().count()));
  std::uniform_int_distribution<int> dist(0, 15);

  std::string salt;
  for (int i = 0; i < 16; ++i) {
    salt += charset[dist(rng)];
  }
  return salt;
}

std::string DeviceDBManager::HashPasswordWithSalt(const std::string& salt,
                                                  const std::string& password) {
  return Sha256(salt + password);
}

bool DeviceDBManager::DeviceIdExists(const std::string& device_id) {
  std::lock_guard<std::recursive_mutex> lock(db_mutex_);
  if (db_ == nullptr || device_id.empty()) {
    return false;
  }

  const char* sql = "SELECT 1 FROM devices WHERE device_id = ? LIMIT 1;";
  sqlite3_stmt* stmt = nullptr;

  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    return false;
  }

  sqlite3_bind_text(stmt, 1, device_id.c_str(), -1, SQLITE_TRANSIENT);
  bool exists = (sqlite3_step(stmt) == SQLITE_ROW);
  sqlite3_finalize(stmt);

  return exists;
}

std::string DeviceDBManager::GenerateDeviceId() {
  std::lock_guard<std::recursive_mutex> lock(db_mutex_);
  if (db_ == nullptr) {
    LOG_ERROR("Database is not initialized in GenerateDeviceId.");
    return {};
  }

  const int MIN_ID = 100000000;
  const int MAX_ID = 999999999;
  const int MAX_RETRIES = 100;

  std::random_device rd;
  std::mt19937 rng(rd());
  std::uniform_int_distribution<int> dist(MIN_ID, MAX_ID);

  // try to generate unique ID
  for (int attempt = 0; attempt < MAX_RETRIES; ++attempt) {
    int obfuscated_id = dist(rng);

    char buf[10] = {0};
    snprintf(buf, sizeof(buf), "%09d", obfuscated_id);
    std::string device_id(buf);

    // check if ID already exists
    if (!DeviceIdExists(device_id)) {
      return device_id;
    }
  }

  LOG_ERROR("Failed to generate unique device ID after {} attempts.",
            MAX_RETRIES);
  return {};
}

std::string DeviceDBManager::GeneratePassword() {
  static const char charset[] =
      "0123456789"
      "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
      "abcdefghijklmnopqrstuvwxyz";
  static std::mt19937 rng(static_cast<unsigned>(
      std::chrono::steady_clock::now().time_since_epoch().count()));
  std::uniform_int_distribution<int> dist(0,
                                          sizeof(charset) - 2);  // exclude '\0'

  std::string pwd;
  for (int i = 0; i < 6; ++i) {
    pwd += charset[dist(rng)];
  }
  return pwd;
}

DeviceCredential DeviceDBManager::AddDevice(const std::string& device_id,
                                            const std::string& password) {
  std::lock_guard<std::recursive_mutex> lock(db_mutex_);
  if (db_ == nullptr) {
    LOG_ERROR("Database is not initialized.");
    return {};
  }

  if (!device_id.empty() && device_id != "web") {
    const char* select_sql =
        "SELECT password_salt, password_hash FROM devices WHERE device_id = ?;";
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, select_sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
      LOG_ERROR("Failed to prepare select statement.");
      return {};
    }

    sqlite3_bind_text(stmt, 1, device_id.c_str(), -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
      // Device exists
      std::string salt = ColumnText(stmt, 0);
      std::string stored_hash = ColumnText(stmt, 1);
      std::string hash = HashPasswordWithSalt(salt, password);

      sqlite3_finalize(stmt);
      if (stored_hash != hash) {
        LOG_WARN("Reject existing device [{}] login: password mismatch.",
                 device_id);
        return {};
      }
      return {device_id, "", false};  // same password
    }
    sqlite3_finalize(stmt);
  }

  // Device not exists or device_id is empty — generate new
  const int MAX_RETRIES = 10;
  for (int i = 0; i < MAX_RETRIES; ++i) {
    std::string new_id;
    if (device_id == "web") {
      std::string generated_id = GenerateDeviceId();
      if (generated_id.empty()) {
        LOG_ERROR("Failed to generate device ID for web client.");
        return {};
      }
      new_id = device_id + "-" + generated_id;
    } else {
      new_id = GenerateDeviceId();
      if (new_id.empty()) {
        LOG_ERROR("Failed to generate device ID.");
        return {};
      }
    }

    // Check if the generated ID (including web- prefix) already exists
    if (DeviceIdExists(new_id)) {
      LOG_WARN("Generated ID {} already exists, retrying...", new_id);
      continue;
    }

    std::string new_pwd = GeneratePassword();
    if (new_pwd.empty()) {
      LOG_ERROR("Failed to generate password.");
      return {};
    }

    std::string salt = GenerateSalt();
    std::string hash = HashPasswordWithSalt(salt, new_pwd);

    // Use transaction to reduce race condition
    sqlite3_exec(db_, "BEGIN TRANSACTION;", nullptr, nullptr, nullptr);

    // Double-check ID uniqueness within transaction
    if (DeviceIdExists(new_id)) {
      sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
      LOG_WARN("Generated ID {} already exists, retrying...", new_id);
      continue;
    }

    const char* insert_sql =
        "INSERT INTO devices (device_id, password_hash, password_salt) VALUES "
        "(?, ?, ?);";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, insert_sql, -1, &stmt, nullptr) != SQLITE_OK) {
      sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
      LOG_ERROR("Failed to prepare insert statement.");
      return {};
    }

    sqlite3_bind_text(stmt, 1, new_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, hash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, salt.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc == SQLITE_DONE) {
      sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);
      // For web clients, return empty password
      if (device_id == "web") {
        return {new_id, "", false};
      }
      return {new_id, new_pwd, false};
    } else if (rc == SQLITE_CONSTRAINT) {
      sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
      LOG_WARN(
          "Insert failed due to constraint (ID may have been inserted "
          "concurrently): {}",
          new_id);
      continue;
    } else {
      sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
      LOG_ERROR("Insert device failed: {}", sqlite3_errmsg(db_));
      return {};
    }
  }

  LOG_ERROR("Failed to generate unique device_id after {} attempts.",
            MAX_RETRIES);
  return {};
}

int DeviceDBManager::VerifyDevice(const std::string& device_id,
                                  const std::string& password) {
  std::lock_guard<std::recursive_mutex> lock(db_mutex_);
  if (db_ == nullptr) {
    LOG_ERROR("Database is not initialized in VerifyDevice.");
    return -1;
  }

  const char* sql =
      "SELECT password_salt, password_hash FROM devices WHERE device_id = ?;";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    return -1;
  }

  sqlite3_bind_text(stmt, 1, device_id.c_str(), -1, SQLITE_TRANSIENT);

  // Check if device exists
  int result = -2;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    std::string salt = ColumnText(stmt, 0);
    std::string stored_hash = ColumnText(stmt, 1);

    std::string hash = HashPasswordWithSalt(salt, password);
    if (hash == stored_hash) {
      // Password is correct
      result = 0;
    } else {
      // Password is incorrect
      result = -1;
    }
  }

  sqlite3_finalize(stmt);
  return result;
}

bool DeviceDBManager::UpdatePassword(const std::string& device_id,
                                     const std::string& new_password) {
  std::lock_guard<std::recursive_mutex> lock(db_mutex_);
  if (db_ == nullptr) {
    LOG_ERROR("Database is not initialized in UpdatePassword.");
    return false;
  }

  std::string salt = GenerateSalt();
  std::string hash = HashPasswordWithSalt(salt, new_password);

  const char* sql =
      "UPDATE devices SET password_salt = ?, password_hash = ? WHERE device_id "
      "= ?;";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    return false;
  }

  sqlite3_bind_text(stmt, 1, salt.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, hash.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, device_id.c_str(), -1, SQLITE_TRANSIENT);

  bool success = (sqlite3_step(stmt) == SQLITE_DONE);
  sqlite3_finalize(stmt);
  return success;
}

bool DeviceDBManager::RemoveDevice(const std::string& device_id) {
  std::lock_guard<std::recursive_mutex> lock(db_mutex_);
  if (db_ == nullptr) {
    LOG_ERROR("Database is not initialized in RemoveDevice.");
    return false;
  }

  const char* sql = "DELETE FROM devices WHERE device_id = ?;";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    return false;
  }

  sqlite3_bind_text(stmt, 1, device_id.c_str(), -1, SQLITE_TRANSIENT);
  bool success = (sqlite3_step(stmt) == SQLITE_DONE);
  sqlite3_finalize(stmt);
  return success;
}

int64_t DeviceDBManager::GetRuntimeLastSeen() {
  std::lock_guard<std::recursive_mutex> lock(db_mutex_);
  if (db_ == nullptr) {
    LOG_ERROR("Database is not initialized in GetRuntimeLastSeen.");
    return 0;
  }

  const char* sql = "SELECT last_seen_at FROM server_runtime WHERE id = 1;";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    return 0;
  }

  int64_t last_seen_at = 0;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    last_seen_at = sqlite3_column_int64(stmt, 0);
  }
  sqlite3_finalize(stmt);
  return last_seen_at;
}

bool DeviceDBManager::RecordRuntimeHeartbeat() {
  std::lock_guard<std::recursive_mutex> lock(db_mutex_);
  if (db_ == nullptr) {
    LOG_ERROR("Database is not initialized in RecordRuntimeHeartbeat.");
    return false;
  }

  std::string now = std::to_string(NowSeconds());
  std::string sql = "UPDATE server_runtime SET last_seen_at = " + now +
                    " WHERE id = 1;";

  char* err_msg = nullptr;
  if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err_msg) !=
      SQLITE_OK) {
    std::string error = SqliteExecError(db_, err_msg);
    LOG_ERROR("Failed to record runtime heartbeat: {}", error);
    sqlite3_free(err_msg);
    return false;
  }
  return true;
}

bool DeviceDBManager::SetDeviceOnline(const std::string& device_id,
                                      bool online) {
  std::lock_guard<std::recursive_mutex> lock(db_mutex_);
  if (db_ == nullptr) {
    LOG_ERROR("Database is not initialized in SetDeviceOnline.");
    return false;
  }

  const char* online_sql =
      "INSERT INTO device_presence "
      "(device_id, online, updated_at, online_since, total_online_seconds) "
      "VALUES (?, 1, CAST(strftime('%s','now') AS INTEGER), "
      "CAST(strftime('%s','now') AS INTEGER), 0) "
      "ON CONFLICT(device_id) DO UPDATE SET "
      "online=1, "
      "updated_at=CAST(strftime('%s','now') AS INTEGER), "
      "online_since=CASE "
      "WHEN device_presence.online = 1 AND device_presence.online_since > 0 "
      "THEN device_presence.online_since "
      "ELSE CAST(strftime('%s','now') AS INTEGER) END;";

  const char* offline_sql =
      "INSERT INTO device_presence "
      "(device_id, online, updated_at, online_since, total_online_seconds) "
      "VALUES (?, 0, CAST(strftime('%s','now') AS INTEGER), 0, 0) "
      "ON CONFLICT(device_id) DO UPDATE SET "
      "total_online_seconds=device_presence.total_online_seconds + "
      "CASE WHEN device_presence.online = 1 AND "
      "device_presence.online_since > 0 THEN "
      "MAX(0, CAST(strftime('%s','now') AS INTEGER) - "
      "device_presence.online_since) ELSE 0 END, "
      "online=0, "
      "updated_at=CAST(strftime('%s','now') AS INTEGER), "
      "online_since=0;";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, online ? online_sql : offline_sql, -1, &stmt,
                         nullptr) != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_text(stmt, 1, device_id.c_str(), -1, SQLITE_TRANSIENT);
  bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
  sqlite3_finalize(stmt);
  return ok;
}

bool DeviceDBManager::UpdateDeviceNetworkInfo(
    const std::string& device_id, const ClientNetworkInfo& network_info) {
  std::lock_guard<std::recursive_mutex> lock(db_mutex_);
  if (db_ == nullptr) {
    LOG_ERROR("Database is not initialized in UpdateDeviceNetworkInfo.");
    return false;
  }
  if (device_id.empty()) {
    return false;
  }

  const char* sql =
      "INSERT INTO device_presence "
      "(device_id, online, updated_at, online_since, total_online_seconds, "
      "client_ip, geo_country, geo_region, geo_city, geo_location) "
      "VALUES (?, 0, CAST(strftime('%s','now') AS INTEGER), 0, 0, ?, ?, ?, ?, "
      "?) "
      "ON CONFLICT(device_id) DO UPDATE SET "
      "client_ip=excluded.client_ip, "
      "geo_country=excluded.geo_country, "
      "geo_region=excluded.geo_region, "
      "geo_city=excluded.geo_city, "
      "geo_location=excluded.geo_location;";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_text(stmt, 1, device_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, network_info.client_ip.c_str(), -1,
                    SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, network_info.country.c_str(), -1,
                    SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 4, network_info.region.c_str(), -1,
                    SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 5, network_info.city.c_str(), -1,
                    SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 6, network_info.location.c_str(), -1,
                    SQLITE_TRANSIENT);
  bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
  sqlite3_finalize(stmt);
  return ok;
}

bool DeviceDBManager::UpdateDeviceClientInfo(
    const std::string& device_id, const std::string& client_version,
    const std::string& client_platform) {
  std::lock_guard<std::recursive_mutex> lock(db_mutex_);
  if (db_ == nullptr) {
    LOG_ERROR("Database is not initialized in UpdateDeviceClientInfo.");
    return false;
  }
  if (device_id.empty()) {
    return false;
  }

  const char* sql =
      "INSERT INTO device_presence "
      "(device_id, online, updated_at, client_version, client_platform) "
      "VALUES (?, 0, CAST(strftime('%s','now') AS INTEGER), ?, ?) "
      "ON CONFLICT(device_id) DO UPDATE SET "
      "client_version=excluded.client_version, "
      "client_platform=excluded.client_platform;";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_text(stmt, 1, device_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, client_version.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, client_platform.c_str(), -1, SQLITE_TRANSIENT);
  const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
  sqlite3_finalize(stmt);
  return ok;
}

bool DeviceDBManager::StartRemoteControlSession(
    const std::string& transmission_id, const std::string& host_id,
    const std::string& guest_id) {
  std::lock_guard<std::recursive_mutex> lock(db_mutex_);
  if (db_ == nullptr) {
    LOG_ERROR("Database is not initialized in StartRemoteControlSession.");
    return false;
  }
  if (transmission_id.empty() || host_id.empty() || guest_id.empty() ||
      host_id == guest_id) {
    return false;
  }

  char* err_msg = nullptr;
  if (sqlite3_exec(db_, "BEGIN TRANSACTION;", nullptr, nullptr, &err_msg) !=
      SQLITE_OK) {
    LOG_ERROR("Failed to begin StartRemoteControlSession transaction: {}",
              err_msg ? err_msg : sqlite3_errmsg(db_));
    sqlite3_free(err_msg);
    return false;
  }

  auto rollback = [this]() {
    char* rollback_err = nullptr;
    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, &rollback_err);
    sqlite3_free(rollback_err);
  };

  auto ensure_presence = [this](const std::string& device_id) {
    const char* sql =
        "INSERT INTO device_presence "
        "(device_id, online, updated_at, online_since, total_online_seconds, "
        "total_control_seconds, total_controlled_seconds) "
        "VALUES (?, 0, CAST(strftime('%s','now') AS INTEGER), 0, 0, 0, 0) "
        "ON CONFLICT(device_id) DO NOTHING;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
      return false;
    }
    sqlite3_bind_text(stmt, 1, device_id.c_str(), -1, SQLITE_TRANSIENT);
    bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return ok;
  };

  std::string normalized_host_id = NormalizeRemoteDeviceId(host_id);
  std::string normalized_guest_id = NormalizeRemoteDeviceId(guest_id);
  if (!ensure_presence(normalized_host_id) ||
      !ensure_presence(normalized_guest_id)) {
    rollback();
    return false;
  }

  const char* sql =
      "INSERT OR IGNORE INTO remote_control_sessions "
      "(transmission_id, guest_id, host_id, normalized_guest_id, "
      "normalized_host_id, started_at) "
      "VALUES (?, ?, ?, ?, ?, CAST(strftime('%s','now') AS INTEGER));";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    rollback();
    return false;
  }
  sqlite3_bind_text(stmt, 1, transmission_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, guest_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, host_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 4, normalized_guest_id.c_str(), -1,
                    SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 5, normalized_host_id.c_str(), -1,
                    SQLITE_TRANSIENT);
  bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
  sqlite3_finalize(stmt);
  if (!ok) {
    rollback();
    return false;
  }

  if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, &err_msg) != SQLITE_OK) {
    LOG_ERROR("Failed to commit StartRemoteControlSession transaction: {}",
              err_msg ? err_msg : sqlite3_errmsg(db_));
    sqlite3_free(err_msg);
    rollback();
    return false;
  }
  return true;
}

bool DeviceDBManager::EndRemoteControlSession(
    const std::string& transmission_id, const std::string& host_id,
    const std::string& guest_id) {
  std::lock_guard<std::recursive_mutex> lock(db_mutex_);
  if (db_ == nullptr) {
    LOG_ERROR("Database is not initialized in EndRemoteControlSession.");
    return false;
  }
  if (transmission_id.empty() || guest_id.empty()) {
    return false;
  }

  const char* select_sql =
      "SELECT host_id, started_at FROM remote_control_sessions "
      "WHERE transmission_id = ? AND guest_id = ?;";
  sqlite3_stmt* select_stmt = nullptr;
  if (sqlite3_prepare_v2(db_, select_sql, -1, &select_stmt, nullptr) !=
      SQLITE_OK) {
    return false;
  }
  sqlite3_bind_text(select_stmt, 1, transmission_id.c_str(), -1,
                    SQLITE_TRANSIENT);
  sqlite3_bind_text(select_stmt, 2, guest_id.c_str(), -1, SQLITE_TRANSIENT);

  std::string stored_host_id;
  int64_t started_at = 0;
  bool found = false;
  if (sqlite3_step(select_stmt) == SQLITE_ROW) {
    stored_host_id = ColumnText(select_stmt, 0);
    started_at = sqlite3_column_int64(select_stmt, 1);
    found = true;
  }
  sqlite3_finalize(select_stmt);
  if (!found) {
    return true;
  }

  std::string effective_host_id =
      stored_host_id.empty() ? host_id : stored_host_id;
  if (effective_host_id.empty()) {
    return false;
  }
  int64_t duration = std::max<int64_t>(0, NowSeconds() - started_at);

  char* err_msg = nullptr;
  if (sqlite3_exec(db_, "BEGIN TRANSACTION;", nullptr, nullptr, &err_msg) !=
      SQLITE_OK) {
    LOG_ERROR("Failed to begin EndRemoteControlSession transaction: {}",
              err_msg ? err_msg : sqlite3_errmsg(db_));
    sqlite3_free(err_msg);
    return false;
  }

  auto rollback = [this]() {
    char* rollback_err = nullptr;
    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, &rollback_err);
    sqlite3_free(rollback_err);
  };

  auto add_duration = [this](const char* column, const std::string& device_id,
                             int64_t value) {
    std::string sql =
        std::string("UPDATE device_presence SET ") + column + " = " + column +
        " + ? WHERE device_id = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) !=
        SQLITE_OK) {
      return false;
    }
    sqlite3_bind_int64(stmt, 1, value);
    sqlite3_bind_text(stmt, 2, device_id.c_str(), -1, SQLITE_TRANSIENT);
    bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return ok;
  };

  if (!add_duration("total_control_seconds",
                    NormalizeRemoteDeviceId(guest_id), duration) ||
      !add_duration("total_controlled_seconds",
                    NormalizeRemoteDeviceId(effective_host_id), duration)) {
    rollback();
    return false;
  }

  const char* delete_sql =
      "DELETE FROM remote_control_sessions "
      "WHERE transmission_id = ? AND guest_id = ?;";
  sqlite3_stmt* delete_stmt = nullptr;
  if (sqlite3_prepare_v2(db_, delete_sql, -1, &delete_stmt, nullptr) !=
      SQLITE_OK) {
    rollback();
    return false;
  }
  sqlite3_bind_text(delete_stmt, 1, transmission_id.c_str(), -1,
                    SQLITE_TRANSIENT);
  sqlite3_bind_text(delete_stmt, 2, guest_id.c_str(), -1, SQLITE_TRANSIENT);
  bool ok = (sqlite3_step(delete_stmt) == SQLITE_DONE);
  sqlite3_finalize(delete_stmt);
  if (!ok) {
    rollback();
    return false;
  }

  if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, &err_msg) != SQLITE_OK) {
    LOG_ERROR("Failed to commit EndRemoteControlSession transaction: {}",
              err_msg ? err_msg : sqlite3_errmsg(db_));
    sqlite3_free(err_msg);
    rollback();
    return false;
  }
  return true;
}

bool DeviceDBManager::EndRemoteControlTransmission(
    const std::string& transmission_id) {
  std::lock_guard<std::recursive_mutex> lock(db_mutex_);
  if (db_ == nullptr) {
    LOG_ERROR("Database is not initialized in EndRemoteControlTransmission.");
    return false;
  }
  if (transmission_id.empty()) {
    return false;
  }

  const char* sql =
      "SELECT host_id, guest_id FROM remote_control_sessions "
      "WHERE transmission_id = ?;";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_text(stmt, 1, transmission_id.c_str(), -1, SQLITE_TRANSIENT);

  std::vector<std::pair<std::string, std::string>> sessions;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    sessions.push_back({ColumnText(stmt, 0), ColumnText(stmt, 1)});
  }
  sqlite3_finalize(stmt);

  bool ok = true;
  for (const auto& session : sessions) {
    ok = EndRemoteControlSession(transmission_id, session.first,
                                 session.second) &&
         ok;
  }
  return ok;
}

int DeviceDBManager::CountActiveRemoteControlConnections() {
  std::lock_guard<std::recursive_mutex> lock(db_mutex_);
  if (db_ == nullptr) {
    LOG_ERROR(
        "Database is not initialized in CountActiveRemoteControlConnections.");
    return 0;
  }

  const char* sql = "SELECT COUNT(*) FROM remote_control_sessions;";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    return 0;
  }

  int count = 0;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    count = sqlite3_column_int(stmt, 0);
  }
  sqlite3_finalize(stmt);
  return count;
}

int DeviceDBManager::CountRemoteControlTransmissions(
    const std::string& search) {
  std::lock_guard<std::recursive_mutex> lock(db_mutex_);
  if (db_ == nullptr) {
    LOG_ERROR("Database is not initialized in CountRemoteControlTransmissions.");
    return 0;
  }

  std::string sql =
      "SELECT COUNT(*) FROM ("
      "SELECT transmission_id, host_id FROM remote_control_sessions ";
  if (!search.empty()) {
    sql +=
        "WHERE transmission_id LIKE ? ESCAPE '\\' "
        "OR host_id LIKE ? ESCAPE '\\' "
        "OR guest_id LIKE ? ESCAPE '\\' ";
  }
  sql += "GROUP BY transmission_id, host_id);";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) !=
      SQLITE_OK) {
    return 0;
  }
  if (!search.empty()) {
    std::string pattern = "%" + EscapeLikePattern(search) + "%";
    sqlite3_bind_text(stmt, 1, pattern.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, pattern.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, pattern.c_str(), -1, SQLITE_TRANSIENT);
  }

  int count = 0;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    count = sqlite3_column_int(stmt, 0);
  }
  sqlite3_finalize(stmt);
  return count;
}

std::vector<RemoteControlSessionInfo>
DeviceDBManager::ListRemoteControlSessions(size_t limit, size_t offset,
                                           const std::string& search) {
  std::lock_guard<std::recursive_mutex> lock(db_mutex_);
  std::vector<RemoteControlSessionInfo> result;
  if (db_ == nullptr) {
    LOG_ERROR("Database is not initialized in ListRemoteControlSessions.");
    return result;
  }

  std::string sql =
      "SELECT transmission_id, host_id, MIN(started_at), "
      "GROUP_CONCAT(guest_id) "
      "FROM remote_control_sessions ";
  if (!search.empty()) {
    sql +=
        "WHERE transmission_id LIKE ? ESCAPE '\\' "
        "OR host_id LIKE ? ESCAPE '\\' "
        "OR guest_id LIKE ? ESCAPE '\\' ";
  }
  sql +=
      "GROUP BY transmission_id, host_id "
      "ORDER BY MIN(started_at) DESC, transmission_id ASC "
      "LIMIT ? OFFSET ?;";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) !=
      SQLITE_OK) {
    return result;
  }

  int bind_index = 1;
  if (!search.empty()) {
    std::string pattern = "%" + EscapeLikePattern(search) + "%";
    sqlite3_bind_text(stmt, bind_index++, pattern.c_str(), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, bind_index++, pattern.c_str(), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, bind_index++, pattern.c_str(), -1,
                      SQLITE_TRANSIENT);
  }
  sqlite3_bind_int64(stmt, bind_index++, static_cast<sqlite3_int64>(limit));
  sqlite3_bind_int64(stmt, bind_index++, static_cast<sqlite3_int64>(offset));

  while (sqlite3_step(stmt) == SQLITE_ROW) {
    RemoteControlSessionInfo info;
    info.transmission_id = ColumnText(stmt, 0);
    info.host_id = ColumnText(stmt, 1);
    info.started_at = sqlite3_column_int64(stmt, 2);
    info.guest_ids = SplitCommaSeparatedIds(ColumnText(stmt, 3));
    result.push_back(info);
  }
  sqlite3_finalize(stmt);
  return result;
}

int DeviceDBManager::GetOnlineDeviceCount() {
  return CountOnlineDevices();
}

int DeviceDBManager::CountOnlineDevices(const std::string& search) {
  std::lock_guard<std::recursive_mutex> lock(db_mutex_);
  if (db_ == nullptr) {
    LOG_ERROR("Database is not initialized in CountOnlineDevices.");
    return 0;
  }

  std::string sql =
      "SELECT COUNT(*) FROM device_presence "
      "WHERE online = 1 "
      "AND device_id NOT LIKE 'web-%' "
      "AND device_id NOT LIKE 'C-%' ";
  if (!search.empty()) {
    sql += "AND device_id LIKE ? ESCAPE '\\' ";
  }
  sql += ";";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) !=
      SQLITE_OK) {
    return 0;
  }
  if (!search.empty()) {
    std::string pattern = "%" + EscapeLikePattern(search) + "%";
    sqlite3_bind_text(stmt, 1, pattern.c_str(), -1, SQLITE_TRANSIENT);
  }

  int count = 0;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    count = sqlite3_column_int(stmt, 0);
  }
  sqlite3_finalize(stmt);
  return count;
}

int DeviceDBManager::CountDevicePresence(const std::string& search,
                                         const std::string& filter,
                                         const std::string& kind) {
  std::lock_guard<std::recursive_mutex> lock(db_mutex_);
  if (db_ == nullptr) {
    LOG_ERROR("Database is not initialized in CountDevicePresence.");
    return 0;
  }

  std::string sql = "SELECT COUNT(*) FROM device_presence WHERE " +
                    DevicePresenceFilterClause(filter, kind);
  if (!search.empty()) {
    sql += "AND device_id LIKE ? ESCAPE '\\' ";
  }
  sql += ";";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) !=
      SQLITE_OK) {
    return 0;
  }
  if (!search.empty()) {
    std::string pattern = "%" + EscapeLikePattern(search) + "%";
    sqlite3_bind_text(stmt, 1, pattern.c_str(), -1, SQLITE_TRANSIENT);
  }

  int count = 0;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    count = sqlite3_column_int(stmt, 0);
  }
  sqlite3_finalize(stmt);
  return count;
}

DevicePresenceCounts DeviceDBManager::CountDevicePresenceByFilters(
    const std::string& search, const std::string& kind) {
  std::lock_guard<std::recursive_mutex> lock(db_mutex_);
  DevicePresenceCounts counts;
  if (db_ == nullptr) {
    LOG_ERROR("Database is not initialized in CountDevicePresenceByFilters.");
    return counts;
  }

  const std::string kind_clause = DevicePresenceKindClause(kind);
  std::string sql =
      "SELECT "
      "COALESCE(SUM(CASE WHEN " +
      kind_clause +
      "THEN 1 ELSE 0 END), 0), "
      "COALESCE(SUM(CASE WHEN " +
      kind_clause +
      "AND online = 1 THEN 1 ELSE 0 END), 0), "
      "COALESCE(SUM(CASE WHEN " +
      kind_clause +
      "AND online = 0 THEN 1 ELSE 0 END), 0), "
      "COALESCE(SUM(CASE WHEN " +
      kind_clause +
      "AND EXISTS ("
      "SELECT 1 FROM remote_control_sessions "
      "WHERE normalized_host_id = device_presence.device_id) "
      "THEN 1 ELSE 0 END), 0), "
      "COALESCE(SUM(CASE WHEN device_id LIKE 'web-%' "
      "THEN 1 ELSE 0 END), 0) "
      "FROM device_presence ";
  if (!search.empty()) {
    sql += "WHERE device_id LIKE ? ESCAPE '\\' ";
  }
  sql += ";";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) !=
      SQLITE_OK) {
    return counts;
  }
  if (!search.empty()) {
    std::string pattern = "%" + EscapeLikePattern(search) + "%";
    sqlite3_bind_text(stmt, 1, pattern.c_str(), -1, SQLITE_TRANSIENT);
  }

  if (sqlite3_step(stmt) == SQLITE_ROW) {
    counts.all = sqlite3_column_int64(stmt, 0);
    counts.online = sqlite3_column_int64(stmt, 1);
    counts.offline = sqlite3_column_int64(stmt, 2);
    counts.active = sqlite3_column_int64(stmt, 3);
    counts.web = sqlite3_column_int64(stmt, 4);
  }
  sqlite3_finalize(stmt);
  return counts;
}

OnlineDurationStats DeviceDBManager::GetOnlineDurationStats() {
  std::lock_guard<std::recursive_mutex> lock(db_mutex_);
  OnlineDurationStats stats;
  if (db_ == nullptr) {
    LOG_ERROR("Database is not initialized in GetOnlineDurationStats.");
    return stats;
  }

  std::string sql =
      "SELECT "
      "COALESCE(SUM(CASE WHEN online = 1 AND online_since > 0 THEN "
      "MAX(0, CAST(strftime('%s','now') AS INTEGER) - online_since) "
      "ELSE 0 END), 0), "
      "COALESCE(SUM(total_online_seconds + "
      "CASE WHEN online = 1 AND online_since > 0 THEN "
      "MAX(0, CAST(strftime('%s','now') AS INTEGER) - online_since) "
      "ELSE 0 END), 0), "
      "COALESCE(SUM(total_control_seconds + COALESCE(("
      "SELECT SUM(MAX(0, CAST(strftime('%s','now') AS INTEGER) - started_at)) "
      "FROM remote_control_sessions "
      "WHERE normalized_guest_id = device_presence.device_id), 0)), 0), "
      "COALESCE(SUM(total_controlled_seconds + COALESCE(("
      "SELECT SUM(MAX(0, CAST(strftime('%s','now') AS INTEGER) - started_at)) "
      "FROM remote_control_sessions "
      "WHERE normalized_host_id = device_presence.device_id), 0)), 0) "
      "FROM device_presence "
      "WHERE device_id NOT LIKE 'web-%' "
      "AND device_id NOT LIKE 'C-%';";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) !=
      SQLITE_OK) {
    return stats;
  }

  if (sqlite3_step(stmt) == SQLITE_ROW) {
    stats.current_online_seconds = sqlite3_column_int64(stmt, 0);
    stats.total_online_seconds = sqlite3_column_int64(stmt, 1);
    stats.total_control_seconds = sqlite3_column_int64(stmt, 2);
    stats.total_controlled_seconds = sqlite3_column_int64(stmt, 3);
  }
  sqlite3_finalize(stmt);
  return stats;
}

ClientGeoDistribution DeviceDBManager::GetClientGeoDistribution() {
  std::lock_guard<std::recursive_mutex> lock(db_mutex_);
  ClientGeoDistribution distribution;
  if (db_ == nullptr) {
    LOG_ERROR("Database is not initialized in GetClientGeoDistribution.");
    return distribution;
  }

  const char* sql =
      "SELECT geo_country, geo_region, geo_location, COUNT(*) "
      "FROM device_presence "
      "WHERE online = 1 "
      "AND device_id NOT LIKE 'C-%' "
      "GROUP BY geo_country, geo_region, geo_location;";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    return distribution;
  }

  std::unordered_map<std::string, int64_t> province_counts;
  std::unordered_map<std::string, int64_t> country_counts;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    std::string country = ColumnText(stmt, 0);
    std::string region = ColumnText(stmt, 1);
    std::string location = ColumnText(stmt, 2);
    int64_t count = sqlite3_column_int64(stmt, 3);
    distribution.total_count += count;

    std::string province = NormalizeChinaProvince(region, location);
    country = Trim(country);
    if (!province.empty()) {
      distribution.domestic_count += count;
      province_counts[province] += count;
    } else if (IsChinaCountry(country)) {
      distribution.domestic_count += count;
    } else if (!country.empty()) {
      distribution.foreign_count += count;
      country_counts[country] += count;
    } else {
      distribution.unknown_count += count;
    }
  }
  sqlite3_finalize(stmt);

  for (const auto& pair : province_counts) {
    distribution.provinces.push_back({pair.first, pair.second});
  }
  for (const auto& pair : country_counts) {
    distribution.countries.push_back({pair.first, pair.second});
  }
  std::sort(distribution.provinces.begin(), distribution.provinces.end(),
            [](const ProvinceUserCount& lhs, const ProvinceUserCount& rhs) {
              if (lhs.count != rhs.count) {
                return lhs.count > rhs.count;
              }
              return lhs.province < rhs.province;
            });
  std::sort(distribution.countries.begin(), distribution.countries.end(),
            [](const CountryUserCount& lhs, const CountryUserCount& rhs) {
              if (lhs.count != rhs.count) {
                return lhs.count > rhs.count;
              }
              return lhs.country < rhs.country;
            });

  return distribution;
}

std::vector<OnlineDeviceInfo> DeviceDBManager::ListOnlineDevices() {
  return ListOnlineDevices(static_cast<size_t>(std::numeric_limits<int>::max()),
                           0, "");
}

std::vector<OnlineDeviceInfo> DeviceDBManager::ListOnlineDevices(
    size_t limit, size_t offset, const std::string& search) {
  std::lock_guard<std::recursive_mutex> lock(db_mutex_);
  std::vector<OnlineDeviceInfo> result;
  if (db_ == nullptr) {
    LOG_ERROR("Database is not initialized in ListOnlineDevices.");
    return result;
  }

  std::string sql =
      "SELECT device_id, online, updated_at, online_since, "
      "CASE WHEN online = 1 AND online_since > 0 THEN "
      "MAX(0, CAST(strftime('%s','now') AS INTEGER) - online_since) "
      "ELSE 0 END AS online_duration_seconds, "
      "total_online_seconds + CASE WHEN online = 1 AND online_since > 0 "
      "THEN MAX(0, CAST(strftime('%s','now') AS INTEGER) - online_since) "
      "ELSE 0 END AS total_online_seconds, "
      "total_control_seconds + COALESCE(("
      "SELECT SUM(MAX(0, CAST(strftime('%s','now') AS INTEGER) - started_at)) "
      "FROM remote_control_sessions "
      "WHERE normalized_guest_id = device_presence.device_id), 0) "
      "AS total_control_seconds, "
      "total_controlled_seconds + COALESCE(("
      "SELECT SUM(MAX(0, CAST(strftime('%s','now') AS INTEGER) - started_at)) "
      "FROM remote_control_sessions "
      "WHERE normalized_host_id = device_presence.device_id), 0) "
      "AS total_controlled_seconds, "
      "client_ip, geo_country, geo_region, geo_city, geo_location, "
      "client_version, client_platform "
      "FROM device_presence "
      "WHERE online = 1 "
      "AND device_id NOT LIKE 'web-%' "
      "AND device_id NOT LIKE 'C-%' ";
  if (!search.empty()) {
    sql += "AND device_id LIKE ? ESCAPE '\\' ";
  }
  sql += "ORDER BY updated_at DESC, device_id ASC LIMIT ? OFFSET ?;";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) !=
      SQLITE_OK) {
    return result;
  }
  int bind_index = 1;
  if (!search.empty()) {
    std::string pattern = "%" + EscapeLikePattern(search) + "%";
    sqlite3_bind_text(stmt, bind_index++, pattern.c_str(), -1,
                      SQLITE_TRANSIENT);
  }
  sqlite3_bind_int64(stmt, bind_index++, static_cast<sqlite3_int64>(limit));
  sqlite3_bind_int64(stmt, bind_index++, static_cast<sqlite3_int64>(offset));

  while (sqlite3_step(stmt) == SQLITE_ROW) {
    OnlineDeviceInfo info;
    info.device_id = ColumnText(stmt, 0);
    info.online = sqlite3_column_int(stmt, 1) != 0;
    info.updated_at = sqlite3_column_int64(stmt, 2);
    info.online_since = sqlite3_column_int64(stmt, 3);
    info.online_duration_seconds = sqlite3_column_int64(stmt, 4);
    info.total_online_seconds = sqlite3_column_int64(stmt, 5);
    info.total_control_seconds = sqlite3_column_int64(stmt, 6);
    info.total_controlled_seconds = sqlite3_column_int64(stmt, 7);
    info.client_ip = ColumnText(stmt, 8);
    info.country = ColumnText(stmt, 9);
    info.region = ColumnText(stmt, 10);
    info.city = ColumnText(stmt, 11);
    info.location = ColumnText(stmt, 12);
    info.client_version = ColumnText(stmt, 13);
    info.client_platform = ColumnText(stmt, 14);
    result.push_back(info);
  }
  sqlite3_finalize(stmt);

  return result;
}

std::vector<OnlineDeviceInfo> DeviceDBManager::ListDevicePresence(
    size_t limit, size_t offset, const std::string& search,
    const std::string& filter, const std::string& sort,
    const std::string& order, const std::string& kind) {
  std::lock_guard<std::recursive_mutex> lock(db_mutex_);
  std::vector<OnlineDeviceInfo> result;
  if (db_ == nullptr) {
    LOG_ERROR("Database is not initialized in ListDevicePresence.");
    return result;
  }

  std::string sql =
      "SELECT device_id, online, updated_at, online_since, "
      "CASE WHEN online = 1 AND online_since > 0 THEN "
      "MAX(0, CAST(strftime('%s','now') AS INTEGER) - online_since) "
      "ELSE 0 END AS online_duration_seconds, "
      "total_online_seconds + CASE WHEN online = 1 AND online_since > 0 "
      "THEN MAX(0, CAST(strftime('%s','now') AS INTEGER) - online_since) "
      "ELSE 0 END AS total_online_seconds, "
      "total_control_seconds + COALESCE(("
      "SELECT SUM(MAX(0, CAST(strftime('%s','now') AS INTEGER) - started_at)) "
      "FROM remote_control_sessions "
      "WHERE normalized_guest_id = device_presence.device_id), 0) "
      "AS total_control_seconds, "
      "total_controlled_seconds + COALESCE(("
      "SELECT SUM(MAX(0, CAST(strftime('%s','now') AS INTEGER) - started_at)) "
      "FROM remote_control_sessions "
      "WHERE normalized_host_id = device_presence.device_id), 0) "
      "AS total_controlled_seconds, "
      "COALESCE(("
      "SELECT SUM(MAX(0, CAST(strftime('%s','now') AS INTEGER) - started_at)) "
      "FROM remote_control_sessions "
      "WHERE normalized_guest_id = device_presence.device_id), 0) "
      "AS current_control_seconds, "
      "COALESCE(("
      "SELECT SUM(MAX(0, CAST(strftime('%s','now') AS INTEGER) - started_at)) "
      "FROM remote_control_sessions "
      "WHERE normalized_host_id = device_presence.device_id), 0) "
      "AS current_controlled_seconds, "
      "COALESCE(("
      "SELECT COUNT(*) FROM remote_control_sessions "
      "WHERE normalized_guest_id = device_presence.device_id), 0) "
      "AS active_control_count, "
      "COALESCE(("
      "SELECT COUNT(*) FROM remote_control_sessions "
      "WHERE normalized_host_id = device_presence.device_id), 0) "
      "AS active_controlled_count, "
      "COALESCE(("
      "SELECT GROUP_CONCAT(DISTINCT normalized_host_id) "
      "FROM remote_control_sessions "
      "WHERE normalized_guest_id = device_presence.device_id), '') "
      "AS active_control_targets, "
      "COALESCE(("
      "SELECT GROUP_CONCAT(DISTINCT normalized_guest_id) "
      "FROM remote_control_sessions "
      "WHERE normalized_host_id = device_presence.device_id), '') "
      "AS active_controlled_by, "
      "client_ip, geo_country, geo_region, geo_city, geo_location, "
      "client_version, client_platform "
      "FROM device_presence "
      "WHERE " +
      DevicePresenceFilterClause(filter, kind);
  if (!search.empty()) {
    sql += "AND device_id LIKE ? ESCAPE '\\' ";
  }
  sql += "ORDER BY " + DevicePresenceSortClause(sort, order) +
         "LIMIT ? OFFSET ?;";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) !=
      SQLITE_OK) {
    return result;
  }
  int bind_index = 1;
  if (!search.empty()) {
    std::string pattern = "%" + EscapeLikePattern(search) + "%";
    sqlite3_bind_text(stmt, bind_index++, pattern.c_str(), -1,
                      SQLITE_TRANSIENT);
  }
  sqlite3_bind_int64(stmt, bind_index++, static_cast<sqlite3_int64>(limit));
  sqlite3_bind_int64(stmt, bind_index++, static_cast<sqlite3_int64>(offset));

  while (sqlite3_step(stmt) == SQLITE_ROW) {
    OnlineDeviceInfo info;
    info.device_id = ColumnText(stmt, 0);
    info.online = sqlite3_column_int(stmt, 1) != 0;
    info.updated_at = sqlite3_column_int64(stmt, 2);
    info.online_since = sqlite3_column_int64(stmt, 3);
    info.online_duration_seconds = sqlite3_column_int64(stmt, 4);
    info.total_online_seconds = sqlite3_column_int64(stmt, 5);
    info.total_control_seconds = sqlite3_column_int64(stmt, 6);
    info.total_controlled_seconds = sqlite3_column_int64(stmt, 7);
    info.current_control_seconds = sqlite3_column_int64(stmt, 8);
    info.current_controlled_seconds = sqlite3_column_int64(stmt, 9);
    info.active_control_count = sqlite3_column_int64(stmt, 10);
    info.active_controlled_count = sqlite3_column_int64(stmt, 11);
    info.active_control_targets = SplitCommaSeparatedIds(ColumnText(stmt, 12));
    info.active_controlled_by = SplitCommaSeparatedIds(ColumnText(stmt, 13));
    info.client_ip = ColumnText(stmt, 14);
    info.country = ColumnText(stmt, 15);
    info.region = ColumnText(stmt, 16);
    info.city = ColumnText(stmt, 17);
    info.location = ColumnText(stmt, 18);
    info.client_version = ColumnText(stmt, 19);
    info.client_platform = ColumnText(stmt, 20);
    result.push_back(info);
  }
  sqlite3_finalize(stmt);

  return result;
}

std::vector<std::pair<std::string, bool>> DeviceDBManager::BatchQueryOnline(
    const std::vector<std::string>& device_ids) {
  std::lock_guard<std::recursive_mutex> lock(db_mutex_);
  std::vector<std::pair<std::string, bool>> result;
  if (db_ == nullptr) {
    LOG_ERROR("Database is not initialized in BatchQueryOnline.");
    return result;
  }
  if (device_ids.empty()) {
    return result;
  }

  std::stringstream ss;
  ss << "SELECT device_id, online FROM device_presence WHERE device_id IN (";
  for (size_t i = 0; i < device_ids.size(); ++i) {
    ss << (i == 0 ? "?" : ",?");
  }
  ss << ");";
  std::string sql = ss.str();

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
    return result;
  }
  for (size_t i = 0; i < device_ids.size(); ++i) {
    sqlite3_bind_text(stmt, static_cast<int>(i + 1), device_ids[i].c_str(), -1,
                      SQLITE_TRANSIENT);
  }

  std::unordered_map<std::string, bool> map;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    std::string id = ColumnText(stmt, 0);
    int online = sqlite3_column_int(stmt, 1);
    map[id] = (online != 0);
  }
  sqlite3_finalize(stmt);

  for (const auto& id : device_ids) {
    auto it = map.find(id);
    bool online = (it != map.end()) ? it->second : false;
    result.emplace_back(id, online);
  }
  return result;
}

bool DeviceDBManager::SetUserDevices(
    const std::string& user_id, const std::vector<std::string>& device_ids) {
  std::lock_guard<std::recursive_mutex> lock(db_mutex_);
  if (db_ == nullptr) {
    LOG_ERROR("Database is not initialized in SetUserDevices.");
    return false;
  }

  char* err_msg = nullptr;
  if (sqlite3_exec(db_, "BEGIN TRANSACTION;", nullptr, nullptr, &err_msg) !=
      SQLITE_OK) {
    LOG_ERROR("Failed to begin SetUserDevices transaction: {}",
              err_msg ? err_msg : sqlite3_errmsg(db_));
    sqlite3_free(err_msg);
    return false;
  }

  const char* delete_sql = "DELETE FROM user_devices WHERE user_id = ?;";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, delete_sql, -1, &stmt, nullptr) != SQLITE_OK) {
    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    return false;
  }
  sqlite3_bind_text(stmt, 1, user_id.c_str(), -1, SQLITE_TRANSIENT);
  bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
  sqlite3_finalize(stmt);

  if (!ok) {
    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    return false;
  }

  const char* insert_sql =
      "INSERT OR IGNORE INTO user_devices (user_id, device_id) VALUES (?, ?);";
  if (sqlite3_prepare_v2(db_, insert_sql, -1, &stmt, nullptr) != SQLITE_OK) {
    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    return false;
  }

  for (const auto& device_id : device_ids) {
    if (device_id.empty()) {
      continue;
    }
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    sqlite3_bind_text(stmt, 1, user_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, device_id.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
      ok = false;
      break;
    }
  }
  sqlite3_finalize(stmt);

  if (!ok) {
    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    return false;
  }

  if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, &err_msg) != SQLITE_OK) {
    LOG_ERROR("Failed to commit SetUserDevices transaction: {}",
              err_msg ? err_msg : sqlite3_errmsg(db_));
    sqlite3_free(err_msg);
    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    return false;
  }

  return true;
}

std::vector<std::string> DeviceDBManager::GetUserDevices(
    const std::string& user_id) {
  std::lock_guard<std::recursive_mutex> lock(db_mutex_);
  std::vector<std::string> result;
  if (db_ == nullptr) {
    LOG_ERROR("Database is not initialized in GetUserDevices.");
    return result;
  }

  const char* sql =
      "SELECT device_id FROM user_devices WHERE user_id = ? ORDER BY "
      "device_id;";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    return result;
  }

  sqlite3_bind_text(stmt, 1, user_id.c_str(), -1, SQLITE_TRANSIENT);
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    result.push_back(ColumnText(stmt, 0));
  }
  sqlite3_finalize(stmt);

  return result;
}
