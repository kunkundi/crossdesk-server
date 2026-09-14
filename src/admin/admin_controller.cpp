#include "admin_controller.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>

namespace {

constexpr char kSessionCookieName[] = "cd_admin_session";
constexpr char kDisconnectPrefix[] = "/api/admin/sessions/";
constexpr char kDisconnectSuffix[] = "/disconnect";
constexpr size_t kDefaultPageLimit = 50;
constexpr size_t kMaxPageLimit = 200;
constexpr char kAdminAssetPrefix[] = "/admin/assets/";

std::string ResourcePath(const std::string& resource) {
  size_t query_pos = resource.find('?');
  return query_pos == std::string::npos ? resource
                                        : resource.substr(0, query_pos);
}

std::string ResourceQuery(const std::string& resource) {
  size_t query_pos = resource.find('?');
  return query_pos == std::string::npos ? "" : resource.substr(query_pos + 1);
}

int HexValue(char ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
  if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
  return -1;
}

std::string UrlDecode(const std::string& value) {
  std::string decoded;
  decoded.reserve(value.size());
  for (size_t i = 0; i < value.size(); ++i) {
    char ch = value[i];
    if (ch == '+') {
      decoded.push_back(' ');
    } else if (ch == '%' && i + 2 < value.size()) {
      int hi = HexValue(value[i + 1]);
      int lo = HexValue(value[i + 2]);
      if (hi >= 0 && lo >= 0) {
        decoded.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
      } else {
        decoded.push_back(ch);
      }
    } else {
      decoded.push_back(ch);
    }
  }
  return decoded;
}

std::map<std::string, std::string> ParseQueryParams(
    const std::string& query_string) {
  std::map<std::string, std::string> params;
  size_t start = 0;
  while (start <= query_string.size()) {
    size_t end = query_string.find('&', start);
    std::string item =
        query_string.substr(start, end == std::string::npos
                                       ? std::string::npos
                                       : end - start);
    if (!item.empty()) {
      size_t equals = item.find('=');
      std::string key = UrlDecode(item.substr(0, equals));
      std::string value = equals == std::string::npos
                              ? ""
                              : UrlDecode(item.substr(equals + 1));
      params[key] = value;
    }
    if (end == std::string::npos) {
      break;
    }
    start = end + 1;
  }
  return params;
}

bool ParseSize(const std::string& value, size_t* result) {
  if (value.empty()) {
    return false;
  }
  size_t parsed = 0;
  for (unsigned char ch : value) {
    if (!std::isdigit(ch)) {
      return false;
    }
    size_t digit = static_cast<size_t>(ch - '0');
    if (parsed > (static_cast<size_t>(-1) - digit) / 10) {
      return false;
    }
    parsed = parsed * 10 + digit;
  }
  *result = parsed;
  return true;
}

size_t QuerySizeParam(const std::map<std::string, std::string>& params,
                      const std::string& key, size_t fallback,
                      size_t max_value) {
  auto it = params.find(key);
  if (it == params.end()) {
    return fallback;
  }
  size_t value = 0;
  if (!ParseSize(it->second, &value)) {
    return fallback;
  }
  return (std::min)(value, max_value);
}

std::string QueryStringParam(const std::map<std::string, std::string>& params,
                             const std::string& key) {
  auto it = params.find(key);
  return it == params.end() ? "" : it->second;
}

std::string QueryStringParam(const std::map<std::string, std::string>& params,
                             const std::string& key,
                             const std::string& fallback) {
  auto it = params.find(key);
  return it == params.end() || it->second.empty() ? fallback : it->second;
}

std::string ClientKind(const std::string& device_id) {
  return device_id.rfind("web-", 0) == 0 ? "web" : "device";
}

std::string LowerAscii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char ch) {
                   return static_cast<char>(std::tolower(ch));
                 });
  return value;
}

bool IsLocationSort(const std::string& sort) {
  return LowerAscii(sort) == "location";
}

