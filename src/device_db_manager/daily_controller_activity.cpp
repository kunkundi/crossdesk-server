#include "device_db_manager.h"

#include <stdexcept>

namespace {
constexpr int64_t kDay = 86400;
constexpr int64_t kBeijingOffset = 8 * 3600;

int64_t Now() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch()).count();
}

int64_t DayStart(int64_t now) {
  return (now + kBeijingOffset) / kDay * kDay - kBeijingOffset;
}

class Statement {
 public:
  Statement(sqlite3* db, const char* sql) : db_(db) {
    if (sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr) != SQLITE_OK)
      throw std::runtime_error(sqlite3_errmsg(db));
  }
  ~Statement() { sqlite3_finalize(stmt_); }
  void Bind(int index, int64_t value) { sqlite3_bind_int64(stmt_, index, value); }
  void Bind(int index, const std::string& value) {
    sqlite3_bind_text(stmt_, index, value.c_str(), -1, SQLITE_TRANSIENT);
  }
  bool Row() {
    const int rc = sqlite3_step(stmt_);
    if (rc != SQLITE_ROW && rc != SQLITE_DONE)
      throw std::runtime_error(sqlite3_errmsg(db_));
    return rc == SQLITE_ROW;
  }
  int64_t Number(int column) { return sqlite3_column_int64(stmt_, column); }
  std::string Text(int column) {
    const auto* text = sqlite3_column_text(stmt_, column);
    return text ? reinterpret_cast<const char*>(text) : "";
  }

 private:
  sqlite3* db_;
  sqlite3_stmt* stmt_ = nullptr;
};

void Exec(sqlite3* db, const char* sql) {
  if (sqlite3_exec(db, sql, nullptr, nullptr, nullptr) != SQLITE_OK)
    throw std::runtime_error(sqlite3_errmsg(db));
}
}  // namespace

void DeviceDBManager::InitDailyControllerActivity() {
  Exec(db_, "BEGIN IMMEDIATE;");
  try {
    Exec(db_,
        "CREATE TABLE IF NOT EXISTS controller_activity_tracking ("
        "id INTEGER PRIMARY KEY CHECK(id=1), started_at INTEGER NOT NULL);"
        "CREATE TABLE IF NOT EXISTS daily_controller_activity ("
        "day_start INTEGER NOT NULL, device_id TEXT NOT NULL,"
        "kind TEXT NOT NULL CHECK(kind IN ('pc','mobile','web')),"
        "PRIMARY KEY(day_start,device_id));"
        "CREATE INDEX IF NOT EXISTS idx_daily_controller_device "
        "ON daily_controller_activity(device_id,day_start);"
        // A persisted row alone is not proof of activity after a restart.
        "UPDATE remote_control_sessions SET activity_confirmed=0;");
    Statement tracking(db_, "INSERT OR IGNORE INTO controller_activity_tracking "
                            "(id,started_at) VALUES (1,?);");
    tracking.Bind(1, Now());
    tracking.Row();
    Exec(db_, "COMMIT;");
  } catch (...) {
    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    throw;
  }
}

void DeviceDBManager::RecordDailyControllerActivity(
    int64_t now, const std::string& transmission_id, const std::string& guest_id) {
  std::string sql =
      "INSERT INTO daily_controller_activity(day_start,device_id,kind) "
      "SELECT DISTINCT ?1,s.normalized_guest_id,"
      "CASE WHEN s.normalized_guest_id LIKE 'web-%' THEN 'web' "
      "WHEN lower(p.client_platform) IN ('ios','android') THEN 'mobile' "
      "WHEN coalesce(p.client_platform,'')<>'' THEN 'pc' "
      // Login clears metadata before the client reports its platform. Preserve
      // the last known category across this gap, including at midnight.
      "ELSE coalesce((SELECT a.kind FROM daily_controller_activity a "
      "WHERE a.device_id=s.normalized_guest_id ORDER BY a.day_start DESC LIMIT 1),"
      "'pc') END "
      "FROM remote_control_sessions s LEFT JOIN device_presence p "
      "ON p.device_id=s.normalized_guest_id "
      "WHERE s.activity_confirmed=1 AND s.normalized_guest_id<>'' ";
  if (!transmission_id.empty())
    sql += "AND s.transmission_id=?2 AND s.guest_id=?3 ";
  sql +=
      "ON CONFLICT(day_start,device_id) DO UPDATE SET kind=excluded.kind "
      "WHERE daily_controller_activity.kind<>excluded.kind;";
  Statement record(db_, sql.c_str());
  record.Bind(1, DayStart(now));
  if (!transmission_id.empty()) {
    record.Bind(2, transmission_id);
    record.Bind(3, guest_id);
  }
  record.Row();
}

void DeviceDBManager::RefreshDailyControllerActivity(int64_t now) {
  const auto today = DayStart(now);
  if (today == activity_day_) return;
  Exec(db_, "BEGIN IMMEDIATE;");
  try {
    RecordDailyControllerActivity(now);
    Statement prune(db_, "DELETE FROM daily_controller_activity WHERE day_start<?;");
    prune.Bind(1, today - kDay);
    prune.Row();
    Exec(db_, "COMMIT;");
    activity_day_ = today;
  } catch (...) {
    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    throw;
  }
}

void DeviceDBManager::UpdateDailyControllerKind(const std::string& device_id,
                                              const std::string& platform) {
  const auto id = device_id.rfind("C-", 0) == 0 ? device_id.substr(2) : device_id;
  Statement update(db_,
      "UPDATE daily_controller_activity SET kind="
      "CASE WHEN device_id LIKE 'web-%' THEN 'web' "
      "WHEN lower(?1) IN ('ios','android') THEN 'mobile' ELSE 'pc' END "
      "WHERE day_start=?2 AND device_id=?3;");
  update.Bind(1, platform);
  update.Bind(2, DayStart(Now()));
  update.Bind(3, id);
  update.Row();
}

nlohmann::json DeviceDBManager::GetDailyControllerStats() {
  std::lock_guard<std::recursive_mutex> lock(db_mutex_);
  const auto today = DayStart(Now());
  // One read snapshot; the reader never inserts records or performs maintenance.
  Statement query(db_,
      "SELECT t.started_at,a.day_start,a.kind,count(a.device_id) "
      "FROM controller_activity_tracking t LEFT JOIN daily_controller_activity a "
      "ON a.day_start IN (?1,?2) WHERE t.id=1 GROUP BY a.day_start,a.kind;");
  query.Bind(1, today);
  query.Bind(2, today - kDay);
  nlohmann::json result = {{"timezone", "Asia/Shanghai"},
                           {"started_at", nullptr},
                           {"today", nullptr}, {"yesterday", nullptr}};
  while (query.Row()) {
    if (result["started_at"].is_null()) {
      const auto started = query.Number(0);
      result["started_at"] = started;
      for (const auto& period : {"today", "yesterday"}) {
        const auto day = today - (std::string(period) == "yesterday" ? kDay : 0);
        if (started < day + kDay)
          result[period] = {{"pc", 0}, {"mobile", 0}, {"web", 0},
                            {"partial", started > day}};
      }
    }
    const auto kind = query.Text(2);
    if (kind.empty()) continue;
    const auto period = query.Number(1) == today ? "today" : "yesterday";
    if (!result[period].is_null()) result[period][kind] = query.Number(3);
  }
  return result;
}
