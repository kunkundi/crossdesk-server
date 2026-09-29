#include "session_recovery.h"

#include <openssl/rand.h>
#include <openssl/crypto.h>
#include <chrono>
#include <stdexcept>

namespace {
int64_t Now() {
  return std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
}
struct Statement {
  sqlite3_stmt* value = nullptr;
  Statement(sqlite3* db, const char* sql) {
    if (sqlite3_prepare_v2(db, sql, -1, &value, nullptr) != SQLITE_OK)
      throw std::runtime_error("Session recovery database operation failed");
  }
  ~Statement() { sqlite3_finalize(value); }
  void Bind(int index, const std::string& text) {
    sqlite3_bind_text(value, index, text.c_str(), -1, SQLITE_TRANSIENT);
  }
  std::string Text(int index) {
    const auto* p = sqlite3_column_text(value, index);
    return p ? reinterpret_cast<const char*>(p) : "";
  }
  void Done() {
    if (sqlite3_step(value) != SQLITE_DONE)
      throw std::runtime_error("Session recovery database write failed");
  }
  int Step() {
    const int result = sqlite3_step(value);
    if (result != SQLITE_ROW && result != SQLITE_DONE)
      throw std::runtime_error("Session recovery database read failed");
    return result;
  }
};
}

SessionRecovery::SessionRecovery(const std::string& path, DeviceDBManager* db,
    std::shared_ptr<TransmissionManager> transmission, Send send)
    : db_(db), transmission_(std::move(transmission)), send_(std::move(send)) {
  if (sqlite3_open(path.c_str(), &sql_) != SQLITE_OK) {
    if (sql_) sqlite3_close(sql_);
    sql_ = nullptr;
    throw std::runtime_error("Cannot open session recovery database");
  }
  try {
  sqlite3_busy_timeout(sql_, 1000);
  if (sqlite3_exec(sql_, "PRAGMA secure_delete=ON; CREATE TABLE IF NOT EXISTS session_recovery ("
      "transmission_id TEXT NOT NULL, guest_id TEXT NOT NULL, host_id TEXT NOT NULL,"
      "token TEXT NOT NULL, PRIMARY KEY(transmission_id,guest_id));"
      "CREATE TRIGGER IF NOT EXISTS session_recovery_cleanup AFTER DELETE ON remote_control_sessions "
      "BEGIN DELETE FROM session_recovery WHERE transmission_id=OLD.transmission_id AND guest_id=OLD.guest_id; END;"
      "DELETE FROM session_recovery WHERE NOT EXISTS (SELECT 1 FROM "
      "remote_control_sessions s WHERE s.transmission_id=session_recovery.transmission_id "
      "AND s.guest_id=session_recovery.guest_id);", nullptr, nullptr, nullptr) != SQLITE_OK)
    throw std::runtime_error("Cannot initialize session recovery database");
  Statement read(sql_, "SELECT transmission_id,host_id,guest_id,token FROM session_recovery;");
  while (read.Step() == SQLITE_ROW) {
    Ticket ticket;
    ticket.tx = read.Text(0); ticket.host = read.Text(1);
    ticket.guest = read.Text(2); ticket.token = read.Text(3);
    ticket.recovering = true; ticket.deadline = Now() + 120;
    tickets_[{ticket.tx, ticket.guest}] = std::move(ticket);
  }
  } catch (...) {
    sqlite3_close(sql_); sql_ = nullptr;
    throw;
  }
}

SessionRecovery::~SessionRecovery() { if (sql_) sqlite3_close(sql_); }

void SessionRecovery::Save(const Ticket& ticket) {
  Statement stmt(sql_, "INSERT OR REPLACE INTO session_recovery VALUES(?,?,?,?);");
  stmt.Bind(1, ticket.tx); stmt.Bind(2, ticket.guest);
  stmt.Bind(3, ticket.host); stmt.Bind(4, ticket.token); stmt.Done();
}

bool SessionRecovery::Exists(const Ticket& ticket) {
  Statement stmt(sql_, "SELECT 1 FROM remote_control_sessions WHERE transmission_id=? AND guest_id=? AND host_id=?;");
  stmt.Bind(1, ticket.tx); stmt.Bind(2, ticket.guest); stmt.Bind(3, ticket.host);
  return stmt.Step() == SQLITE_ROW;
}

void SessionRecovery::Remove(const Key& key) {
  Statement stmt(sql_, "DELETE FROM session_recovery WHERE transmission_id=? AND guest_id=?;");
  stmt.Bind(1, key.first); stmt.Bind(2, key.second); stmt.Done();
  tickets_.erase(key);
}

bool SessionRecovery::Manages(const std::string& tx, const std::string& guest) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  return tickets_.count({tx, guest}) != 0;
}

bool SessionRecovery::IsRecovering(const std::string& tx, const std::string& guest) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  const auto it = tickets_.find({tx, guest});
  return it != tickets_.end() && it->second.recovering;
}

