#ifndef _ADMIN_CONTROLLER_H_
#define _ADMIN_CONTROLLER_H_

#include <chrono>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>
#include <vector>

#include "admin_auth.h"
#include "device_db_manager.h"
#include "presence_manager.h"
#include "transmission_manager.h"

struct AdminHttpRequest {
  std::string method;
  std::string resource;
  std::string body;
  std::string cookie;
};

struct AdminHttpResponse {
  int status = 200;
  std::string content_type;
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;
};

class AdminController {
 public:
  AdminController(
      AdminAuth* auth, PresenceManager* presence,
      std::shared_ptr<TransmissionManager> transmission, DeviceDBManager* db,
      std::function<void(const std::string&, nlohmann::json)> send_to_user,
      std::chrono::milliseconds stats_ttl = std::chrono::milliseconds(0));

  static bool IsAdminRoute(const std::string& resource);
  static std::string ExtractDisconnectTransmissionId(
      const std::string& resource);

  AdminHttpResponse Handle(const AdminHttpRequest& request);
  void InvalidateStatsCache() { stats_cache_ = nullptr; }
  nlohmann::json GetPublicStats() const { return BuildStats(0); }

 private:
  AdminHttpResponse HandleAdminPage();
  AdminHttpResponse HandleLogin(const AdminHttpRequest& request);
  AdminHttpResponse HandleLogout(const AdminHttpRequest& request);
  AdminHttpResponse HandleStats(const AdminHttpRequest& request);
  AdminHttpResponse HandleOverview(const AdminHttpRequest& request);
  AdminHttpResponse HandleAdminAsset(const AdminHttpRequest& request);
  AdminHttpResponse HandleDisconnect(const AdminHttpRequest& request);

  bool IsAuthorized(const AdminHttpRequest& request);
  nlohmann::json BuildStats(size_t online_device_fallback) const;
  ClientGeoDistribution GetCurrentGeoDistribution() const;
  AdminHttpResponse JsonResponse(int status, const nlohmann::json& body) const;
  AdminHttpResponse HtmlResponse(int status, const std::string& body) const;
  AdminHttpResponse ErrorResponse(int status, const std::string& error) const;

  std::chrono::milliseconds stats_ttl_;
  mutable std::chrono::steady_clock::time_point stats_cached_at_{};
  mutable nlohmann::json stats_cache_;

  AdminAuth* auth_ = nullptr;
  PresenceManager* presence_ = nullptr;
  std::shared_ptr<TransmissionManager> transmission_;
  DeviceDBManager* db_ = nullptr;
  std::function<void(const std::string&, nlohmann::json)> send_to_user_;
};

#endif  // _ADMIN_CONTROLLER_H_