std::string NormalizeDeviceKind(const std::string& kind) {
  std::string normalized = LowerAscii(kind);
  if (normalized == "web") {
    return "web";
  }
  if (normalized == "all") {
    return "all";
  }
  return "pc";
}

ClientNetworkInfo CurrentNetworkInfo(PresenceManager* presence,
                                     const OnlineDeviceInfo& device) {
  ClientNetworkInfo network_info;
  if (presence && device.online) {
    presence->GetDeviceNetworkInfo(device.device_id, &network_info);
  }
  return network_info;
}

bool HasDisplayedLocation(PresenceManager* presence,
                          const OnlineDeviceInfo& device) {
  return !CurrentNetworkInfo(presence, device).location.empty();
}

void SortDevicesByDisplayedLocation(
    std::vector<OnlineDeviceInfo>* devices, PresenceManager* presence,
    const std::string& order) {
  if (!devices) {
    return;
  }

  const bool ascending = LowerAscii(order) == "asc";
  std::unordered_map<std::string, bool> known_location_cache;
  auto has_location = [&](const OnlineDeviceInfo& device) {
    auto cached = known_location_cache.find(device.device_id);
    if (cached != known_location_cache.end()) {
      return cached->second;
    }
    bool known = HasDisplayedLocation(presence, device);
    known_location_cache.emplace(device.device_id, known);
    return known;
  };

  std::sort(devices->begin(), devices->end(),
            [&](const OnlineDeviceInfo& lhs, const OnlineDeviceInfo& rhs) {
              const bool lhs_known = has_location(lhs);
              const bool rhs_known = has_location(rhs);
              if (lhs_known != rhs_known) {
                return ascending ? !lhs_known : lhs_known;
              }
              if (lhs.online != rhs.online) {
                return lhs.online > rhs.online;
              }
              if (lhs.updated_at != rhs.updated_at) {
                return lhs.updated_at > rhs.updated_at;
              }
              return lhs.device_id < rhs.device_id;
            });
}

void ApplyDevicePage(std::vector<OnlineDeviceInfo>* devices, size_t offset,
                     size_t limit) {
  if (!devices || offset >= devices->size() || limit == 0) {
    if (devices) {
      devices->clear();
    }
    return;
  }

  const size_t available = devices->size() - offset;
  const size_t count = (std::min)(limit, available);
  std::vector<OnlineDeviceInfo> page(devices->begin() + offset,
                                     devices->begin() + offset + count);
  devices->swap(page);
}

int64_t CountForDeviceFilter(const DevicePresenceCounts& counts,
                             const std::string& filter) {
  std::string normalized = filter;
  std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                 [](unsigned char ch) {
                   return static_cast<char>(std::tolower(ch));
                 });
  if (normalized == "online") {
    return counts.online;
  }
  if (normalized == "offline") {
    return counts.offline;
  }
  if (normalized == "active") {
    return counts.active;
  }
  if (normalized == "web") {
    return counts.web;
  }
  return counts.all;
}

nlohmann::json GeoDistributionJson(const ClientGeoDistribution& distribution) {
  nlohmann::json geo_distribution = {{"total_count", distribution.total_count},
                                     {"domestic_count",
                                      distribution.domestic_count},
                                     {"foreign_count",
                                      distribution.foreign_count},
                                     {"unknown_count",
                                      distribution.unknown_count},
                                     {"provinces", nlohmann::json::array()},
                                     {"countries", nlohmann::json::array()}};
  for (const auto& province : distribution.provinces) {
    geo_distribution["provinces"].push_back(
        {{"province", province.province}, {"count", province.count}});
  }
  for (const auto& country : distribution.countries) {
    geo_distribution["countries"].push_back(
        {{"country", country.country}, {"count", country.count}});
  }
  return geo_distribution;
}