void SessionRecovery::Login(websocketpp::connection_hdl hdl, const nlohmann::json& request) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  capable_[hdl] = request.contains("session_resume_version") &&
                 request["session_resume_version"] == 1;
}

void SessionRecovery::Issue(const std::string& tx, const std::string& host,
    const std::string& guest, websocketpp::connection_hdl guest_hdl) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  const auto host_hdl = transmission_->GetWsHandle(host);
  if (!capable_[host_hdl] || !capable_[guest_hdl]) return;
  unsigned char bytes[32];
  if (RAND_bytes(bytes, sizeof(bytes)) != 1)
    throw std::runtime_error("Cannot generate session recovery ticket");
  Ticket ticket;
  ticket.tx = tx; ticket.host = host; ticket.guest = guest;
  ticket.host_hdl = host_hdl; ticket.guest_hdl = guest_hdl;
  const char* hex = "0123456789abcdef";
  for (auto byte : bytes) {
    ticket.token += hex[byte >> 4]; ticket.token += hex[byte & 15];
  }
  Save(ticket);
  tickets_[{tx, guest}] = ticket;
  const nlohmann::json msg = {{"type", "session_ticket"}, {"transmission_id", tx},
      {"host_id", host}, {"guest_id", guest}, {"token", ticket.token}};
  send_(host_hdl, msg); send_(guest_hdl, msg);
}

void SessionRecovery::Report(websocketpp::connection_hdl hdl, const nlohmann::json& msg) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  for (const char* key : {"transmission_id", "guest_id", "token"})
    if (!msg.contains(key) || !msg[key].is_string()) return;
  if (!msg.contains("alive") || !msg["alive"].is_boolean()) return;
  const Key key{msg["transmission_id"], msg["guest_id"]};
  auto found = tickets_.find(key);
  if (found == tickets_.end()) return;
  auto& ticket = found->second;
  const auto user = transmission_->GetUserId(hdl);
  const auto token = msg["token"].get<std::string>();
  if (token.size() != ticket.token.size() ||
      CRYPTO_memcmp(token.data(), ticket.token.data(), token.size()) != 0 ||
      (user != ticket.host && user != ticket.guest) || !Exists(ticket) ||
      (ticket.recovering && Now() > ticket.deadline)) return;
  if (!msg["alive"].get<bool>()) {
    transmission_->ReleaseGuestFromTransmission(ticket.guest, ticket.tx);
    db_->EndRemoteControlSession(ticket.tx, ticket.host, ticket.guest);
    Remove(key);
    return;
  }
  if (user == ticket.host) {
    ticket.host_ready = true; ticket.host_hdl = hdl;
  } else {
    ticket.guest_ready = true; ticket.guest_hdl = hdl;
  }
  if (ticket.host_ready && ticket.guest_ready &&
      !ticket.host_hdl.expired() && !ticket.guest_hdl.expired() &&
      transmission_->GetUserId(ticket.host_hdl) == ticket.host &&
      transmission_->GetUserId(ticket.guest_hdl) == ticket.guest) {
    if (!transmission_->IsTransmissionExist(ticket.tx))
      transmission_->BindHostToTransmission(ticket.host, ticket.tx);
    if (transmission_->BindGuestToTransmission(ticket.guest, ticket.tx, ticket.guest_hdl)) {
      ticket.recovering = false;
      const nlohmann::json ack = {{"type", "session_resumed"},
          {"transmission_id", ticket.tx}, {"guest_id", ticket.guest}};
      send_(ticket.host_hdl, ack); send_(ticket.guest_hdl, ack);
    }
  }
}

void SessionRecovery::Expire() {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  for (auto it = capable_.begin(); it != capable_.end();)
    if (it->first.expired()) it = capable_.erase(it); else ++it;
  for (auto it = tickets_.begin(); it != tickets_.end();) {
    const auto ticket = (it++)->second;
    if (!Exists(ticket) || (ticket.recovering && Now() > ticket.deadline)) {
      if (ticket.recovering) {
        transmission_->ReleaseGuestFromTransmission(ticket.guest, ticket.tx);
        db_->EndRemoteControlSession(ticket.tx, ticket.host, ticket.guest);
      }
      Remove({ticket.tx, ticket.guest});
    }
  }
}

nlohmann::json SessionRecovery::Status() {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  int recoverable = 0, recovering = 0;
  for (const auto& pair : tickets_) {
    const auto& t = pair.second;
    if (!Exists(t)) continue;
    if (t.recovering) ++recovering;
    else if (t.host_ready && t.guest_ready && !t.host_hdl.expired() && !t.guest_hdl.expired())
      ++recoverable;
  }
  Statement count(sql_, "SELECT COUNT(*) FROM remote_control_sessions;");
  if (count.Step() != SQLITE_ROW) throw std::runtime_error("Cannot count saved sessions");
  const int total = sqlite3_column_int(count.value, 0);
  return {{"total", total}, {"recoverable", recoverable}, {"recovering", recovering}};
}
