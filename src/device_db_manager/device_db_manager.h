/*
 * @Author: DI JUNKUN
 * @Date: 2025-06-19
 * Copyright (c) 2025 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _DEVICE_DB_MANAGER_H_
#define _DEVICE_DB_MANAGER_H_

#include <sqlite3.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

struct DeviceCredential {
  std::string device_id;
  std::string password;
  bool update;
};

struct OnlineDeviceInfo {
  std::string device_id;
  bool online = false;
  int64_t updated_at = 0;
  int64_t online_since = 0;
  int64_t online_duration_seconds = 0;
  int64_t total_online_seconds = 0;
  int64_t total_control_seconds = 0;
  int64_t total_controlled_seconds = 0;
  int64_t current_control_seconds = 0;
  int64_t current_controlled_seconds = 0;
  int64_t active_control_count = 0;
  int64_t active_controlled_count = 0;
  std::vector<std::string> active_control_targets;
  std::vector<std::string> active_controlled_by;
  std::string client_ip;
  std::string country;
  std::string region;
  std::string city;
  std::string location;
  std::string client_version;
  std::string client_platform;
};

struct ClientNetworkInfo {
  std::string client_ip;
  std::string country;
  std::string region;
  std::string city;
  std::string location;
};

struct ProvinceUserCount {
  std::string province;
  int64_t count = 0;
};

struct CountryUserCount {
  std::string country;
  int64_t count = 0;
};

struct ClientGeoDistribution {
  int64_t total_count = 0;
  int64_t domestic_count = 0;
  int64_t foreign_count = 0;
  int64_t unknown_count = 0;
  std::vector<ProvinceUserCount> provinces;
  std::vector<CountryUserCount> countries;
};

struct DevicePresenceCounts {
  int64_t all = 0;
  int64_t online = 0;
  int64_t offline = 0;
  int64_t active = 0;  // Distinct devices currently being controlled.
  int64_t web = 0;
};

struct OnlineDurationStats {
  int64_t current_online_seconds = 0;
  int64_t total_online_seconds = 0;
  int64_t total_control_seconds = 0;
  int64_t total_controlled_seconds = 0;
};

struct RemoteControlSessionInfo {
  std::string transmission_id;
  std::string host_id;
  std::vector<std::string> guest_ids;
  int64_t started_at = 0;
};

class DeviceDBManager {
 public:
  enum class OpenMode { ReadWrite, ReadOnly };
  explicit DeviceDBManager(const std::string& db_path,
                           OpenMode mode = OpenMode::ReadWrite);
  ~DeviceDBManager();
  // Only the dedicated reader worker sets this; writers keep their existing
  // transaction behavior. SQLite's progress handler bounds expensive reads.
  void SetReadDeadline(std::chrono::steady_clock::time_point deadline);
  bool ClearReadDeadline();

  DeviceDBManager(const DeviceDBManager&) = delete;
  DeviceDBManager& operator=(const DeviceDBManager&) = delete;

  DeviceCredential AddDevice(const std::string& device_id,
                             const std::string& password);

  bool UpdatePassword(const std::string& device_id,
                      const std::string& new_password);

  int VerifyDevice(const std::string& device_id, const std::string& password);
  bool RemoveDevice(const std::string& device_id);

  bool SetDeviceOnline(const std::string& device_id, bool online);
  bool UpdateDeviceNetworkInfo(const std::string& device_id,
                               const ClientNetworkInfo& network_info);
  bool UpdateDeviceClientInfo(const std::string& device_id,
                              const std::string& client_version,
                              const std::string& client_platform);
  bool RecordRuntimeHeartbeat();
  bool StartRemoteControlSession(const std::string& transmission_id,
                                 const std::string& host_id,
                                 const std::string& guest_id);
  bool EndRemoteControlSession(const std::string& transmission_id,
                               const std::string& host_id,
                               const std::string& guest_id);
  bool EndRemoteControlTransmission(const std::string& transmission_id);
  int CountActiveRemoteControlConnections();
  int CountRemoteControlTransmissions(const std::string& search = "");
  std::vector<RemoteControlSessionInfo> ListRemoteControlSessions(
      size_t limit, size_t offset, const std::string& search = "");
  int GetOnlineDeviceCount();
  int CountOnlineDevices(const std::string& search = "");
  int CountDevicePresence(const std::string& search = "",
                          const std::string& filter = "all",
                          const std::string& kind = "pc");
  DevicePresenceCounts CountDevicePresenceByFilters(
      const std::string& search = "", const std::string& kind = "pc");
  OnlineDurationStats GetOnlineDurationStats();
  ClientGeoDistribution GetClientGeoDistribution();
  std::vector<OnlineDeviceInfo> ListOnlineDevices();
  std::vector<OnlineDeviceInfo> ListOnlineDevices(
      size_t limit, size_t offset, const std::string& search);
  std::vector<OnlineDeviceInfo> ListDevicePresence(
      size_t limit, size_t offset, const std::string& search,
      const std::string& filter = "all",
      const std::string& sort = "status",
      const std::string& order = "desc", const std::string& kind = "pc");
  std::vector<std::pair<std::string, bool>> BatchQueryOnline(
      const std::vector<std::string>& device_ids);
  bool SetUserDevices(const std::string& user_id,
                      const std::vector<std::string>& device_ids);
  std::vector<std::string> GetUserDevices(const std::string& user_id);

 private:
  void InitDB();
  std::string Sha256(const std::string& str);
  std::string GenerateDeviceId();
  std::string GeneratePassword();
  std::string GenerateSalt();
  bool DeviceIdExists(const std::string& device_id);
  int64_t GetRuntimeLastSeen();

  std::string HashPasswordWithSalt(const std::string& salt,
                                   const std::string& password);

 private:
  std::chrono::steady_clock::time_point read_deadline_ = std::chrono::steady_clock::time_point::max();
  sqlite3* db_;
  mutable std::recursive_mutex db_mutex_;
};

#endif  // _DEVICE_DB_MANAGER_H_