std::vector<std::filesystem::path> AdminWebRootCandidates() {
  std::vector<std::filesystem::path> roots;
  const char* env_root = std::getenv("CROSSDESK_ADMIN_WEB_DIR");
  if (env_root && *env_root) {
    roots.emplace_back(env_root);
  }
  roots.emplace_back("/crossdesk-server/admin");
  roots.emplace_back("/usr/share/crossdesk/admin");
  roots.emplace_back("src/admin/web");
  roots.emplace_back("../src/admin/web");
  std::error_code ec;
  std::filesystem::path cwd = std::filesystem::current_path(ec);
  while (!ec && !cwd.empty()) {
    roots.emplace_back(cwd / "src/admin/web");
    if (cwd == cwd.parent_path()) {
      break;
    }
    cwd = cwd.parent_path();
  }
  return roots;
}

const std::set<std::string>& AdminAllowedWebFiles() {
  static const std::set<std::string> kAllowedFiles = {
      "index.html", "admin.css", "admin.js", "china-provinces.json"};
  return kAllowedFiles;
}

std::filesystem::path ResolveAdminWebRoot() {
  for (const auto& root : AdminWebRootCandidates()) {
    std::filesystem::path index_path = root / "index.html";
    std::error_code ec;
    if (std::filesystem::is_regular_file(index_path, ec)) {
      return root;
    }
  }
  return {};
}

const std::filesystem::path& AdminWebRoot() {
  static const std::filesystem::path kRoot = ResolveAdminWebRoot();
  return kRoot;
}

void WarmAdminWebRoot() { (void)AdminWebRoot(); }

bool ReadAdminWebFile(const std::string& file_name, std::string* content) {
  if (!content ||
      AdminAllowedWebFiles().find(file_name) == AdminAllowedWebFiles().end()) {
    return false;
  }

  static std::mutex cache_mutex;
  static std::unordered_map<std::string, std::string> cache;
  std::lock_guard<std::mutex> lock(cache_mutex);
  auto cached = cache.find(file_name);
  if (cached != cache.end()) {
    *content = cached->second;
    return true;
  }

  const auto& root = AdminWebRoot();
  if (root.empty()) {
    return false;
  }

  std::filesystem::path path = root / file_name;
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return false;
  }
  std::string body((std::istreambuf_iterator<char>(file)),
                   std::istreambuf_iterator<char>());
  *content = body;
  cache.emplace(file_name, std::move(body));
  return true;
}

std::string AdminAssetNameFromPath(const std::string& path) {
  if (path.rfind(kAdminAssetPrefix, 0) != 0) {
    return "";
  }
  std::string file_name = path.substr(std::string(kAdminAssetPrefix).size());
  if (file_name.find('/') != std::string::npos ||
      file_name.find('\\') != std::string::npos) {
    return "";
  }
  return file_name;
}

std::string AdminAssetContentType(const std::string& file_name) {
  if (file_name == "admin.css") {
    return "text/css; charset=utf-8";
  }
  if (file_name == "admin.js") {
    return "application/javascript; charset=utf-8";
  }
  if (file_name == "china-provinces.json") {
    return "application/json; charset=utf-8";
  }
  return "application/octet-stream";
}

std::string AdminDisabledHtml() {
  return "<!doctype html><html><head><meta charset=\"utf-8\"><title>CrossDesk "
         "Admin</title></head><body><h1>CrossDesk Admin</h1><p>Admin "
         "dashboard is not enabled. Set ADMIN_USERNAME and ADMIN_PASSWORD to "
         "enable it.</p></body></html>";
}

std::string JsonContentType() { return "application/json; charset=utf-8"; }

}  // namespace

AdminController::AdminController(
    AdminAuth* auth, PresenceManager* presence,
    std::shared_ptr<TransmissionManager> transmission, DeviceDBManager* db,
    std::function<void(const std::string&, nlohmann::json)> send_to_user,
    std::chrono::milliseconds stats_ttl)
    : stats_ttl_(stats_ttl),
      auth_(auth),
      presence_(presence),
      transmission_(std::move(transmission)),
      db_(db),
      send_to_user_(std::move(send_to_user)) {
  WarmAdminWebRoot();
}

