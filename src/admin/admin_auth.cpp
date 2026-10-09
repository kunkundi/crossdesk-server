#include "admin_auth.h"

#include <openssl/rand.h>
#include <openssl/crypto.h>

#include <cstdlib>
#include <iomanip>
#include <sstream>

namespace {

constexpr char kSessionCookieName[] = "cd_admin_session";

bool SecretEqual(const std::string& a, const std::string& b) {
  return a.size() == b.size() && !a.empty() &&
         CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}

std::string GetEnvString(const char* name) {
  const char* value = std::getenv(name);
  return value ? std::string(value) : std::string();
}

std::string Trim(std::string value) {
  size_t begin = value.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) {
    return "";
  }
  size_t end = value.find_last_not_of(" \t\r\n");
  return value.substr(begin, end - begin + 1);
}

}  // namespace

AdminAuth::AdminAuth()
    : AdminAuth(GetEnvString("ADMIN_USERNAME"), GetEnvString("ADMIN_PASSWORD"),
                std::chrono::hours(8)) {}

AdminAuth::AdminAuth(std::string username, std::string password,
                     std::chrono::seconds session_ttl,
                     DeviceAuthLimiter::Now now)
    : username_(std::move(username)),
      password_(std::move(password)),
      session_ttl_(session_ttl),
      login_limiter_(std::move(now), 5, 20) {}

bool AdminAuth::IsEnabled() const {
  return !username_.empty() && !password_.empty();
}

std::optional<std::string> AdminAuth::Login(const std::string& username,
                                            const std::string& password) {
  return Login(username, password, "").token;
}

AdminAuth::LoginResult AdminAuth::Login(const std::string& username,
                                       const std::string& password,
                                       const std::string& source_address) {
  if (!IsEnabled()) return {};
  // There is one configured administrator. All submitted usernames share its
  // budget, so rotating names or IPs cannot reset the aggregate failure count.
  const auto result = login_limiter_.Verify(source_address, "admin", [&] {
    const bool username_matches = SecretEqual(username, username_);
    const bool password_matches = SecretEqual(password, password_);
    return username_matches && password_matches;
  });
  if (!result.authenticated) return {std::nullopt, result.retry_after};

  std::string token = GenerateToken();
  std::string csrf = GenerateToken();
  if (token.empty() || csrf.empty()) return {};
  std::lock_guard<std::mutex> lock(sessions_mutex_);
  RemoveExpiredSessions();
  sessions_[token] = {std::chrono::system_clock::now() + session_ttl_, csrf};
  return {token, 0};
}

bool AdminAuth::ValidateSession(const std::string& token) {
  if (token.empty()) {
    return false;
  }

  std::lock_guard<std::mutex> lock(sessions_mutex_);
  RemoveExpiredSessions();
  auto it = sessions_.find(token);
  if (it == sessions_.end()) {
    return false;
  }

  if (it->second.expires <= std::chrono::system_clock::now()) {
    sessions_.erase(it);
    return false;
  }

  return true;
}

std::optional<std::string> AdminAuth::CsrfToken(const std::string& token) {
  std::lock_guard<std::mutex> lock(sessions_mutex_);
  RemoveExpiredSessions();
  auto it = sessions_.find(token);
  if (it == sessions_.end()) return std::nullopt;
  return it->second.csrf;
}

bool AdminAuth::ValidateCsrf(const std::string& token, const std::string& csrf) {
  const auto expected = CsrfToken(token);
  return expected && SecretEqual(*expected, csrf);
}

bool AdminAuth::Reauthenticate(const std::string& token,
                               const std::string& password) {
  std::lock_guard<std::mutex> lock(sessions_mutex_);
  RemoveExpiredSessions();
  if (!sessions_.count(token)) return false;
  const auto now = std::chrono::steady_clock::now();
  if (now < reauth_retry_at_) return false;
  if (!SecretEqual(password, password_)) {
    if (++reauth_failures_ >= 5) {
      reauth_failures_ = 0;
      reauth_retry_at_ = now + std::chrono::minutes(1);
    }
    return false;
  }
  reauth_failures_ = 0;
  return true;
}

void AdminAuth::Logout(const std::string& token) {
  std::lock_guard<std::mutex> lock(sessions_mutex_);
  sessions_.erase(token);
}

std::string AdminAuth::BuildSessionCookie(const std::string& token) const {
  return std::string(kSessionCookieName) + "=" + token +
         "; HttpOnly; Secure; SameSite=Strict; Path=/";
}

std::string AdminAuth::BuildExpiredCookie() const {
  return std::string(kSessionCookieName) +
         "=; Max-Age=0; HttpOnly; Secure; SameSite=Strict; Path=/";
}

std::string AdminAuth::ExtractCookie(const std::string& cookie_header,
                                     const std::string& name) {
  size_t start = 0;
  while (start <= cookie_header.size()) {
    size_t end = cookie_header.find(';', start);
    std::string part = cookie_header.substr(
        start, end == std::string::npos ? std::string::npos : end - start);
    part = Trim(part);

    const std::string prefix = name + "=";
    if (part.rfind(prefix, 0) == 0) {
      return part.substr(prefix.size());
    }

    if (end == std::string::npos) {
      break;
    }
    start = end + 1;
  }
  return "";
}

std::string AdminAuth::GenerateToken() const {
  unsigned char bytes[32];
  if (RAND_bytes(bytes, sizeof(bytes)) != 1) {
    return "";
  }

  std::ostringstream ss;
  for (unsigned char byte : bytes) {
    ss << std::hex << std::setw(2) << std::setfill('0')
       << static_cast<int>(byte);
  }
  return ss.str();
}

void AdminAuth::RemoveExpiredSessions() {
  auto now = std::chrono::system_clock::now();
  for (auto it = sessions_.begin(); it != sessions_.end();) {
    if (it->second.expires <= now) {
      it = sessions_.erase(it);
    } else {
      ++it;
    }
  }
}
