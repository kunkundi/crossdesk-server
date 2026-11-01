#include <algorithm>
#include <memory>

#include "device_db_manager.h"

namespace {
using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;
Statement Prepare(sqlite3* db, const char* sql) {
  sqlite3_stmt* raw = nullptr;
  if (!db || sqlite3_prepare_v2(db, sql, -1, &raw, nullptr) != SQLITE_OK) {
    sqlite3_finalize(raw);
    raw = nullptr;
  }
  return Statement(raw, sqlite3_finalize);
}
void Bind(sqlite3_stmt* stmt, int index, const std::string& text) {
  sqlite3_bind_text(stmt, index, text.c_str(), -1, SQLITE_TRANSIENT);
}
std::string Text(sqlite3_stmt* stmt, int index) {
  const auto* value = sqlite3_column_text(stmt, index);
  return value ? reinterpret_cast<const char*>(value) : "";
}
int64_t Now() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}
}  // namespace

nlohmann::json DeviceDBManager::RiskRules() {
  std::lock_guard<std::recursive_mutex> lock(db_mutex_);
  auto stmt =
      Prepare(db_,
              "SELECT scope,target,expires_at,reason FROM auth_risk_rules "
              "WHERE expires_at>? ORDER BY scope,target LIMIT 4096;");
  if (!stmt) return nullptr;
  sqlite3_bind_int64(stmt.get(), 1, Now());
  auto rows = nlohmann::json::array();
  int rc;
  while ((rc = sqlite3_step(stmt.get())) == SQLITE_ROW)
    rows.push_back({{"scope", Text(stmt.get(), 0)},
                    {"target", Text(stmt.get(), 1)},
                    {"expires_at", sqlite3_column_int64(stmt.get(), 2)},
                    {"reason", Text(stmt.get(), 3)}});
  return rc == SQLITE_DONE ? rows : nlohmann::json(nullptr);
}

bool DeviceDBManager::SetRiskRule(const std::string& scope,
                                  const std::string& target, int64_t expires_at,
                                  const std::string& reason) {
  if ((scope != "device" && scope != "ip") || target.empty() ||
      target.size() > 128 || target.find('\0') != std::string::npos ||
      reason.size() > 256 || expires_at <= Now() ||
      expires_at > Now() + 30 * 86400)
    return false;
  std::lock_guard<std::recursive_mutex> lock(db_mutex_);
  if (!db_ || sqlite3_exec(db_,
                           "DELETE FROM auth_risk_rules WHERE "
                           "expires_at<=CAST(strftime('%s','now') AS INTEGER);",
                           nullptr, nullptr, nullptr) != SQLITE_OK)
    return false;
  auto stmt = Prepare(
      db_,
      "INSERT INTO auth_risk_rules(scope,target,expires_at,reason) "
      "SELECT ?1,?2,?3,?4 WHERE (SELECT COUNT(*) FROM auth_risk_rules)<4096 "
      "OR EXISTS(SELECT 1 FROM auth_risk_rules WHERE scope=?1 AND target=?2) "
      "ON CONFLICT(scope,target) DO UPDATE SET "
      "expires_at=excluded.expires_at,reason=excluded.reason;");
  if (!stmt) return false;
  Bind(stmt.get(), 1, scope);
  Bind(stmt.get(), 2, target);
  sqlite3_bind_int64(stmt.get(), 3, expires_at);
  Bind(stmt.get(), 4, reason);
  return sqlite3_step(stmt.get()) == SQLITE_DONE && sqlite3_changes(db_) == 1;
}

bool DeviceDBManager::DeleteRiskRule(const std::string& scope,
                                     const std::string& target) {
  std::lock_guard<std::recursive_mutex> lock(db_mutex_);
  auto stmt =
      Prepare(db_, "DELETE FROM auth_risk_rules WHERE scope=? AND target=?;");
  if (!stmt) return false;
  Bind(stmt.get(), 1, scope);
  Bind(stmt.get(), 2, target);
  return sqlite3_step(stmt.get()) == SQLITE_DONE;
}

std::optional<int> DeviceDBManager::PasswordRiskDelay(
    const std::string& source, const std::string& device_id) {
  std::lock_guard<std::recursive_mutex> lock(db_mutex_);
  auto stmt = Prepare(
      db_,
      "SELECT MAX(expires_at) FROM auth_risk_rules "
      "WHERE (scope='ip' AND target=?1) OR (scope='device' AND target=?2);");
  if (!stmt) return std::nullopt;
  Bind(stmt.get(), 1, source);
  Bind(stmt.get(), 2, device_id);
  if (sqlite3_step(stmt.get()) != SQLITE_ROW) return std::nullopt;
  const auto delay = sqlite3_column_int64(stmt.get(), 0) - Now();
  return static_cast<int>(std::clamp<int64_t>(delay, 0, 30 * 86400));
}