bool AdminController::IsAdminRoute(const std::string& resource) {
  std::string path = ResourcePath(resource);
  return path == "/admin" || path.rfind(kAdminAssetPrefix, 0) == 0 ||
         path == "/api/admin" || path.rfind("/api/admin/", 0) == 0;
}

std::string AdminController::ExtractDisconnectTransmissionId(
    const std::string& resource) {
  std::string path = ResourcePath(resource);
  if (path.rfind(kDisconnectPrefix, 0) != 0) {
    return "";
  }

  std::string tail = path.substr(std::string(kDisconnectPrefix).size());
  if (tail.size() <= std::string(kDisconnectSuffix).size()) {
    return "";
  }

  size_t suffix_pos = tail.rfind(kDisconnectSuffix);
  if (suffix_pos == std::string::npos ||
      suffix_pos + std::string(kDisconnectSuffix).size() != tail.size()) {
    return "";
  }

  return tail.substr(0, suffix_pos);
}

AdminHttpResponse AdminController::Handle(const AdminHttpRequest& request) {
  std::string path = ResourcePath(request.resource);
  if (path == "/admin") {
    return HandleAdminPage();
  }
  if (path.rfind(kAdminAssetPrefix, 0) == 0) {
    return HandleAdminAsset(request);
  }

  if (!auth_ || !auth_->IsEnabled()) {
    return ErrorResponse(503, "admin_disabled");
  }

  if (path == "/api/admin/login") {
    return HandleLogin(request);
  }

  if (!IsAuthorized(request)) {
    return ErrorResponse(401, "unauthorized");
  }

  if (path == "/api/admin/logout") {
    return HandleLogout(request);
  }
  if (path == "/api/admin/stats") {
    return HandleStats(request);
  }
  if (path == "/api/admin/overview") {
    return HandleOverview(request);
  }
  if (!ExtractDisconnectTransmissionId(path).empty()) {
    return HandleDisconnect(request);
  }

  return ErrorResponse(404, "not_found");
}

AdminHttpResponse AdminController::HandleAdminPage() {
  if (!auth_ || !auth_->IsEnabled()) {
    return HtmlResponse(200, AdminDisabledHtml());
  }
  std::string body;
  if (!ReadAdminWebFile("index.html", &body)) {
    return HtmlResponse(
        500,
        "<!doctype html><html><head><meta charset=\"utf-8\"><title>CrossDesk "
        "Admin</title></head><body><h1>CrossDesk Admin</h1><p>Admin frontend "
        "assets were not found.</p></body></html>");
  }
  return HtmlResponse(200, body);
}

AdminHttpResponse AdminController::HandleLogin(
    const AdminHttpRequest& request) {
  if (request.method != "POST") {
    return ErrorResponse(405, "method_not_allowed");
  }

  nlohmann::json body;
  try {
    body = nlohmann::json::parse(request.body);
  } catch (...) {
    return ErrorResponse(400, "invalid_json");
  }

  if (!body.contains("username") || !body["username"].is_string() ||
      !body.contains("password") || !body["password"].is_string()) {
    return ErrorResponse(400, "invalid_json");
  }

  auto token = auth_->Login(body["username"].get<std::string>(),
                            body["password"].get<std::string>());
  if (!token.has_value()) {
    return ErrorResponse(401, "unauthorized");
  }

  AdminHttpResponse response = JsonResponse(200, {{"ok", true}});
  response.headers.push_back({"Set-Cookie", auth_->BuildSessionCookie(*token)});
  return response;
}

AdminHttpResponse AdminController::HandleLogout(
    const AdminHttpRequest& request) {
  if (request.method != "POST") {
    return ErrorResponse(405, "method_not_allowed");
  }

  std::string token = AdminAuth::ExtractCookie(request.cookie, kSessionCookieName);
  if (!token.empty()) {
    auth_->Logout(token);
  }

  AdminHttpResponse response = JsonResponse(200, {{"ok", true}});
  response.headers.push_back({"Set-Cookie", auth_->BuildExpiredCookie()});
  return response;
}

