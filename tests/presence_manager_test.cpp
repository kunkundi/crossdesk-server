#include "presence_manager.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "device_db_manager.h"

int main() {
  PresenceManager presence;
  websocketpp::connection_hdl hdl;
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

  expect(presence.GetOnlineDeviceCount() == 0,
         "initial online device count is zero");
  expect(presence.GetOnlineWebClientCount() == 0,
         "initial online web client count is zero");

  presence.OnLogin("device-1", "device-1", hdl);
  expect(presence.GetOnlineDeviceCount() == 1,
         "regular device increments online device count");
  expect(presence.GetOnlineWebClientCount() == 0,
         "regular device does not increment web client count");

  presence.OnLogin("web-1", "web-1", hdl);
  expect(presence.GetOnlineDeviceCount() == 1,
         "web client does not increment online device count");
  expect(presence.GetOnlineWebClientCount() == 1,
         "web client increments web client count");

  presence.OnLogin("C-000000", "C-000000", hdl);
  expect(presence.GetOnlineDeviceCount() == 1,
         "clone client does not increment online device count");
  expect(presence.GetOnlineWebClientCount() == 1,
         "clone client does not increment web client count");

  presence.OnLogout("C-000000");
  expect(presence.GetOnlineDeviceCount() == 1,
         "clone logout leaves online device count unchanged");
  expect(presence.GetOnlineWebClientCount() == 1,
         "clone logout leaves web client count unchanged");

  presence.OnLogout("web-1");
  expect(presence.GetOnlineDeviceCount() == 1,
         "web logout leaves online device count unchanged");
  expect(presence.GetOnlineWebClientCount() == 0,
         "web logout decrements web client count");

  presence.SetDeviceNetworkInfo(
      "device-1",
      {"203.0.113.8"});
  ClientNetworkInfo network_info;
  expect(presence.GetDeviceNetworkInfo("device-1", &network_info) &&
             network_info.client_ip == "203.0.113.8",
         "presence stores current device network info in memory");
  presence.OnLogout("device-1");
  expect(!presence.GetDeviceNetworkInfo("device-1", &network_info),
         "presence clears current network info on logout");
  const auto db_path =
      std::filesystem::temp_directory_path() /
      ("crossdesk_presence_manager_test_" +
       std::to_string(std::chrono::steady_clock::now()
                          .time_since_epoch()
                          .count()) +
       ".db");
  auto execute_sql = [&](const std::string& sql) {
    sqlite3* connection = nullptr;
    const bool opened = sqlite3_open(db_path.string().c_str(), &connection) == SQLITE_OK;
    const bool ok = opened && sqlite3_exec(connection, sql.c_str(), nullptr,
                                           nullptr, nullptr) == SQLITE_OK;
    expect(ok, "prepare database fixture");
    sqlite3_close(connection);
  };
  auto scalar = [&](const std::string& sql) -> int64_t {
    sqlite3* connection = nullptr;
    sqlite3_stmt* statement = nullptr;
    int64_t result = -1;
    if (sqlite3_open(db_path.string().c_str(), &connection) == SQLITE_OK &&
        sqlite3_prepare_v2(connection, sql.c_str(), -1, &statement, nullptr) == SQLITE_OK &&
        sqlite3_step(statement) == SQLITE_ROW) {
      result = sqlite3_column_int64(statement, 0);
    } else {
      expect(false, "read database fixture");
    }
    sqlite3_finalize(statement);
    sqlite3_close(connection);
    return result;
  };
  {
    DeviceDBManager db(db_path.string());
    expect(scalar("SELECT COUNT(*) FROM pragma_table_info('device_presence') "
                  "WHERE name IN ('total_online_seconds', 'total_control_seconds', "
                  "'total_controlled_seconds');") == 0,
           "new databases do not store cumulative durations");
    db.SetDeviceOnline("device-1", true);
    db.SetDeviceOnline("web-1", true);
    db.SetDeviceOnline("C-000000", true);
    expect(db.GetOnlineDeviceCount() == 1,
           "database online device count excludes web and clone clients");
    expect(db.CountDevicePresence() == 1,
           "database device presence count excludes web and clone clients");
    auto online_devices = db.ListOnlineDevices();
    expect(online_devices.size() == 1,
           "online device list excludes web and clone clients");
    expect(online_devices[0].device_id == "device-1",
           "online device list returns regular device id");
    expect(online_devices[0].online,
           "online device list marks device online");
    expect(online_devices[0].updated_at > 0,
           "online device list includes updated_at");
    expect(online_devices[0].online_since > 0,
           "online device list includes online_since");
    expect(online_devices[0].online_duration_seconds >= 0,
           "online device list includes current online duration");
    db.SetDeviceOnline("device-2", true);
    db.SetDeviceOnline("device-3", true);
    expect(db.CountOnlineDevices() == 3,
           "database online device count includes regular devices");
    expect(db.CountDevicePresence() == 3,
           "database device presence count includes regular devices");
    expect(db.ListOnlineDevices(2, 0, "").size() == 2,
           "online device list supports page limit");
    expect(db.ListOnlineDevices(2, 2, "").size() == 1,
           "online device list supports page offset");
    auto filtered_devices = db.ListOnlineDevices(10, 0, "device-2");
    expect(db.CountOnlineDevices("device-2") == 1,
           "online device count supports search");
    expect(filtered_devices.size() == 1 &&
               filtered_devices[0].device_id == "device-2",
           "online device list supports search");
    std::this_thread::sleep_for(std::chrono::seconds(1));
    auto duration_stats = db.GetOnlineDurationStats();
    expect(duration_stats.current_online_seconds >= 1,
           "database sums current online duration");
    db.SetDeviceOnline("device-1", false);
    expect(db.CountOnlineDevices() == 2,
           "offline device no longer counts as online");
    expect(db.CountDevicePresence("device-1") == 1,
           "offline device remains in presence count");
    expect(db.CountDevicePresence("", "online") == 2,
           "presence count supports online filter");
    expect(db.CountDevicePresence("", "offline") == 1,
           "presence count supports offline filter");
    auto presence_counts = db.CountDevicePresenceByFilters();
    expect(presence_counts.all == 3 && presence_counts.online == 2 &&
               presence_counts.offline == 1 && presence_counts.web == 1,
           "presence count aggregation reports all filters");
    auto web_presence_counts = db.CountDevicePresenceByFilters("", "web");
    expect(web_presence_counts.all == 1 && web_presence_counts.online == 1 &&
               web_presence_counts.offline == 0,
           "presence count aggregation supports web client kind");
    auto all_client_presence_counts =
        db.CountDevicePresenceByFilters("", "all");
    expect(all_client_presence_counts.all == 4 &&
               all_client_presence_counts.online == 3 &&
               all_client_presence_counts.offline == 1,
           "presence count aggregation supports all client kinds");
    expect(db.CountDevicePresence("", "online", "web") == 1,
           "presence count supports online web client kind");
    auto web_presence =
        db.ListDevicePresence(10, 0, "", "all", "device_id", "asc", "web");
    expect(web_presence.size() == 1 && web_presence[0].device_id == "web-1",
           "presence list supports web client kind");
    auto sorted_presence =
        db.ListDevicePresence(10, 0, "", "all", "device_id", "asc");
    expect(!sorted_presence.empty() &&
               sorted_presence[0].device_id == "device-1",
           "presence list supports device id sort");
    auto offline_devices = db.ListDevicePresence(10, 0, "device-1");
    expect(offline_devices.size() == 1 && !offline_devices[0].online,
           "presence list includes offline device");
    expect(offline_devices[0].updated_at > 0,
           "offline device keeps last online timestamp");
    expect(offline_devices[0].online_since == 0,
           "offline device clears current online start");
    expect(offline_devices[0].online_duration_seconds == 0,
           "offline device current online duration is zero");
    expect(db.StartRemoteControlSession("tx-1", "device-2", "device-1"),
           "remote control session starts");
    std::this_thread::sleep_for(std::chrono::seconds(1));
    expect(db.CountDevicePresence("", "active") == 1,
           "active presence count includes only the controlled device");
    auto active_presence_counts = db.CountDevicePresenceByFilters();
    expect(active_presence_counts.active == 1,
           "active presence aggregation includes only the controlled device");
    expect(db.CountDevicePresence("device-1", "active") == 0 &&
               db.CountDevicePresenceByFilters("device-1").active == 0,
           "active presence search excludes the controller");
    auto controller_devices = db.ListDevicePresence(10, 0, "device-1");
    expect(controller_devices.size() == 1 &&
               controller_devices[0].active_control_count == 1,
           "unfiltered presence list reports active control count");
    expect(!controller_devices.empty() &&
               controller_devices[0].current_control_seconds >= 1,
           "unfiltered presence list reports current control duration");
    expect(!controller_devices.empty() &&
               contains_id(controller_devices[0].active_control_targets,
                           "device-2"),
           "unfiltered presence list reports active control target id");
    auto active_devices =
        db.ListDevicePresence(10, 0, "", "active", "device_id", "asc");
    expect(active_devices.size() == 1,
           "active presence list includes only the controlled device");
    expect(!active_devices.empty() &&
               active_devices[0].device_id == "device-2" &&
               active_devices[0].active_controlled_count == 1,
           "presence list reports active controlled count");
    expect(!active_devices.empty() &&
               active_devices[0].current_controlled_seconds >= 1,
           "presence list reports current controlled duration");
    expect(!active_devices.empty() &&
               contains_id(active_devices[0].active_controlled_by, "device-1"),
           "presence list reports active controlled-by peer id");
    db.SetDeviceOnline("device-4", true);
    expect(db.StartRemoteControlSession("tx-clone", "device-2", "C-device-4"),
           "clone remote control session starts");
    expect(db.CountDevicePresence("", "active") == 1 &&
               db.CountDevicePresenceByFilters().active == 1,
           "multiple controllers count the same controlled device once");
    auto shared_host = db.ListDevicePresence(10, 0, "", "active");
    expect(shared_host.size() == 1 &&
               shared_host[0].device_id == "device-2" &&
               shared_host[0].active_controlled_count == 2,
           "active list returns the shared host once with both connections");
    expect(db.ListDevicePresence(10, 0, "device-4", "active").empty() &&
               db.CountDevicePresenceByFilters("device-4").active == 0,
           "active presence excludes normalized clone controllers");
    auto clone_control = db.ListDevicePresence(10, 0, "device-4");
    expect(clone_control.size() == 1 &&
               clone_control[0].active_control_count == 1,
           "presence list maps clone guest control count to base device");
    expect(!clone_control.empty() &&
               contains_id(clone_control[0].active_control_targets,
                           "device-2"),
           "presence list maps clone guest control target to base device");
    std::this_thread::sleep_for(std::chrono::seconds(1));
    expect(db.EndRemoteControlSession("tx-clone", "device-2", "C-device-4"),
           "clone remote control session ends");
    expect(db.CountDevicePresence("", "active") == 1 &&
               db.CountDevicePresenceByFilters().active == 1,
           "controlled device remains counted while another connection exists");
    expect(db.StartRemoteControlSession("tx-web", "device-2", "web-1"),
           "web controller session starts");
    expect(db.CountDevicePresence("", "active", "web") == 0 &&
               db.CountDevicePresenceByFilters("", "web").active == 0 &&
               db.ListDevicePresence(10, 0, "", "active", "device_id", "asc",
                                     "web").empty(),
           "web controllers are excluded from controlled device counts and list");
    expect(db.CountDevicePresence("", "active", "all") == 1 &&
               db.CountDevicePresenceByFilters("", "all").active == 1,
           "all client kinds count only the shared controlled device");
    expect(db.EndRemoteControlSession("tx-web", "device-2", "web-1"),
           "web controller session ends");
    expect(db.StartRemoteControlSession("tx-chain", "device-3", "device-2"),
           "controlled device also starts controlling another device");
    expect(db.CountDevicePresence("", "active") == 2 &&
               db.CountDevicePresenceByFilters().active == 2,
           "two controlled devices count once each even with a dual-role device");
    auto controlled_page =
        db.ListDevicePresence(1, 1, "", "active", "device_id", "asc");
    expect(controlled_page.size() == 1 &&
               controlled_page[0].device_id == "device-3",
           "controlled device pagination excludes controller-only devices");
    expect(db.EndRemoteControlSession("tx-chain", "device-3", "device-2"),
           "chained remote control session ends");
    auto clone_ended = db.ListDevicePresence(10, 0, "device-4");
    expect(!clone_ended.empty() && clone_ended[0].current_control_seconds == 0 &&
               clone_ended[0].active_control_count == 0,
           "ending a clone session clears its current duration and count");
    expect(db.EndRemoteControlSession("tx-1", "device-2", "device-1"),
           "remote control session ends");
    expect(db.CountDevicePresence("", "active") == 0 &&
               db.CountDevicePresenceByFilters().active == 0 &&
               db.ListDevicePresence(10, 0, "", "active").empty(),
           "controlled device count and list clear after the last connection ends");
    expect(db.StartRemoteControlSession("tx-stale", "device-2", "device-3"),
           "stale remote control session starts before restart");
    std::this_thread::sleep_for(std::chrono::seconds(1));
    expect(db.RecordRuntimeHeartbeat(),
           "database records runtime heartbeat before restart");
  }
  {
    DeviceDBManager restarted_db(db_path.string());
    expect(restarted_db.CountOnlineDevices() == 0,
           "database clears stale online devices on restart");
    auto restarted_stats = restarted_db.GetOnlineDurationStats();
    expect(restarted_stats.current_online_seconds == 0,
           "database clears stale current online duration on restart");
    expect(restarted_db.CountActiveRemoteControlConnections() == 1,
           "database preserves active remote control across short restart");
    expect(restarted_db.CountRemoteControlTransmissions() == 1,
           "database preserves active transmission across short restart");
    auto restored_sessions =
        restarted_db.ListRemoteControlSessions(10, 0, "tx-stale");
    expect(restored_sessions.size() == 1 &&
               restored_sessions[0].transmission_id == "tx-stale",
           "database lists restored remote control session");
    expect(!restored_sessions.empty() &&
               contains_id(restored_sessions[0].guest_ids, "device-3"),
           "database lists restored remote control guest");
    expect(restarted_db.CountDevicePresence("", "active") == 1 &&
               restarted_db.CountDevicePresenceByFilters().active == 1,
           "restored remote control counts only the controlled device");
    auto restarted_devices =
        restarted_db.ListDevicePresence(10, 0, "device-2");
    expect(!restarted_devices.empty() && !restarted_devices[0].online,
           "restart marks previously online device offline");
    expect(!restarted_devices.empty() &&
               restarted_devices[0].online_since == 0,
           "restart clears stale online_since");
  }
  // Simulate an existing deployment. Legacy columns remain readable by older
  // tools but are no longer updated or returned by the server.
  execute_sql(
      "ALTER TABLE device_presence ADD COLUMN total_online_seconds INTEGER NOT NULL DEFAULT 0;"
      "ALTER TABLE device_presence ADD COLUMN total_control_seconds INTEGER NOT NULL DEFAULT 0;"
      "ALTER TABLE device_presence ADD COLUMN total_controlled_seconds INTEGER NOT NULL DEFAULT 0;"
      "UPDATE device_presence SET total_online_seconds=101, total_control_seconds=202, "
      "total_controlled_seconds=303, online=1, online_since=strftime('%s','now')-60;");
  {
    DeviceDBManager upgraded_db(db_path.string());
    expect(upgraded_db.CountOnlineDevices() == 0,
           "legacy database resets online presence on upgrade");
    expect(upgraded_db.SetDeviceOnline("device-1", true),
           "legacy database still supports login");
    execute_sql("UPDATE device_presence SET online_since=strftime('%s','now')-60 "
                "WHERE device_id='device-1';");
    auto current = upgraded_db.ListOnlineDevices(10, 0, "device-1");
    expect(current.size() == 1 && current[0].online_duration_seconds >= 60,
           "legacy database still reports the current online period");
    expect(upgraded_db.SetDeviceOnline("device-1", false),
           "legacy database still supports logout");
    expect(upgraded_db.StartRemoteControlSession("tx-upgrade", "device-2", "C-device-1"),
           "legacy database starts remote sessions");
    execute_sql("UPDATE remote_control_sessions SET started_at=strftime('%s','now')-60 "
                "WHERE transmission_id='tx-upgrade';");
    auto controlling = upgraded_db.ListDevicePresence(10, 0, "device-1");
    expect(controlling.size() == 1 && controlling[0].current_control_seconds >= 60,
           "legacy database still reports current remote duration");
    expect(upgraded_db.EndRemoteControlSession("tx-upgrade", "device-2", "C-device-1") &&
               upgraded_db.EndRemoteControlSession("tx-upgrade", "device-2", "C-device-1"),
           "ending remote sessions remains idempotent");
    for (const auto& action : {"query", "export"}) {
      const auto report = upgraded_db.AdminDeviceData(
          "device-1", action, "history", "", "test-admin", "REQ-UPGRADE");
      expect(report.value("ok", false) && !report["data"]["presence"].empty(),
             "legacy database supports device data query and export");
      if (report.value("ok", false)) {
        for (const auto& record : report["data"]["presence"]) {
          expect(!record.contains("total_online_seconds") &&
                     !record.contains("total_control_seconds") &&
                     !record.contains("total_controlled_seconds"),
                 "device data reports exclude cumulative duration fields");
        }
      }
    }
  }
  // Expired recovery sessions must also be cleared without accumulating time.
  execute_sql("UPDATE server_runtime SET last_seen_at=strftime('%s','now')-3600;"
              "UPDATE device_presence SET online=1, online_since=strftime('%s','now')-7200;");
  {
    DeviceDBManager stale_db(db_path.string());
    expect(stale_db.CountActiveRemoteControlConnections() == 0 &&
               stale_db.CountOnlineDevices() == 0,
           "long restart clears stale sessions and online presence");
  }
  expect(scalar("SELECT COUNT(*) FROM device_presence WHERE total_online_seconds!=101 "
                "OR total_control_seconds!=202 OR total_controlled_seconds!=303;") == 0,
         "login, logout, session end and restart never update legacy totals");
  std::filesystem::remove(db_path);

  {
    DeviceDBManager filters_db(":memory:");
    filters_db.SetDeviceOnline("win-1", true);
    filters_db.SetDeviceOnline("win-2", false);
    filters_db.SetDeviceOnline("linux-1", true);
    filters_db.SetDeviceOnline("unreported", true);
    filters_db.SetDeviceOnline("web-1", true);
    filters_db.SetDeviceOnline("C-clone", true);
    filters_db.UpdateDeviceClientInfo("win-1", "1.2.3", "windows");
    filters_db.UpdateDeviceClientInfo("win-2", "1.2.3", "windows");
    filters_db.UpdateDeviceClientInfo("linux-1", "1.2.3", "linux");
    filters_db.UpdateDeviceClientInfo("web-1", "3.0.0", "web");
    filters_db.UpdateDeviceClientInfo("C-clone", "hidden-version", "hidden-platform");
    expect(filters_db.CountDevicePresence() == 4 &&
               filters_db.CountDevicePresence("", "all", "pc", "windows") == 2 &&
               filters_db.CountDevicePresence("", "all", "pc", "", "1.2.3") == 3,
           "platform and version filters are optional and compose with category counts");
    auto filtered_page = filters_db.ListDevicePresence(
        1, 1, "win-", "all", "device_id", "asc", "pc", "windows", "1.2.3");
    expect(filtered_page.size() == 1 && filtered_page[0].device_id == "win-2",
           "combined metadata and search filters apply before limit and offset");
    auto filtered_counts = filters_db.CountDevicePresenceByFilters(
        "win-", "pc", "windows", "1.2.3");
    expect(filtered_counts.all == 2 && filtered_counts.online == 1 &&
               filtered_counts.offline == 1 && filtered_counts.web == 0,
           "aggregate metadata counts agree with filtered pages");
    auto options = filters_db.ListDeviceClientFilterOptions();
    expect(options.platforms == std::vector<std::string>({"linux", "windows"}) &&
               options.versions == std::vector<std::string>({"1.2.3"}),
           "metadata options are unique and exclude unreported, clone and web values");
    expect(filters_db.ListDeviceClientFilterOptions("web").versions ==
               std::vector<std::string>({"3.0.0"}),
           "web metadata options are scoped to web clients");
    expect(filters_db.CountDevicePresence("", "all", "pc", "%") == 0 &&
               filters_db.CountDevicePresence("", "all", "pc", "", "1.2") == 0 &&
               filters_db.ListDevicePresence(10, 0, "", "all", "status", "desc",
                                              "pc", "' OR 1=1--").empty(),
           "metadata filters use literal equality and bound parameters");
    const std::string special_version = "1.2%_'build";
    filters_db.UpdateDeviceClientInfo("win-1", special_version, "windows");
    expect(filters_db.CountDevicePresence("win-", "all", "pc", "windows",
                                           special_version) == 1 &&
               filters_db.CountDevicePresenceByFilters("", "pc", "windows",
                                                       special_version).all == 1 &&
               filters_db.ListDevicePresence(10, 0, "", "all", "device_id", "asc",
                                              "pc", "windows", special_version).size() == 1,
           "version punctuation is treated consistently as data by all filter queries");
  }

  return failures == 0 ? 0 : 1;
}
