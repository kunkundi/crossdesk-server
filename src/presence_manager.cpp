#include "presence_manager.h"

#include <nlohmann/json.hpp>

#include "log.h"

namespace {

bool IsWebClient(const std::string& device_id) {
  return device_id.rfind("web-", 0) == 0;
}

bool IsCloneClient(const std::string& device_id) {
  return device_id.rfind("C-", 0) == 0;
}

bool ShouldTrackOnlineDevice(const std::string& device_id) {
  return !IsWebClient(device_id) && !IsCloneClient(device_id);
}

}  // namespace

void PresenceManager::OnLogin(const std::string& user_id,
                              const std::string& device_id,
                              websocketpp::connection_hdl hdl) {
  {
    std::lock_guard<std::mutex> lock(online_devices_mutex_);
    if (ShouldTrackOnlineDevice(device_id)) {
      online_devices_.insert(device_id);
    } else if (IsWebClient(device_id)) {
      online_web_clients_.insert(device_id);
    }
  }
  if (db_) {
    db_->SetDeviceOnline(device_id, true);
  }
  NotifyUserDevices(user_id, device_id, true);
}

void PresenceManager::OnLogout(const std::string& device_id) {
  std::string user_id = device_id;
  {
    std::lock_guard<std::mutex> lock(associations_mutex_);
    associations_.erase(device_id);
  }
  {
    std::lock_guard<std::mutex> lock(online_devices_mutex_);
    if (ShouldTrackOnlineDevice(device_id)) {
      online_devices_.erase(device_id);
    } else if (IsWebClient(device_id)) {
      online_web_clients_.erase(device_id);
    }
  }
  if (db_) {
    db_->SetDeviceOnline(device_id, false);
  }
  {
    std::lock_guard<std::mutex> lock(network_info_mutex_);
    device_network_info_.erase(device_id);
  }
  if (!user_id.empty()) {
    NotifyUserDevices(user_id, device_id, false);
  }
}

bool PresenceManager::IsOnline(const std::string& device_id) const {
  if (!db_) return false;
  auto res = db_->BatchQueryOnline({device_id});
  return !res.empty() && res[0].second;
}

void PresenceManager::ForgetOfflineDeviceData(const std::string& device_id) {
  const auto clone = "C-" + device_id;
  {
    std::lock_guard<std::mutex> lock(associations_mutex_);
    associations_.erase(device_id);
    associations_.erase(clone);
    for (auto& entry : associations_) {
      entry.second.erase(device_id);
      entry.second.erase(clone);
    }
  }
  std::lock_guard<std::mutex> lock(network_info_mutex_);
  device_network_info_.erase(device_id);
  device_network_info_.erase(clone);
}

size_t PresenceManager::GetOnlineDeviceCount() const {
  std::lock_guard<std::mutex> lock(online_devices_mutex_);
  return online_devices_.size();
}

size_t PresenceManager::GetOnlineWebClientCount() const {
  std::lock_guard<std::mutex> lock(online_devices_mutex_);
  return online_web_clients_.size();
}

void PresenceManager::SetDeviceNetworkInfo(
    const std::string& device_id, const ClientNetworkInfo& network_info) {
  if (device_id.empty()) {
    return;
  }
  std::lock_guard<std::mutex> lock(network_info_mutex_);
  device_network_info_[device_id] = network_info;
}

bool PresenceManager::GetDeviceNetworkInfo(
    const std::string& device_id, ClientNetworkInfo* network_info) const {
  if (!network_info) {
    return false;
  }
  std::lock_guard<std::mutex> lock(network_info_mutex_);
  auto it = device_network_info_.find(device_id);
  if (it == device_network_info_.end()) {
    return false;
  }
  *network_info = it->second;
  return true;
}

std::vector<std::pair<std::string, bool>> PresenceManager::BatchQuery(
    const std::vector<std::string>& device_ids) const {
  std::vector<std::pair<std::string, bool>> result;
  if (db_) {
    return db_->BatchQueryOnline(device_ids);
  }
  return result;
}

void PresenceManager::NotifyUserDevices(const std::string& user_id,
                                        const std::string& changed_device_id,
                                        bool online) {
  (void)user_id;

  if (!send_to_device_) {
    return;
  }

  std::vector<std::string> watchers;
  {
    std::lock_guard<std::mutex> lock(associations_mutex_);
    for (const auto& kv : associations_) {
      const auto& watcher = kv.first;
      const auto& watched_set = kv.second;
      if (watched_set.find(changed_device_id) != watched_set.end()) {
        watchers.push_back(watcher);
      }
    }
  }

  if (watchers.empty()) {
    return;
  }
  std::vector<std::string> targets = watchers;
  if (db_) {
    auto statuses = db_->BatchQueryOnline(watchers);
    targets.clear();
    for (const auto& p : statuses) {
      if (p.first == changed_device_id) {
        continue;
      }
      if (p.second) targets.push_back(p.first);
    }
  }
  nlohmann::json j = {
      {"type", "presence_update"},
      {"id", changed_device_id},
      {"online", online},
  };
  for (const auto& id : targets) {
    send_to_device_(id, j);
  }
}

void PresenceManager::UpdateUserDevices(
    const std::string& user_id, const std::vector<std::string>& device_ids,
    bool replace) {
  std::lock_guard<std::mutex> lock(associations_mutex_);
  auto& setref = associations_[user_id];
  if (replace) {
    setref.clear();
  }
  for (const auto& id : device_ids) {
    setref.insert(id);
  }
}