AdminHttpResponse AdminController::HandleStats(
    const AdminHttpRequest& request) {
  if (request.method != "GET") {
    return ErrorResponse(405, "method_not_allowed");
  }

  size_t online_device_fallback =
      !presence_ && db_ ? static_cast<size_t>(db_->CountOnlineDevices()) : 0;
  return JsonResponse(200, {{"stats", BuildStats(online_device_fallback)}});
}

AdminHttpResponse AdminController::HandleOverview(
    const AdminHttpRequest& request) {
  if (request.method != "GET") {
    return ErrorResponse(405, "method_not_allowed");
  }

  auto params = ParseQueryParams(ResourceQuery(request.resource));
  size_t device_limit =
      QuerySizeParam(params, "device_limit", kDefaultPageLimit, kMaxPageLimit);
  size_t device_offset =
      QuerySizeParam(params, "device_offset", 0, static_cast<size_t>(-1));
  std::string device_search = QueryStringParam(params, "device_search");
  std::string device_filter =
      QueryStringParam(params, "device_filter", "online");
  std::string device_kind = NormalizeDeviceKind(
      QueryStringParam(params, "device_kind", "pc"));
  std::string device_sort =
      QueryStringParam(params, "device_sort", "status");
  std::string device_order =
      QueryStringParam(params, "device_order", "desc");
  size_t session_limit =
      QuerySizeParam(params, "session_limit", kDefaultPageLimit, kMaxPageLimit);
  size_t session_offset =
      QuerySizeParam(params, "session_offset", 0, static_cast<size_t>(-1));
  std::string session_search = QueryStringParam(params, "session_search");

  nlohmann::json devices = nlohmann::json::array();
  size_t devices_total = 0;
  nlohmann::json device_counts = {{"all", 0},
                                  {"online", 0},
                                  {"offline", 0},
                                  {"active", 0},
                                  {"web", 0}};
  nlohmann::json device_kind_counts = {{"pc", 0}, {"web", 0}};
  size_t online_device_fallback = 0;
  if (db_) {
    DevicePresenceCounts counts =
        db_->CountDevicePresenceByFilters(device_search, device_kind);
    DevicePresenceCounts pc_counts =
        device_kind == "pc"
            ? counts
            : db_->CountDevicePresenceByFilters(device_search, "pc");
    DevicePresenceCounts web_counts =
        device_kind == "web"
            ? counts
            : db_->CountDevicePresenceByFilters(device_search, "web");
    device_counts["all"] = counts.all;
    device_counts["online"] = counts.online;
    device_counts["offline"] = counts.offline;
    device_counts["active"] = counts.active;
    device_counts["web"] = web_counts.all;
    device_kind_counts["pc"] = pc_counts.all;
    device_kind_counts["web"] = web_counts.all;
    devices_total =
        static_cast<size_t>(CountForDeviceFilter(counts, device_filter));
    if (!presence_) {
      online_device_fallback = static_cast<size_t>(db_->CountOnlineDevices());
    }
    const bool location_sort = IsLocationSort(device_sort);
    size_t query_limit = location_sort ? devices_total : device_limit;
    size_t query_offset = location_sort ? 0 : device_offset;
    std::string query_sort = location_sort ? "status" : device_sort;
    std::vector<OnlineDeviceInfo> device_rows = db_->ListDevicePresence(
        query_limit, query_offset, device_search, device_filter, query_sort,
        device_order, device_kind);
    if (location_sort) {
      SortDevicesByDisplayedLocation(&device_rows, presence_, device_order);
      ApplyDevicePage(&device_rows, device_offset, device_limit);
    }
    for (const auto& device : device_rows) {
      int64_t active_control_count = device.active_control_count;
      int64_t active_controlled_count = device.active_controlled_count;
      ClientNetworkInfo network_info = CurrentNetworkInfo(presence_, device);
      devices.push_back({{"id", device.device_id},
                         {"online", device.online},
                         {"kind", ClientKind(device.device_id)},
                         {"client_version", device.client_version},
                         {"client_platform", device.client_platform},
                         {"updated_at", device.updated_at},
                         {"last_online_at",
                          device.online ? 0 : device.updated_at},
                         {"online_since", device.online_since},
                         {"online_duration_seconds",
                          device.online_duration_seconds},
                         {"total_online_seconds",
                          device.total_online_seconds},
                         {"total_control_seconds",
                          device.total_control_seconds},
                         {"total_controlled_seconds",
                          device.total_controlled_seconds},
                         {"client_ip", network_info.client_ip},
                         {"geo_country", network_info.country},
                         {"geo_region", network_info.region},
                         {"geo_city", network_info.city},
                         {"geo_location", network_info.location},
                         {"current_control_seconds",
                          device.current_control_seconds},
                         {"current_controlled_seconds",
                          device.current_controlled_seconds},
                         {"active_control_count", active_control_count},
                         {"active_controlled_count",
                          active_controlled_count},
                         {"active_control_targets",
                          device.active_control_targets},
                         {"active_controlled_by",
                          device.active_controlled_by},
                         {"active_session_count",
                          active_control_count +
                              active_controlled_count}});
    }
  }

  nlohmann::json sessions = nlohmann::json::array();
  size_t sessions_total = 0;
  if (db_) {
    sessions_total =
        static_cast<size_t>(db_->CountRemoteControlTransmissions(
            session_search));
    for (const auto& session : db_->ListRemoteControlSessions(
             session_limit, session_offset, session_search)) {
      sessions.push_back({{"transmission_id", session.transmission_id},
                          {"host_id", session.host_id},
                          {"guest_ids", session.guest_ids},
                          {"participant_count", 1 + session.guest_ids.size()},
                          {"active", true}});
    }
  }
  if (sessions_total == 0 && transmission_) {
    for (const auto& snapshot : transmission_->GetTransmissionSnapshots(
             session_limit, session_offset, session_search, &sessions_total)) {
      sessions.push_back({{"transmission_id", snapshot.transmission_id},
                          {"host_id", snapshot.host_id},
                          {"guest_ids", snapshot.guest_ids},
                          {"participant_count", snapshot.participant_count},
                          {"active", snapshot.active}});
    }
  }

  nlohmann::json geo_distribution =
      GeoDistributionJson(GetCurrentGeoDistribution());

  nlohmann::json stats = BuildStats(online_device_fallback);

  nlohmann::json devices_page = {{"limit", device_limit},
                                 {"offset", device_offset},
                                 {"total", devices_total},
                                 {"search", device_search},
                                 {"filter", device_filter},
                                 {"kind", device_kind},
                                 {"sort", device_sort},
                                 {"order", device_order}};
  nlohmann::json sessions_page = {{"limit", session_limit},
                                  {"offset", session_offset},
                                  {"total", sessions_total},
                                  {"search", session_search}};

  return JsonResponse(200, {{"stats", stats},
                            {"devices", devices},
                            {"devices_page", devices_page},
                            {"device_counts", device_counts},
                            {"device_kind_counts", device_kind_counts},
                            {"geo_distribution", geo_distribution},
                            {"sessions", sessions},
                            {"sessions_page", sessions_page}});
}

