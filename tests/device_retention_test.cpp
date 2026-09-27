#include "device_db_manager.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {
constexpr int64_t kDay = 86400;

int64_t Now() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch()).count();
}

class Fixture {
 public:
  explicit Fixture(bool legacy = false) {
    static int sequence = 0;
    path_ = std::filesystem::temp_directory_path() /
        ("crossdesk_retention_" + std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()) +
         "_" + std::to_string(++sequence) + ".db");
    if (sqlite3_open(path_.string().c_str(), &connection_) != SQLITE_OK)
      throw std::runtime_error("Cannot open fixture");
    if (legacy) {
      Exec("CREATE TABLE devices (id INTEGER PRIMARY KEY AUTOINCREMENT, "
           "device_id TEXT UNIQUE NOT NULL, password_salt TEXT NOT NULL, "
           "password_hash TEXT NOT NULL);"
           "INSERT INTO devices(device_id,password_salt,password_hash) "
           "VALUES ('legacy','salt','hash');");
    }
    Restart();
  }
  ~Fixture() {
    db.reset();
    sqlite3_close(connection_);
    for (const auto* suffix : {"", "-wal", "-shm"})
      std::filesystem::remove(path_.string() + suffix);
  }
  void Restart() {
    db.reset();
    db = std::make_unique<DeviceDBManager>(path_.string());
  }
  void Exec(const std::string& sql) {
    if (sqlite3_exec(connection_, sql.c_str(), nullptr, nullptr, nullptr) != SQLITE_OK)
      throw std::runtime_error(sqlite3_errmsg(connection_));
  }
  int64_t Scalar(const std::string& sql) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(connection_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK)
      throw std::runtime_error(sqlite3_errmsg(connection_));
    if (sqlite3_step(stmt) != SQLITE_ROW) {
      sqlite3_finalize(stmt);
      throw std::runtime_error("Cannot read fixture");
    }
    const auto result = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return result;
  }
  // IDs below are fixed test inputs, never external SQL input.
  void Credential(const std::string& id) {
    Exec("INSERT OR REPLACE INTO devices "
         "(device_id,password_salt,password_hash,retention_started_at) "
         "VALUES ('" + id + "','salt','hash'," + std::to_string(Now()) + ");");
  }
  void Presence(const std::string& id, int64_t at, bool online = false) {
    Exec("INSERT OR REPLACE INTO device_presence(device_id,online,updated_at) "
         "VALUES ('" + id + "'," + (online ? "1" : "0") + "," +
         std::to_string(at) + ");");
  }
  void Device(const std::string& id, int64_t at, bool online = false) {
    Credential(id);
    Presence(id, at, online);
  }
  bool Exists(const std::string& id) {
    return Scalar("SELECT COUNT(*) FROM devices WHERE device_id='" + id + "';") != 0;
  }
  std::unique_ptr<DeviceDBManager> db;

 private:
  std::filesystem::path path_;
  sqlite3* connection_ = nullptr;
};
}  // namespace

