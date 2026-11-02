#include "device_db_manager.h"
#include "controller_identity.h"

#include <memory>
#include <stdexcept>

namespace {
using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;

Statement Prepare(sqlite3* db, const std::string& sql) {
  sqlite3_stmt* raw = nullptr;
  if (sqlite3_prepare_v2(db, sql.c_str(), -1, &raw, nullptr) != SQLITE_OK)
    throw std::runtime_error(sqlite3_errmsg(db));
  return Statement(raw, sqlite3_finalize);
}

void Exec(sqlite3* db, const char* sql) {
  if (sqlite3_exec(db, sql, nullptr, nullptr, nullptr) != SQLITE_OK)
    throw std::runtime_error(sqlite3_errmsg(db));
}
}  // namespace

RetentionCleanupResult DeviceDBManager::CleanupTemporaryControllers(
    size_t batch_size, const std::string& device_id) {
  if (batch_size < 1 || batch_size > 1000 ||
      (!device_id.empty() && !IsTemporaryControllerId(device_id)))
    throw std::invalid_argument("Invalid temporary controller cleanup request");
  std::lock_guard<std::recursive_mutex> lock(db_mutex_);
  RetentionCleanupResult result;
  Exec(db_, "BEGIN IMMEDIATE;");
  try {
    // Prefix ranges use the ID indexes. UNION ALL can stop at the batch limit
    // without sorting/materializing the entire legacy backlog. Registrations
    // without presence also cover interrupted logins and failed bindings.
    const std::string candidates = device_id.empty()
        ? "SELECT device_id FROM device_presence WHERE online=0 AND "
          "(device_id GLOB 'web-*' OR device_id GLOB 'cli-*') "
          "UNION ALL SELECT d.device_id FROM devices d WHERE "
          "(d.device_id GLOB 'web-*' OR d.device_id GLOB 'cli-*') "
          "AND NOT EXISTS (SELECT 1 FROM device_presence p "
          "WHERE p.device_id=d.device_id)"
        : "SELECT ?2 AS device_id";
    {
      auto query = Prepare(db_,
          "SELECT t.device_id FROM (" + candidates + ") t "
          "WHERE NOT EXISTS (SELECT 1 FROM device_presence p "
          "WHERE p.device_id IN (t.device_id,'C-'||t.device_id) AND p.online<>0) "
          "AND NOT EXISTS (SELECT 1 FROM remote_control_sessions s "
          "WHERE s.normalized_host_id=t.device_id OR s.normalized_guest_id=t.device_id) "
          "LIMIT ?1;");
      sqlite3_bind_int64(query.get(), 1, static_cast<sqlite3_int64>(batch_size));
      if (!device_id.empty())
        sqlite3_bind_text(query.get(), 2, device_id.c_str(), -1, SQLITE_TRANSIENT);
      int rc;
      while ((rc = sqlite3_step(query.get())) == SQLITE_ROW)
        result.device_ids.emplace_back(
            reinterpret_cast<const char*>(sqlite3_column_text(query.get(), 0)));
      if (rc != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(db_));
    }

    auto associations = Prepare(db_, "DELETE FROM user_devices "
        "WHERE user_id IN (?1,?2) OR device_id IN (?1,?2);");
    auto presence = Prepare(db_,
        "DELETE FROM device_presence WHERE device_id IN (?1,?2);");
    auto credentials = Prepare(db_, "DELETE FROM devices WHERE device_id IN (?1,?2);");
    for (const auto& id : result.device_ids) {
      const auto clone = "C-" + id;
      for (auto* statement : {associations.get(), presence.get(), credentials.get()}) {
        sqlite3_bind_text(statement, 1, id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(statement, 2, clone.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(statement) != SQLITE_DONE)
          throw std::runtime_error(sqlite3_errmsg(db_));
        const auto removed = sqlite3_changes(db_);
        if (statement == associations.get()) result.associations += removed;
        else if (statement == presence.get()) result.devices += removed;
        else result.credentials += removed;
        sqlite3_reset(statement);
      }
    }
    // A disconnect must not subtract a controller that was active today.
    // Daily activity has its own two-calendar-day retention policy.
    result.more = device_id.empty() && result.device_ids.size() == batch_size;
    Exec(db_, "COMMIT;");
  } catch (...) {
    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    throw;
  }
  if (device_id.empty() && (result.devices || result.associations || result.credentials))
    sqlite3_wal_checkpoint_v2(db_, nullptr, SQLITE_CHECKPOINT_PASSIVE, nullptr, nullptr);
  return result;
}