AdminHttpResponse AdminController::HandleAdminAsset(
    const AdminHttpRequest& request) {
  if (request.method != "GET") {
    return ErrorResponse(405, "method_not_allowed");
  }

  std::string file_name = AdminAssetNameFromPath(ResourcePath(request.resource));
  std::string body;
  if (file_name.empty() || !ReadAdminWebFile(file_name, &body)) {
    return ErrorResponse(404, "not_found");
  }

  AdminHttpResponse response;
  response.status = 200;
  response.content_type = AdminAssetContentType(file_name);
  response.headers.push_back({"Cache-Control", "public, max-age=3600"});
  response.body = std::move(body);
  return response;
}

AdminHttpResponse AdminController::HandleDisconnect(
    const AdminHttpRequest& request) {
  if (request.method != "POST") {
    return ErrorResponse(405, "method_not_allowed");
  }

  std::string transmission_id =
      ExtractDisconnectTransmissionId(request.resource);
  if (transmission_id.empty()) {
    return ErrorResponse(404, "not_found");
  }

  bool existed = false;
  if (transmission_) {
    auto snapshots = transmission_->GetTransmissionSnapshots();
    for (const auto& snapshot : snapshots) {
      if (snapshot.transmission_id != transmission_id) {
        continue;
      }
      existed = true;
      nlohmann::json message = {{"type", "admin_disconnect_transmission"},
                                {"transmission_id", transmission_id}};
      if (send_to_user_) {
        send_to_user_(snapshot.host_id, message);
        for (const auto& guest_id : snapshot.guest_ids) {
          send_to_user_(guest_id, message);
        }
      }
      break;
    }
    transmission_->DisconnectTransmission(transmission_id);
  }

  bool persisted = false;
  if (db_) {
    for (const auto& session : db_->ListRemoteControlSessions(
             static_cast<size_t>((std::numeric_limits<int>::max)()), 0,
             transmission_id)) {
      if (session.transmission_id == transmission_id) {
        persisted = true;
        break;
      }
    }
    if (persisted) {
      db_->EndRemoteControlTransmission(transmission_id);
    }
  }

  if (!existed && !persisted) {
    return JsonResponse(200, {{"ok", true}, {"already_closed", true}});
  }
  return JsonResponse(200, {{"ok", true}});
}

