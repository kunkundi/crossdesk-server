#include "admin_controller.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "admin_auth.h"
#include "device_db_manager.h"
#include "presence_manager.h"
#include "transmission_manager.h"

int main() {
  int failures = 0;

  auto expect = [&failures](bool condition, const std::string& message) {
    if (!condition) {
      std::cerr << "FAIL: " << message << std::endl;
      ++failures;
    }
  };
  auto contains_id = [](const std::vector<std::string>& ids,
                        const std::string& id) {
    return std::find(ids.begin(), ids.end(), id) != ids.end();
  };

  expect(AdminController::IsAdminRoute("/admin"), "/admin is admin route");
  expect(AdminController::IsAdminRoute("/admin/assets/admin.js"),
         "/admin/assets/admin.js is admin route");
  expect(AdminController::IsAdminRoute("/api/admin/overview"),
         "/api/admin/overview is admin route");
  expect(AdminController::IsAdminRoute("/api/admin/overview?session_limit=1"),
         "/api/admin/overview with query is admin route");
  expect(!AdminController::IsAdminRoute("/api/stats"),
         "/api/stats is not admin route");
  expect(!AdminController::IsAdminRoute("/api/adminx"),
         "/api/adminx is not admin route");

  expect(AdminController::ExtractDisconnectTransmissionId(
             "/api/admin/sessions/100284391/disconnect") == "100284391",
         "disconnect route extracts transmission id");
  expect(AdminController::ExtractDisconnectTransmissionId(
             "/api/admin/sessions/100284391/disconnect?x=1") == "100284391",
         "disconnect route extracts transmission id with query");
  expect(AdminController::ExtractDisconnectTransmissionId(
             "/api/admin/sessions/100284391") == "",
         "non-disconnect route does not extract id");

  auto transmission = std::make_shared<TransmissionManager>();
  AdminAuth disabled("", "", std::chrono::seconds(60));
  AdminController disabled_controller(
      &disabled, nullptr, transmission, nullptr,
      [](const std::string&, nlohmann::json) {});

  AdminHttpResponse disabled_response = disabled_controller.Handle(
      {"GET", "/api/admin/overview", "", ""});
  expect(disabled_response.status == 503,
         "disabled admin API returns service unavailable");
  expect(disabled_response.body.find("admin_disabled") != std::string::npos,
         "disabled admin API returns admin_disabled error");

  AdminAuth auth("admin", "secret", std::chrono::seconds(60));
  AdminController controller(&auth, nullptr, transmission, nullptr,
                             [](const std::string&, nlohmann::json) {});
  AdminHttpResponse admin_page =
      controller.Handle({"GET", "/admin", "", ""});
  expect(admin_page.status == 200, "admin page returns static frontend");
  expect(admin_page.body.find("/admin/assets/admin.js") != std::string::npos,
         "admin page references separated frontend script");
  expect(admin_page.body.find("CrossDesk uses IP2Location.io") !=
             std::string::npos,
         "admin page includes IP2Location attribution");
  expect(admin_page.body.find("https://www.ip2location.io") !=
             std::string::npos,
         "admin page links IP2Location attribution");
  AdminHttpResponse admin_script =
      controller.Handle({"GET", "/admin/assets/admin.js", "", ""});
  expect(admin_script.status == 200, "admin script asset returns ok");
  expect(admin_script.content_type.find("javascript") != std::string::npos,
         "admin script asset uses javascript content type");
  AdminHttpResponse china_map =
      controller.Handle({"GET", "/admin/assets/china-provinces.json", "", ""});
  expect(china_map.status == 200, "china map asset returns ok");
  expect(china_map.content_type.find("json") != std::string::npos,
         "china map asset uses json content type");
  expect(china_map.body.find("FeatureCollection") != std::string::npos,
         "china map asset returns geojson data");
  AdminHttpResponse unauthorized =
      controller.Handle({"GET", "/api/admin/overview", "", ""});
  expect(unauthorized.status == 401,
         "protected admin API requires session");
  expect(unauthorized.body.find("unauthorized") != std::string::npos,
         "protected admin API returns unauthorized error");

  AdminHttpResponse login = controller.Handle(
      {"POST", "/api/admin/login",
       R"({"username":"admin","password":"secret"})", ""});
  expect(login.status == 200, "valid login returns ok");
  bool has_cookie = false;
  for (const auto& header : login.headers) {
    if (header.first == "Set-Cookie" &&
        header.second.find("cd_admin_session=") != std::string::npos) {
      has_cookie = true;
    }
  }
  expect(has_cookie, "valid login sets session cookie");

  AdminHttpResponse bad_login = controller.Handle(
      {"POST", "/api/admin/login",
       R"({"username":"admin","password":"wrong"})", ""});
  expect(bad_login.status == 401, "invalid login returns unauthorized");

  transmission->BindHostToTransmission("host-1", "host-1");
  transmission->BindGuestToTransmission("guest-1", "host-1");
  auto token = auth.Login("admin", "secret");
  AdminHttpResponse overview = controller.Handle(
      {"GET",
       "/api/admin/overview?session_limit=1&session_offset=0&session_search=guest-1",
       "", "cd_admin_session=" + *token});
  expect(overview.status == 200, "overview with pagination returns ok");
  auto overview_body = nlohmann::json::parse(overview.body);
  expect(overview_body["sessions"].size() == 1,
         "overview applies session pagination");
  expect(overview_body["sessions_page"]["total"] == 1,
         "overview reports filtered session total");
  AdminHttpResponse stats = controller.Handle(
      {"GET", "/api/admin/stats", "", "cd_admin_session=" + *token});
  expect(stats.status == 200, "stats returns ok");
  auto stats_body = nlohmann::json::parse(stats.body);
  expect(stats_body["stats"]["active_connection_count"] == 1,
         "stats reports active connection count");
  expect(stats_body["stats"]["online_duration_seconds"] == 0,
         "stats reports online duration without database");

  const auto db_path =
      std::filesystem::temp_directory_path() /
      ("crossdesk_admin_controller_test_" +
       std::to_string(std::chrono::steady_clock::now()
                          .time_since_epoch()
                          .count()) +
       ".db");
  {
    DeviceDBManager db(db_path.string());
    PresenceManager presence;
    presence.SetDeviceDB(&db);
    websocketpp::connection_hdl hdl;
    presence.OnLogin("device-admin-1", "device-admin-1", hdl);
    db.UpdateDeviceNetworkInfo(
        "device-admin-1",
        {"10.0.0.1", "Stale Country", "Stale Region", "",
         "Stale Region, Stale Country"});
    presence.SetDeviceNetworkInfo(
        "device-admin-1",
        {"203.0.113.8", "Testland", "Test Region", "",
         "Test Region, Testland"});
    presence.OnLogin("device-admin-zhejiang", "device-admin-zhejiang", hdl);
    presence.SetDeviceNetworkInfo(
        "device-admin-zhejiang",
        {"198.51.100.8", "China", "Zhejiang", "",
         "Zhejiang, China"});
    presence.OnLogin("device-admin-offline", "device-admin-offline", hdl);
    presence.SetDeviceNetworkInfo(
        "device-admin-offline",
        {"203.0.113.9", "Offline Country", "Offline Region", "",
         "Offline Region, Offline Country"});
    presence.OnLogout("device-admin-offline");
    presence.OnLogin("device-admin-control", "device-admin-control", hdl);
    presence.OnLogin("web-admin-1", "web-admin-1", hdl);
    presence.SetDeviceNetworkInfo(
        "web-admin-1",
        {"198.51.100.10", "China", "Shanghai", "",
         "Shanghai, China"});
    db.StartRemoteControlSession("tx-admin", "device-admin-1",
                                 "device-admin-offline");
    db.EndRemoteControlSession("tx-admin", "device-admin-1",
                               "device-admin-offline");
    db.StartRemoteControlSession("tx-admin-live", "device-admin-1",
                                 "device-admin-offline");
    db.StartRemoteControlSession("tx-admin-clone", "device-admin-1",
                                 "C-device-admin-control");
    AdminController db_controller(&auth, &presence, transmission, &db,
                                  [](const std::string&, nlohmann::json) {});
    AdminHttpResponse db_overview = db_controller.Handle(
        {"GET", "/api/admin/overview?device_search=device-admin-1", "",
         "cd_admin_session=" + *token});
    expect(db_overview.status == 200,
           "overview with device durations returns ok");
    auto db_overview_body = nlohmann::json::parse(db_overview.body);
    expect(db_overview_body["devices"].size() == 1,
           "overview defaults to filtered online devices");
    expect(db_overview_body["devices"][0]["online_since"] > 0,
           "overview reports device online_since");
    expect(db_overview_body["devices"][0].contains("online_duration_seconds"),
           "overview reports device online duration");
    expect(db_overview_body["devices"][0].contains("total_online_seconds"),
           "overview reports device total online duration");
    expect(db_overview_body["devices"][0].contains("total_control_seconds"),
           "overview reports device total control duration");
    expect(db_overview_body["devices"][0].contains("total_controlled_seconds"),
           "overview reports device total controlled duration");
    expect(db_overview_body["devices"][0]["client_ip"] == "203.0.113.8",
           "overview reports current in-memory device client ip");
    expect(db_overview_body["devices"][0]["geo_city"] == "",
           "overview leaves geo city empty");
    expect(db_overview_body["devices"][0]["geo_location"] ==
               "Test Region, Testland",
           "overview ignores stale database geo location");
    expect(db_overview_body["devices"][0].contains("current_control_seconds"),
           "overview reports device current control duration");
    expect(db_overview_body["devices"][0].contains(
               "current_controlled_seconds"),
           "overview reports device current controlled duration");
    expect(db_overview_body["stats"].contains("online_duration_seconds"),
           "overview stats include online duration");
    expect(db_overview_body["stats"].contains("total_control_seconds"),
           "overview stats include total control duration");
    expect(db_overview_body["stats"].contains("total_controlled_seconds"),
           "overview stats include total controlled duration");
    expect(db_overview_body["device_counts"]["all"] == 1,
           "overview search scopes all device count");
    expect(db_overview_body["device_counts"]["online"] == 1,
           "overview reports online device count");
    expect(db_overview_body["device_counts"]["offline"] == 0,
           "overview search scopes offline count");
    expect(db_overview_body["device_counts"]["active"] == 1,
           "overview search scopes active count");
    expect(db_overview_body["device_counts"]["web"] == 0,
           "overview search scopes web count");
    expect(db_overview_body["device_kind_counts"]["pc"] == 1,
           "overview reports searched PC client kind count");
    expect(db_overview_body["device_kind_counts"]["web"] == 0,
           "overview reports searched web client kind count");
    expect(db_overview_body["geo_distribution"]["total_count"] == 4,
           "overview geo distribution total includes web clients");
    expect(db_overview_body["geo_distribution"]["domestic_count"] == 2,
           "overview reports domestic user count");
    expect(db_overview_body["geo_distribution"]["foreign_count"] == 1,
           "overview reports foreign user count");
    expect(db_overview_body["geo_distribution"]["unknown_count"] == 1,
           "overview keeps unknown user count");
    bool found_testland = false;
    for (const auto& country :
         db_overview_body["geo_distribution"]["countries"]) {
      if (country["country"] == "Testland" && country["count"] == 1) {
        found_testland = true;
      }
    }
    expect(found_testland, "overview reports foreign country user count");
    bool found_zhejiang = false;
    for (const auto& province :
         db_overview_body["geo_distribution"]["provinces"]) {
      if (province["province"] == "zhejiang" && province["count"] == 1) {
        found_zhejiang = true;
      }
    }
    expect(found_zhejiang, "overview reports china province user count");
    bool found_shanghai = false;
    for (const auto& province :
         db_overview_body["geo_distribution"]["provinces"]) {
      if (province["province"] == "shanghai" && province["count"] == 1) {
        found_shanghai = true;
      }
    }
    expect(found_shanghai, "overview includes web client province count");

    AdminHttpResponse offline_overview = db_controller.Handle(
        {"GET",
         "/api/admin/overview?device_filter=offline&device_search=device-admin-offline",
         "",
         "cd_admin_session=" + *token});
    expect(offline_overview.status == 200,
           "overview with offline device returns ok");
    auto offline_body = nlohmann::json::parse(offline_overview.body);
    expect(offline_body["devices"].size() == 1,
           "overview keeps offline device in presence list");
    expect(!offline_body["devices"][0]["online"].get<bool>(),
           "overview reports offline status");
    expect(offline_body["devices"][0]["last_online_at"] > 0,
           "overview reports last online timestamp for offline device");
    expect(offline_body["devices"][0]["online_duration_seconds"] == 0,
           "overview reports zero current duration for offline device");
    expect(offline_body["devices"][0]["client_ip"] == "",
           "overview clears transient network info for offline device");
    expect(offline_body["devices"][0]["active_control_count"] == 1,
           "overview preserves controller details outside the active filter");

    AdminHttpResponse active_overview = db_controller.Handle(
        {"GET",
         "/api/admin/overview?device_filter=active&device_sort=device_id&device_order=asc",
         "",
         "cd_admin_session=" + *token});
    expect(active_overview.status == 200,
           "overview with active device filter returns ok");
    auto active_body = nlohmann::json::parse(active_overview.body);
    expect(active_body["devices_page"]["total"] == 1 &&
               active_body["device_counts"]["active"] == 1,
           "overview counts a shared controlled device once");
    expect(active_body["devices"].size() == 1,
           "overview returns only controlled devices");
    expect(active_body["devices"][0]["id"] == "device-admin-1",
           "overview active filter returns the host");
    expect(active_body["devices"][0]["active_controlled_count"] == 2,
           "overview reports active controlled count");
    auto first_controlled_by =
        active_body["devices"][0]["active_controlled_by"]
            .get<std::vector<std::string>>();
    expect(contains_id(first_controlled_by, "device-admin-offline") &&
               contains_id(first_controlled_by, "device-admin-control"),
           "overview reports active controlled-by peer ids");
    AdminHttpResponse control_overview = db_controller.Handle(
        {"GET",
         "/api/admin/overview?device_filter=all&device_search=device-admin-control",
         "",
         "cd_admin_session=" + *token});
    expect(control_overview.status == 200,
           "overview with controller search returns ok");
    auto control_body = nlohmann::json::parse(control_overview.body);
    expect(control_body["device_counts"]["active"] == 0,
           "overview excludes controllers from the controlled count");
    expect(control_body["devices"].size() == 1 &&
               control_body["devices"][0]["id"] == "device-admin-control" &&
               control_body["devices"][0]["active_control_count"] == 1,
           "overview maps clone guest control count to base device");
    auto clone_targets = control_body["devices"][0]["active_control_targets"]
                             .get<std::vector<std::string>>();
    expect(contains_id(clone_targets, "device-admin-1"),
           "overview maps clone guest control target to base device");

    AdminHttpResponse active_control_overview = db_controller.Handle(
        {"GET",
         "/api/admin/overview?device_filter=active&device_search=device-admin-control",
         "",
         "cd_admin_session=" + *token});
    expect(active_control_overview.status == 200,
           "overview with active controller search returns ok");
    auto active_control_body =
        nlohmann::json::parse(active_control_overview.body);
    expect(active_control_body["devices_page"]["total"] == 0 &&
               active_control_body["device_counts"]["active"] == 0 &&
               active_control_body["devices"].empty(),
           "overview controlled filter excludes controllers from count and list");

    AdminHttpResponse location_desc_overview = db_controller.Handle(
        {"GET",
         "/api/admin/overview?device_filter=all&device_sort=location&device_order=desc&device_limit=2",
         "",
         "cd_admin_session=" + *token});
    expect(location_desc_overview.status == 200,
           "overview with location status descending sort returns ok");
    auto location_desc_body =
        nlohmann::json::parse(location_desc_overview.body);
    expect(location_desc_body["devices"].size() == 2,
           "overview applies pagination after location status sort");
    bool location_desc_has_current_location = false;
    for (const auto& device : location_desc_body["devices"]) {
      expect(!device["geo_location"].get<std::string>().empty(),
             "overview location status descending shows known locations first");
      if (device["id"] == "device-admin-1" &&
          device["geo_location"] == "Test Region, Testland") {
        location_desc_has_current_location = true;
      }
    }
    expect(location_desc_has_current_location,
           "overview location status sort uses current in-memory location");

    AdminHttpResponse location_asc_overview = db_controller.Handle(
        {"GET",
         "/api/admin/overview?device_filter=all&device_sort=location&device_order=asc&device_limit=2",
         "",
         "cd_admin_session=" + *token});
    expect(location_asc_overview.status == 200,
           "overview with location status ascending sort returns ok");
    auto location_asc_body =
        nlohmann::json::parse(location_asc_overview.body);
    expect(location_asc_body["devices"].size() == 2,
           "overview returns unknown locations on first ascending page");
    for (const auto& device : location_asc_body["devices"]) {
      expect(device["geo_location"].get<std::string>().empty(),
             "overview location status ascending shows unknown locations first");
    }

    AdminHttpResponse web_overview = db_controller.Handle(
        {"GET", "/api/admin/overview?device_filter=web", "",
         "cd_admin_session=" + *token});
    expect(web_overview.status == 200,
           "overview with web client filter returns ok");
    auto web_body = nlohmann::json::parse(web_overview.body);
    expect(web_body["devices"].size() == 1,
           "overview returns web clients on web filter");
    expect(web_body["devices"][0]["kind"] == "web",
           "overview marks web client kind");

    AdminHttpResponse web_kind_overview = db_controller.Handle(
        {"GET",
         "/api/admin/overview?device_kind=web&device_filter=online",
         "",
         "cd_admin_session=" + *token});
    expect(web_kind_overview.status == 200,
           "overview with web client kind returns ok");
    auto web_kind_body = nlohmann::json::parse(web_kind_overview.body);
    expect(web_kind_body["devices_page"]["kind"] == "web",
           "overview echoes normalized web client kind");
    expect(web_kind_body["device_counts"]["online"] == 1,
           "overview counts online web clients for selected kind");
    expect(web_kind_body["device_kind_counts"]["pc"] == 4,
           "overview reports PC client kind count");
    expect(web_kind_body["device_kind_counts"]["web"] == 1,
           "overview reports web client kind count");
    expect(web_kind_body["devices"].size() == 1 &&
               web_kind_body["devices"][0]["kind"] == "web",
           "overview filters device list by web client kind");

    auto restored_transmission = std::make_shared<TransmissionManager>();
    AdminController restored_controller(
        &auth, nullptr, restored_transmission, &db,
        [](const std::string&, nlohmann::json) {});
    AdminHttpResponse restored_stats = restored_controller.Handle(
        {"GET", "/api/admin/stats", "", "cd_admin_session=" + *token});
    expect(restored_stats.status == 200,
           "restored stats with persisted sessions returns ok");
    auto restored_stats_body = nlohmann::json::parse(restored_stats.body);
    expect(restored_stats_body["stats"]["active_connection_count"] == 2,
           "stats reports persisted active connection count");

    AdminHttpResponse restored_sessions = restored_controller.Handle(
        {"GET",
         "/api/admin/overview?session_search=tx-admin-live&session_limit=10",
         "",
         "cd_admin_session=" + *token});
    expect(restored_sessions.status == 200,
           "overview lists persisted sessions without memory state");
    auto restored_sessions_body =
        nlohmann::json::parse(restored_sessions.body);
    expect(restored_sessions_body["sessions_page"]["total"] == 1,
           "overview reports persisted session total");
    expect(restored_sessions_body["sessions"].size() == 1 &&
               restored_sessions_body["sessions"][0]["transmission_id"] ==
                   "tx-admin-live",
           "overview returns persisted session row");

    AdminHttpResponse restored_disconnect = restored_controller.Handle(
        {"POST", "/api/admin/sessions/tx-admin-live/disconnect", "",
         "cd_admin_session=" + *token});
    expect(restored_disconnect.status == 200,
           "disconnect clears persisted session without memory state");
    expect(db.CountActiveRemoteControlConnections() == 1,
           "disconnect removes persisted active connection");
  }
  std::filesystem::remove(db_path);

  AdminHttpResponse disconnect = controller.Handle(
      {"POST", "/api/admin/sessions/host-1/disconnect", "",
       "cd_admin_session=" + *token});
  expect(disconnect.status == 200, "disconnect returns ok");
  expect(transmission->GetTransmissionSnapshots().empty(),
         "disconnect releases transmission");

  AdminHttpResponse already_closed = controller.Handle(
      {"POST", "/api/admin/sessions/host-1/disconnect", "",
       "cd_admin_session=" + *token});
  expect(already_closed.status == 200,
         "disconnect missing transmission is idempotent");
  expect(already_closed.body.find("already_closed") != std::string::npos,
         "idempotent disconnect reports already_closed");

  return failures == 0 ? 0 : 1;
}
