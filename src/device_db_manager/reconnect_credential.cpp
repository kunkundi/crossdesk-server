#include "device_db_manager.h"

#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include <algorithm>
#include <array>
#include <memory>

namespace {
constexpr int64_t kLifetimeSeconds = 30 * 24 * 60 * 60;
using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;

bool PersistentDevice(const std::string& id) {
  return !id.empty() && id != "web" && id.size() <= 128 &&
         id.find('\0') == std::string::npos && id.find('@') == std::string::npos &&
         id.rfind("web-", 0) != 0 && id.rfind("C-", 0) != 0;
}

int64_t Now() {
  return std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string Hex(const unsigned char* bytes, size_t count) {
  constexpr char digits[] = "0123456789abcdef";
  std::string result;
  result.reserve(count * 2);
  for (size_t i = 0; i < count; ++i) {
    result += digits[bytes[i] >> 4];
    result += digits[bytes[i] & 15];
  }
  return result;
}

std::string Digest(const std::string& token) {
  std::array<unsigned char, SHA256_DIGEST_LENGTH> hash{};
  SHA256(reinterpret_cast<const unsigned char*>(token.data()), token.size(),
         hash.data());
  return Hex(hash.data(), hash.size());
}
}  // namespace

std::optional<ReconnectCredential> DeviceDBManager::IssueReconnectCredential(
    const std::string& device_id) {
  if (!PersistentDevice(device_id)) return std::nullopt;
  std::array<unsigned char, 32> random{};
  if (RAND_priv_bytes(random.data(), random.size()) != 1) return std::nullopt;
  ReconnectCredential result{Hex(random.data(), random.size()), Now() + kLifetimeSeconds};
  OPENSSL_cleanse(random.data(), random.size());
  const auto digest = Digest(result.token);
  std::lock_guard<std::recursive_mutex> lock(db_mutex_);
  sqlite3_stmt* raw = nullptr;
  if (!db_ || sqlite3_prepare_v2(db_,
      "INSERT INTO device_reconnect_credentials(device_id,token_hash,expires_at) "
      "SELECT device_id,?2,?3 FROM devices WHERE device_id=?1 "
      "ON CONFLICT(device_id) DO UPDATE SET token_hash=excluded.token_hash,"
      "expires_at=excluded.expires_at;", -1, &raw, nullptr) != SQLITE_OK)
    return std::nullopt;
  Statement stmt(raw, sqlite3_finalize);
  sqlite3_bind_text(raw, 1, device_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(raw, 2, digest.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(raw, 3, result.expires_at);
  if (sqlite3_step(raw) != SQLITE_DONE || sqlite3_changes(db_) != 1)
    return std::nullopt;
  return result;
}

std::optional<int64_t> DeviceDBManager::VerifyReconnectCredential(
    const std::string& device_id, const std::string& token) {
  if (!PersistentDevice(device_id) || token.size() != 64 ||
      !std::all_of(token.begin(), token.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
      })) return std::nullopt;
  const auto digest = Digest(token);
  std::lock_guard<std::recursive_mutex> lock(db_mutex_);
  sqlite3_stmt* raw = nullptr;
  if (!db_ || sqlite3_prepare_v2(db_,
      "SELECT r.token_hash,r.expires_at FROM device_reconnect_credentials r "
      "JOIN devices d ON d.device_id=r.device_id WHERE r.device_id=?;",
      -1, &raw, nullptr) != SQLITE_OK) return std::nullopt;
  Statement stmt(raw, sqlite3_finalize);
  sqlite3_bind_text(raw, 1, device_id.c_str(), -1, SQLITE_TRANSIENT);
  // Always compare a full digest, even for missing/expired device records.
  std::string expected(64, '0');
  int64_t expires = 0;
  if (sqlite3_step(raw) == SQLITE_ROW && sqlite3_column_bytes(raw, 0) == 64) {
    expected.assign(reinterpret_cast<const char*>(sqlite3_column_text(raw, 0)), 64);
    expires = sqlite3_column_int64(raw, 1);
  }
  const bool matches = CRYPTO_memcmp(expected.data(), digest.data(), 64) == 0;
  if (!matches || expires <= Now()) return std::nullopt;
  // No rotation/write on reconnect: parallel retries and lost responses must
  // not strand the client, and restart bursts should avoid extra WAL writes.
  return expires;
}

bool DeviceDBManager::RevokeReconnectCredential(const std::string& device_id) {
  if (!PersistentDevice(device_id)) return false;
  std::lock_guard<std::recursive_mutex> lock(db_mutex_);
  sqlite3_stmt* raw = nullptr;
  if (!db_ || sqlite3_prepare_v2(db_,
      "DELETE FROM device_reconnect_credentials WHERE device_id=?;",
      -1, &raw, nullptr) != SQLITE_OK) return false;
  Statement stmt(raw, sqlite3_finalize);
  sqlite3_bind_text(raw, 1, device_id.c_str(), -1, SQLITE_TRANSIENT);
  return sqlite3_step(raw) == SQLITE_DONE;
}
