#include <openssl/sha.h>

#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>

#include "device_db_manager.h"

namespace {
using json = nlohmann::json;

class Statement {
 public:
  Statement(sqlite3* db, const char* sql) {
    if (sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr) != SQLITE_OK)
      throw std::runtime_error("Admin data statement preparation failed");
  }
  ~Statement() { sqlite3_finalize(stmt_); }
  void Bind(int index, const std::string& value) {
    if (sqlite3_bind_text(stmt_, index, value.c_str(), -1, SQLITE_TRANSIENT) !=
        SQLITE_OK)
      throw std::runtime_error("Admin data binding failed");
  }
  void Bind(int index, int64_t value) {
    if (sqlite3_bind_int64(stmt_, index, value) != SQLITE_OK)
      throw std::runtime_error("Admin data binding failed");
  }
  bool Row() {
    const int rc = sqlite3_step(stmt_);
    if (rc != SQLITE_ROW && rc != SQLITE_DONE)
      throw std::runtime_error("Admin data operation failed");
    return rc == SQLITE_ROW;
  }
  std::string Text(int col) const {
    const auto* text = sqlite3_column_text(stmt_, col);
    return text ? reinterpret_cast<const char*>(text) : "";
  }
  int64_t Number(int col) const { return sqlite3_column_int64(stmt_, col); }

 private:
  sqlite3_stmt* stmt_ = nullptr;
};

class WorkBudget {
 public:
  explicit WorkBudget(sqlite3* db)
      : db_(db),
        deadline_(std::chrono::steady_clock::now() + std::chrono::seconds(1)) {
    sqlite3_progress_handler(
        db_, 1000,
        [](void* self) {
          return std::chrono::steady_clock::now() >=
                         static_cast<WorkBudget*>(self)->deadline_
                     ? 1
                     : 0;
        },
        this);
  }
  ~WorkBudget() { sqlite3_progress_handler(db_, 0, nullptr, nullptr); }

 private:
  sqlite3* db_;
  std::chrono::steady_clock::time_point deadline_;
};

class Transaction {
 public:
  explicit Transaction(sqlite3* db) : db_(db) { Exec("BEGIN IMMEDIATE;"); }
  ~Transaction() {
    if (!committed_) sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
  }
  void Commit() {
    Exec("COMMIT;");
    committed_ = true;
  }

 private:
  void Exec(const char* sql) {
    if (sqlite3_exec(db_, sql, nullptr, nullptr, nullptr) != SQLITE_OK)
      throw std::runtime_error("Admin data transaction failed");
  }
  sqlite3* db_;
  bool committed_ = false;
};

bool Identifier(const std::string& value) {
  if (value.empty() || value.size() > 128) return false;
  for (char c : value)
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '-' || c == '_'))
      return false;
  return true;
}

int64_t Now() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string Digest(const json& value) {
  const auto input = value.dump();
  unsigned char bytes[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const unsigned char*>(input.data()), input.size(),
         bytes);
  std::ostringstream out;
  for (auto b : bytes)
    out << std::hex << std::setfill('0') << std::setw(2) << unsigned(b);
  return out.str();
}

json Error(const char* error) { return {{"ok", false}, {"error", error}}; }

// Bound the work on the signaling writer. Very large device records require
// supervised offline processing, never a silently truncated export/deletion.
constexpr size_t kMaxRelatedRows = 10000;
}  // namespace