int main() {
  int failures = 0;
  auto expect = [&](bool condition, const std::string& message) {
    if (!condition) {
      std::cerr << "FAIL: " << message << '\n';
      ++failures;
    }
  };
  {
    Fixture f;
    const auto old = Now() - 181 * kDay;
    const auto recent = Now() - 179 * kDay;
    f.Device("expired", old);
    f.Presence("C-expired", old);
    f.Device("web-expired", old);
    f.Device("online", old, true);
    f.Device("recent", recent);
    f.Device("clone-online", old);
    f.Presence("C-clone-online", old, true);
    f.Device("clone-recent", old);
    f.Presence("C-clone-recent", recent);
    f.Device("base-recent", recent);
    f.Presence("C-base-recent", old);
    f.Presence("C-online", old);
    f.Device("host", old);
    f.Device("guest", old);
    expect(f.db->StartRemoteControlSession("recoverable", "C-host", "C-guest"),
           "create recoverable session fixture");
    f.Exec("INSERT INTO user_devices(user_id,device_id,updated_at) VALUES "
           "('expired','peer',0),('C-expired','peer',0),"
           "('watcher','expired',0),('watcher','C-expired',0),"
           "('C-clone-recent','peer',0),('C-guest','peer',0),"
           "('orphan','peer',0);");
    const auto result = f.db->CleanupExpiredDevices(180);
    expect(result.devices == 3 && result.credentials == 2 &&
               result.device_ids.size() == 2 && result.associations == 5 && !result.more,
           "expired base, clone, web credentials and related associations are deleted");
    expect(!f.Exists("expired") && !f.Exists("web-expired"), "expired identities are gone");
    for (const auto* id : {"online", "recent", "clone-online", "clone-recent",
                           "base-recent", "host", "guest"})
      expect(f.Exists(id), std::string("protected identity remains: ") + id);
    expect(f.Scalar("SELECT COUNT(*) FROM user_devices;") == 2,
           "recent clone and recoverable session associations remain");
    expect(f.Scalar("SELECT COUNT(*) FROM device_presence WHERE device_id IN "
                    "('C-online','C-base-recent');") == 2,
           "old clones of active base identities remain");
    expect(f.db->SetDeviceOnline("expired", true), "device reconnects before cleanup");
    f.Credential("expired");
    expect(f.db->CleanupExpiredDevices(180).device_ids.empty() && f.Exists("expired"),
           "cleanup rechecks state and preserves reconnected devices");
  }
  {
    Fixture f;
    bool checked = false;
    // Require a stable wall-clock second so the exact boundary assertion does
    // not depend on whether the scheduler crossed a second during SQLite I/O.
    for (int attempt = 0; attempt < 5 && !checked; ++attempt) {
      const auto now = Now();
      f.Device("boundary", now - 180 * kDay);
      f.Device("over", now - 180 * kDay - 1);
      f.Device("under", now - 180 * kDay + 1);
      const auto result = f.db->CleanupExpiredDevices(180);
      if (Now() != now) continue;
      checked = true;
      expect(result.credentials == 1 && !f.Exists("over") &&
                 f.Exists("boundary") && f.Exists("under"),
             "only strictly more than 180 offline days expires");
    }
    expect(checked, "exact cutoff was verified within a stable clock second");
  }
  {
    Fixture f;
    for (const auto* id : {"batch-a", "batch-b", "batch-c"}) {
      f.Device(id, Now() - 181 * kDay);
      f.Presence(std::string("C-") + id, Now() - 181 * kDay);
    }
    const auto first = f.db->CleanupExpiredDevices(180, 2);
    expect(first.more && first.credentials == 2 && first.devices == 4,
           "batch limit counts base identities without splitting clones");
    const auto last = f.db->CleanupExpiredDevices(180, 2);
    expect(!last.more && last.credentials == 1 && last.devices == 2,
           "subsequent batch drains the backlog");
    expect(f.db->CleanupExpiredDevices(180).device_ids.empty(), "cleanup is idempotent");
  }
  {
    Fixture f;
    f.Device("rollback", Now() - 181 * kDay);
    f.Exec("INSERT INTO user_devices VALUES ('rollback','peer',0);"
           "CREATE TRIGGER fail_cleanup BEFORE DELETE ON devices "
           "BEGIN SELECT RAISE(ABORT,'injected cleanup failure'); END;");
    bool threw = false;
    try { f.db->CleanupExpiredDevices(180); }
    catch (const std::runtime_error&) { threw = true; }
    expect(threw && f.Exists("rollback") &&
               f.Scalar("SELECT COUNT(*) FROM device_presence;") == 1 &&
               f.Scalar("SELECT COUNT(*) FROM user_devices;") == 1,
           "deletion failure rolls back credentials, presence and associations");
    f.Exec("DROP TRIGGER fail_cleanup;");
    expect(f.db->CleanupExpiredDevices(180).credentials == 1, "failed cleanup can retry");
  }
  {
    Fixture f(true);
    expect(f.Scalar("SELECT retention_started_at FROM devices;") >= Now() - 2 &&
               f.db->CleanupExpiredDevices(180).credentials == 0,
           "legacy credentials with unknown offline age get a fresh retention period");
    const auto at = Now() - kDay;
    f.Exec("UPDATE devices SET retention_started_at=" + std::to_string(at) + ";");
    f.Restart();
    expect(f.Scalar("SELECT retention_started_at FROM devices;") == at,
           "restart does not reset the legacy retention period");
    f.Exec("UPDATE devices SET retention_started_at=" + std::to_string(Now() - 181 * kDay) + ";");
    expect(f.db->CleanupExpiredDevices(180).credentials == 1 && !f.Exists("legacy"),
           "legacy credential-only records eventually expire");
  }
  {
    Fixture f;
    f.Credential("unknown-age");
    f.Presence("C-unknown-age", Now() - 181 * kDay);
    expect(f.db->CleanupExpiredDevices(180).device_ids.empty(),
           "old clone cannot expire a credential whose own offline age is unknown");
    f.Exec("UPDATE devices SET retention_started_at=1;");
    const auto result = f.db->CleanupExpiredDevices(180);
    expect(result.credentials == 1 && result.devices == 1,
           "credential without base presence expires once both deadlines pass");
  }
  {
    Fixture f;
    f.Device("downtime", Now() - 182 * kDay, true);
    f.Exec("UPDATE server_runtime SET last_seen_at=" +
           std::to_string(Now() - 181 * kDay) + ";");
    f.Restart();
    expect(f.db->CleanupExpiredDevices(180).credentials == 1,
           "long server downtime counts toward offline expiry after restart");
  }
  {
    Fixture f;
    const auto credential = f.db->AddDevice("", "");
    expect(!credential.device_id.empty() &&
               f.db->CleanupExpiredDevices(180).credentials == 0,
           "newly registered credentials without presence do not expire");
    f.Presence(credential.device_id, Now() - 181 * kDay);
    expect(f.db->CleanupExpiredDevices(180).credentials == 1 &&
               f.db->VerifyDevice(credential.device_id, credential.password) == -2,
           "expired credentials no longer authenticate");
    const auto replacement = f.db->AddDevice(credential.device_id, credential.password);
    expect(!replacement.device_id.empty() && !replacement.password.empty(),
           "returning expired device follows new registration flow");
    f.Presence(replacement.device_id, Now() - 181 * kDay);
    f.Exec("UPDATE devices SET retention_started_at=1;");
    const auto preview = f.db->AdminDeviceData(replacement.device_id, "query", "history",
                                               "", "admin", "REQ-RETENTION");
    const auto cleanup = f.db->AdminDeviceData(replacement.device_id, "cleanup", "history",
        preview["data"]["revision"], "admin", "REQ-RETENTION");
    expect(cleanup.value("ok", false) &&
               f.db->CleanupExpiredDevices(180).credentials == 0 &&
               f.db->VerifyDevice(replacement.device_id, replacement.password) == 0,
           "history-only cleanup keeps credentials for a fresh retention period");
  }
  return failures == 0 ? 0 : 1;
}
