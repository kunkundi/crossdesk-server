#ifndef _ADMIN_AUTH_H_
#define _ADMIN_AUTH_H_

#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

class AdminAuth {
 public:
  AdminAuth();
  AdminAuth(std::string username, std::string password,
            std::chrono::seconds session_ttl);

  bool IsEnabled() const;
  std::optional<std::string> Login(const std::string& username,
                                   const std::string& password);
  bool ValidateSession(const std::string& token);
  std::optional<std::string> CsrfToken(const std::string& token);
  bool ValidateCsrf(const std::string& token, const std::string& csrf);
  bool Reauthenticate(const std::string& token, const std::string& password);
  const std::string& Username() const { return username_; }
  void Logout(const std::string& token);

  std::string BuildSessionCookie(const std::string& token) const;
  std::string BuildExpiredCookie() const;

  static std::string ExtractCookie(const std::string& cookie_header,
                                   const std::string& name);

 private:
  std::string GenerateToken() const;
  void RemoveExpiredSessions();

  std::string username_;
  std::string password_;
  std::chrono::seconds session_ttl_;
  mutable std::mutex sessions_mutex_;
  struct Session {
    std::chrono::system_clock::time_point expires;
    std::string csrf;
  };
  std::unordered_map<std::string, Session> sessions_;
  unsigned reauth_failures_ = 0;
  std::chrono::steady_clock::time_point reauth_retry_at_{};
};

#endif  // _ADMIN_AUTH_H_
