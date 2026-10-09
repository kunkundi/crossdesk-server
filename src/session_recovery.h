/*
 * @Author: DI JUNKUN
 * @Date: 2026-09-29
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _SESSION_RECOVERY_H_
#define _SESSION_RECOVERY_H_

#include <functional>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <sqlite3.h>
#include "device_db_manager.h"
#include "transmission_manager.h"

// A ticket only restores an already authorized session. Both endpoints must
// confirm that their original RTC transport still exists; no SDP is replayed.
class SessionRecovery {
 public:
  using Send = std::function<void(websocketpp::connection_hdl, nlohmann::json)>;
  SessionRecovery(const std::string& path, DeviceDBManager* db,
                  std::shared_ptr<TransmissionManager> transmission, Send send,
                  int recovery_timeout_seconds = 120);
  ~SessionRecovery();
  void Login(websocketpp::connection_hdl hdl, const nlohmann::json& request);
  void Issue(const std::string& tx, const std::string& host,
             const std::string& guest, websocketpp::connection_hdl guest_hdl);
  void Report(websocketpp::connection_hdl hdl, const nlohmann::json& message);
  void Expire();
  nlohmann::json Status();
  bool Manages(const std::string& tx, const std::string& guest);
  bool IsRecovering(const std::string& tx, const std::string& guest);

 private:
  struct Ticket {
    std::string tx, host, guest, token;
    bool host_ready = false, guest_ready = false, recovering = false;
    int64_t deadline = 0;
    websocketpp::connection_hdl host_hdl, guest_hdl;
  };
  using Key = std::pair<std::string, std::string>;
  void Save(const Ticket& ticket);
  bool Exists(const Ticket& ticket);
  void Remove(const Key& key);
  sqlite3* sql_ = nullptr;
  DeviceDBManager* db_;
  std::shared_ptr<TransmissionManager> transmission_;
  Send send_;
  std::recursive_mutex mutex_;
  std::map<Key, Ticket> tickets_;
  std::map<websocketpp::connection_hdl, bool,
           std::owner_less<websocketpp::connection_hdl>> capable_;
};

#endif