nlohmann::json DeviceDBManager::AdminDeviceData(
    const std::string& id, const std::string& action, const std::string& scope,
    const std::string& expected_revision, const std::string& actor,
    const std::string& request_ref) {
  if (!Identifier(id) || id.rfind("C-", 0) == 0 || !Identifier(request_ref) ||
      request_ref.size() > 64 || actor.empty() ||
      (action != "query" && action != "export" && action != "cleanup") ||
      (action == "cleanup" && scope != "history" && scope != "identity"))
    return Error("invalid_request");
  std::lock_guard<std::recursive_mutex> lock(db_mutex_);
  WorkBudget budget(db_);
  Transaction transaction(db_);
  const std::string clone = "C-" + id;
  json credentials = json::array(), presence = json::array();
  json associations = json::array(), sessions = json::array();
  {
    Statement s(db_,
                "SELECT device_id,password_salt,password_hash FROM devices "
                "WHERE device_id IN (?,?) ORDER BY device_id;");
    s.Bind(1, id);
    s.Bind(2, clone);
    while (s.Row()) credentials.push_back({s.Text(0), s.Text(1), s.Text(2)});
  }
  bool online = false;
  {
    Statement s(
        db_,
        "SELECT device_id,online,updated_at,online_since,"
        "client_version,client_platform FROM device_presence "
        "WHERE device_id IN (?,?) ORDER BY device_id;");
    s.Bind(1, id);
    s.Bind(2, clone);
    while (s.Row()) {
      online = online || s.Number(1) != 0;
      presence.push_back({{"device_id", s.Text(0)},
                          {"online", s.Number(1) != 0},
                          {"updated_at", s.Number(2)},
                          {"online_since", s.Number(3)},
                          {"client_version", s.Text(4)},
                          {"client_platform", s.Text(5)}});
    }
  }
  {
    Statement s(db_,
                "SELECT user_id,device_id,updated_at FROM user_devices "
                "WHERE user_id IN (?1,?2) OR device_id IN (?1,?2) "
                "ORDER BY user_id,device_id LIMIT 10001;");
    s.Bind(1, id);
    s.Bind(2, clone);
    while (s.Row()) associations.push_back({s.Text(0), s.Text(1), s.Number(2)});
  }
  {
    Statement s(db_,
                "SELECT transmission_id,guest_id,host_id,started_at "
                "FROM remote_control_sessions WHERE normalized_guest_id=?1 "
                "OR normalized_host_id=?1 ORDER BY transmission_id,guest_id "
                "LIMIT 10001;");
    s.Bind(1, id);
    while (s.Row())
      sessions.push_back({s.Text(0), s.Text(1), s.Text(2), s.Number(3)});
  }
  if (associations.size() > kMaxRelatedRows ||
      sessions.size() > kMaxRelatedRows)
    return Error("device_data_too_large");
  if (credentials.empty() && presence.empty() && associations.empty() &&
      sessions.empty())
    return Error("device_not_found");

  // The revision binds confirmation to the complete related DB state, including
  // credentials. Neither secrets nor other participants' identifiers are
  // returned.
  const auto revision =
      Digest({id, credentials, presence, associations, sessions});
  json report = {{"device_id", id},
                 {"revision", revision},
                 {"credential_records", credentials.size()},
                 {"presence", presence},
                 {"association_records", associations.size()},
                 {"active_session_records", sessions.size()},
                 {"cleanup_allowed", !online && sessions.empty()}};
  json own_sessions = json::array();
  for (const auto& s : sessions)
    own_sessions.push_back(
        {{"role", (s[2] == id || s[2] == clone) ? "host" : "guest"},
         {"started_at", s[3]}});
  report["sessions"] = own_sessions;
  report["scope"] = "device_database_summary";
  report["excluded"] = {"passwords_and_hashes",
                        "other_device_identifiers",
                        "legacy_geolocation",
                        "runtime_ip_and_subscriptions",
                        "logs",
                        "backups"};

  json removed = json::object();
  if (action == "cleanup") {
    if (online || !sessions.empty()) return Error("device_busy");
    if (expected_revision.empty() || expected_revision != revision)
      return Error("stale_preview");
    for (const auto& item : std::vector<std::pair<std::string, const char*>>{
             {"associations",
              "DELETE FROM user_devices WHERE user_id IN "
              "(?1,?2) OR device_id IN (?1,?2);"},
             {"presence",
              "DELETE FROM device_presence WHERE device_id IN (?1,?2);"}}) {
      Statement s(db_, item.second);
      s.Bind(1, id);
      s.Bind(2, clone);
      s.Row();
      removed[item.first] = sqlite3_changes(db_);
    }
    if (scope == "identity") {
      Statement s(db_, "DELETE FROM devices WHERE device_id IN (?,?);");
      s.Bind(1, id);
      s.Bind(2, clone);
      s.Row();
      removed["credentials"] = sqlite3_changes(db_);
    } else {
      // History cleanup removes the only known offline timestamp. Keep the
      // identity for a fresh retention period instead of expiring it at once.
      Statement s(db_, "UPDATE devices SET retention_started_at=? "
                        "WHERE device_id IN (?,?);");
      s.Bind(1, Now());
      s.Bind(2, id);
      s.Bind(3, clone);
      s.Row();
      removed["credentials"] = 0;
    }
  }

  // No raw device ID, IP, password, data snapshot, or verification evidence is
  // retained in the audit table. The opaque support reference links the case.
  Statement audit(db_,
                  "INSERT INTO admin_data_audit "
                  "(created_at,actor,request_ref,action,scope,result) "
                  "VALUES (?,?,?,?,?,?);");
  const int64_t now = Now();
  audit.Bind(1, now);
  audit.Bind(2, actor);
  audit.Bind(3, request_ref);
  audit.Bind(4, action);
  audit.Bind(5, action == "cleanup" ? scope : "summary");
  audit.Bind(6, action == "cleanup" ? removed.dump() : "{}");
  audit.Row();
  const auto audit_id = sqlite3_last_insert_rowid(db_);
  transaction.Commit();
  if (action == "cleanup") {
    sqlite3_wal_checkpoint_v2(db_, nullptr, SQLITE_CHECKPOINT_PASSIVE, nullptr,
                              nullptr);
    return {{"ok", true},
            {"audit_id", audit_id},
            {"removed", removed},
            {"scope", scope},
            {"logs_and_backups_deleted", false}};
  }
  report["generated_at"] = now;
  return {{"ok", true}, {"audit_id", audit_id}, {"data", report}};
}

bool DeviceDBManager::CleanupAdminDataAudit(int retention_days) {
  if (retention_days < 1 || retention_days > 3650)
    throw std::invalid_argument("Invalid admin audit retention");
  std::lock_guard<std::recursive_mutex> lock(db_mutex_);
  Statement s(db_,
              "DELETE FROM admin_data_audit WHERE id IN "
              "(SELECT id FROM admin_data_audit WHERE created_at < ? "
              "ORDER BY created_at LIMIT 200);");
  s.Bind(1, Now() - int64_t{retention_days} * 86400);
  s.Row();
  return sqlite3_changes(db_) == 200;
}
