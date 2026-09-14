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
      {"203.0.113.8", "Testland", "Test Region", "",
       "Test Region, Testland"});
  ClientNetworkInfo network_info;
  expect(presence.GetDeviceNetworkInfo("device-1", &network_info) &&
             network_info.client_ip == "203.0.113.8",
         "presence stores current device network info in memory");
  presence.OnLogin("device-2", "device-2", hdl);
  presence.SetDeviceNetworkInfo("device-2", {"203.0.113.8", "", "", "", ""});
  expect(presence.HasDeviceWithClientIp("203.0.113.8"),
         "presence tracks devices by current client ip");
  expect(presence.UpdateDevicesWithClientIp(
             "203.0.113.8",
             {"203.0.113.8", "Sharedland", "Shared Region", "",
              "Shared Region, Sharedland"}) == 2,
         "presence updates all current devices sharing an ip");
  expect(presence.GetDeviceNetworkInfo("device-2", &network_info) &&
             network_info.location == "Shared Region, Sharedland",
         "presence applies shared ip geo result to all matching devices");
  presence.OnLogin("web-2", "web-2", hdl);
  presence.SetDeviceNetworkInfo(
      "web-2",
      {"198.51.100.10", "China", "Zhejiang", "Hangzhou",
       "Hangzhou, Zhejiang, China"});
  presence.OnLogin("device-country-only", "device-country-only", hdl);
  presence.SetDeviceNetworkInfo(
      "device-country-only",
      {"198.51.100.11", "China", "", "", "China"});
  auto distribution = presence.GetClientGeoDistribution();
  expect(distribution.total_count == 4 && distribution.domestic_count == 2 &&
             distribution.foreign_count == 2 && distribution.unknown_count == 0,
         "presence geo distribution includes current web client network info");
  expect(distribution.provinces.size() == 1 &&
             distribution.provinces[0].province == "zhejiang" &&
             distribution.provinces[0].count == 1,
         "presence geo distribution reports domestic province counts");
  expect(distribution.countries.size() == 1 &&
             distribution.countries[0].country == "Sharedland" &&
             distribution.countries[0].count == 2,
         "presence geo distribution reports foreign country counts");
  presence.OnLogout("device-country-only");
  presence.OnLogout("device-1");
  expect(!presence.GetDeviceNetworkInfo("device-1", &network_info),
         "presence clears current network info on logout");
  expect(presence.GetClientGeoDistribution().total_count == 2,
         "presence geo distribution keeps other online clients");
  presence.OnLogout("web-2");
  presence.OnLogout("device-2");
  expect(!presence.HasDeviceWithClientIp("203.0.113.8"),
         "presence drops ip tracking after last matching device logs out");

  const auto geo_db_path =
      std::filesystem::temp_directory_path() /
      ("crossdesk_geo_distribution_test_" +
       std::to_string(std::chrono::steady_clock::now()
                          .time_since_epoch()
                          .count()) +
       ".db");
  {
    DeviceDBManager geo_db(geo_db_path.string());
    geo_db.SetDeviceOnline("geo-country-only", true);
    geo_db.UpdateDeviceNetworkInfo("geo-country-only",
                                   {"198.51.100.12", "China", "", "", "China"});
    auto db_country_only = geo_db.GetClientGeoDistribution();
    expect(db_country_only.total_count == 1 &&
               db_country_only.domestic_count == 1 &&
               db_country_only.unknown_count == 0,
           "database geo distribution treats country-only locations as known");
  }
  std::filesystem::remove(geo_db_path);

  const auto db_path =
      std::filesystem::temp_directory_path() /
      ("crossdesk_presence_manager_test_" +
       std::to_string(std::chrono::steady_clock::now()
                          .time_since_epoch()
                          .count()) +
       ".db");
  int64_t total_online_before_restart = 0;
  int64_t total_control_before_restart = 0;
  int64_t total_controlled_before_restart = 0;
  {
    DeviceDBManager db(db_path.string());
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
    expect(online_devices[0].total_online_seconds >=
               online_devices[0].online_duration_seconds,
           "online device list includes total online duration");
    db.UpdateDeviceNetworkInfo(
        "device-1",
        {"198.51.100.10", "China", "Zhejiang", "Hangzhou",
         "Hangzhou, Zhejiang, China"});
    db.UpdateDeviceNetworkInfo(
        "web-1",
        {"203.0.113.10", "Webland", "Web Region", "",
         "Web Region, Webland"});
    db.UpdateDeviceNetworkInfo(
        "C-000000",
        {"203.0.113.11", "Cloneland", "Clone Region", "",
         "Clone Region, Cloneland"});
    auto db_distribution = db.GetClientGeoDistribution();
    expect(db_distribution.total_count == 2 &&
               db_distribution.domestic_count == 1 &&
               db_distribution.foreign_count == 1,
           "database geo distribution includes web clients and excludes clones");
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
    auto accumulated_stats = db.GetOnlineDurationStats();
    expect(accumulated_stats.total_online_seconds >= 1,
           "database accumulates total online duration after logout");
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
    auto known_location_first =
        db.ListDevicePresence(10, 0, "", "all", "location", "desc");
    expect(!known_location_first.empty() &&
               !known_location_first[0].location.empty(),
           "presence list location sort groups known locations first");
    auto unknown_location_first =
        db.ListDevicePresence(10, 0, "", "all", "location", "asc");
    expect(!unknown_location_first.empty() &&
               unknown_location_first[0].location.empty(),
           "presence list location sort groups unknown locations first");
    auto offline_devices = db.ListDevicePresence(10, 0, "device-1");
    expect(offline_devices.size() == 1 && !offline_devices[0].online,
           "presence list includes offline device");
    expect(offline_devices[0].updated_at > 0,
           "offline device keeps last online timestamp");
    expect(offline_devices[0].online_since == 0,
           "offline device clears current online start");
    expect(offline_devices[0].online_duration_seconds == 0,
           "offline device current online duration is zero");
    expect(offline_devices[0].total_online_seconds >= 1,
           "offline device keeps accumulated online duration");
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
    auto clone_persisted = db.ListDevicePresence(10, 0, "device-4");
    expect(!clone_persisted.empty() &&
               clone_persisted[0].total_control_seconds >= 1,
           "clone guest control duration persists on base device");
    auto guest_control = db.ListDevicePresence(10, 0, "device-1");
    auto host_controlled = db.ListDevicePresence(10, 0, "device-2");
    expect(!guest_control.empty() &&
               guest_control[0].total_control_seconds >= 1,
           "guest accumulates active control duration");
    expect(!host_controlled.empty() &&
               host_controlled[0].total_controlled_seconds >= 1,
           "host accumulates active controlled duration");
    auto active_remote_stats = db.GetOnlineDurationStats();
    expect(active_remote_stats.total_control_seconds >= 1,
           "stats include active control duration");
    expect(active_remote_stats.total_controlled_seconds >= 1,
           "stats include active controlled duration");
    expect(db.EndRemoteControlSession("tx-1", "device-2", "device-1"),
           "remote control session ends");
    expect(db.CountDevicePresence("", "active") == 0 &&
               db.CountDevicePresenceByFilters().active == 0 &&
               db.ListDevicePresence(10, 0, "", "active").empty(),
           "controlled device count and list clear after the last connection ends");
    auto remote_stats = db.GetOnlineDurationStats();
    expect(remote_stats.total_control_seconds >= 1,
           "stats persist total control duration");
    expect(remote_stats.total_controlled_seconds >= 1,
           "stats persist total controlled duration");
    expect(db.StartRemoteControlSession("tx-stale", "device-2", "device-3"),
           "stale remote control session starts before restart");
    std::this_thread::sleep_for(std::chrono::seconds(1));
    auto restart_checkpoint_stats = db.GetOnlineDurationStats();
    total_online_before_restart =
        restart_checkpoint_stats.total_online_seconds;
    total_control_before_restart =
        restart_checkpoint_stats.total_control_seconds;
    total_controlled_before_restart =
        restart_checkpoint_stats.total_controlled_seconds;
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
    expect(restarted_stats.total_online_seconds >=
               total_online_before_restart,
           "database preserves total online duration across restart");
    expect(restarted_stats.total_control_seconds >=
               total_control_before_restart,
           "database preserves total control duration across restart");
    expect(restarted_stats.total_controlled_seconds >=
               total_controlled_before_restart,
           "database preserves total controlled duration across restart");
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
  std::filesystem::remove(db_path);

  return failures == 0 ? 0 : 1;
}