bool AdminController::IsAuthorized(const AdminHttpRequest& request) {
  if (!auth_) {
    return false;
  }
  std::string token = AdminAuth::ExtractCookie(request.cookie, kSessionCookieName);
  return auth_->ValidateSession(token);
}

nlohmann::json AdminController::BuildStats(size_t online_device_fallback) const {
  const auto now = std::chrono::steady_clock::now();
  if (!stats_cache_.is_null() && now - stats_cached_at_ < stats_ttl_)
    return stats_cache_;
  OnlineDurationStats duration_stats;
  if (db_) {
    duration_stats = db_->GetOnlineDurationStats();
  }
  size_t active_connection_count =
      transmission_ ? transmission_->GetActiveConnectionCount() : 0;
  if (db_) {
    active_connection_count = (std::max)(
        active_connection_count,
        static_cast<size_t>(db_->CountActiveRemoteControlConnections()));
  }

  stats_cached_at_ = now;
  stats_cache_ = {{"online_device_count",
           presence_ ? presence_->GetOnlineDeviceCount()
                     : online_device_fallback},
          {"online_web_client_count",
           presence_ ? presence_->GetOnlineWebClientCount() : 0},
          {"active_connection_count", active_connection_count},
          {"online_duration_seconds",
           duration_stats.current_online_seconds},
          {"total_online_seconds", duration_stats.total_online_seconds},
          {"total_control_seconds", duration_stats.total_control_seconds},
          {"total_controlled_seconds",
           duration_stats.total_controlled_seconds}};
  return stats_cache_;
}

ClientGeoDistribution AdminController::GetCurrentGeoDistribution() const {
  if (presence_) {
    return presence_->GetClientGeoDistribution();
  }
  return db_ ? db_->GetClientGeoDistribution() : ClientGeoDistribution{};
}

AdminHttpResponse AdminController::JsonResponse(
    int status, const nlohmann::json& body) const {
  AdminHttpResponse response;
  response.status = status;
  response.content_type = JsonContentType();
  response.headers.push_back({"Cache-Control", "no-store"});
  response.body = body.dump();
  return response;
}

AdminHttpResponse AdminController::HtmlResponse(
    int status, const std::string& body) const {
  AdminHttpResponse response;
  response.status = status;
  response.content_type = "text/html; charset=utf-8";
  response.headers.push_back({"Cache-Control", "no-store"});
  response.body = body;
  return response;
}

AdminHttpResponse AdminController::ErrorResponse(
    int status, const std::string& error) const {
  return JsonResponse(status, {{"ok", false}, {"error", error}});
}
