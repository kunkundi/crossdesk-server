/*
 * @Author: DI JUNKUN
 * @Date: 2026-09-29
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _NOTIFICATION_SERVICE_H_
#define _NOTIFICATION_SERVICE_H_

#include <functional>
#include <optional>
#include <string>
#include <utility>

#include "notification_store.h"

struct NotificationResult {
  int status;
  nlohmann::json body;
};

// Notification-specific validation, protocol and publication behavior live
// here. Authentication and transport stay with the calling adapters.
class NotificationService {
 public:
  explicit NotificationService(
      const std::string& db_path,
      NotificationStore::OpenMode mode = NotificationStore::OpenMode::ReadWrite)
      : store_(db_path, mode) {}

  static bool IsAdminRoute(const std::string& resource);
  static bool IsClientMessage(const std::string& type);
  // Requires an authenticated administrator; POST also requires JSON + CSRF
  // validation by the HTTP adapter before entering the notification module.
  // POST saves by default; action="delete" removes an unpublished revision.
  NotificationResult HandleAdminRequest(const std::string& method,
                                        const std::string& resource,
                                        const std::string& body);
  // Requires an authenticated client. Invalid protocol fields yield no reply.
  std::optional<nlohmann::json> HandleClientMessage(
      const nlohmann::json& message);
  void SetBroadcastCallback(std::function<void(nlohmann::json)> callback) {
    broadcast_ = std::move(callback);
  }
  void SetReadDeadline(std::chrono::steady_clock::time_point deadline) {
    store_.SetReadDeadline(deadline);
  }
  bool ClearReadDeadline() { return store_.ClearReadDeadline(); }

 private:
  NotificationStore store_;
  std::function<void(nlohmann::json)> broadcast_;
};


#endif
