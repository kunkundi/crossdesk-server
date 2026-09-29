/*
 * @Author: DI JUNKUN
 * @Date: 2026-09-29
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _NOTIFICATION_STORE_H_
#define _NOTIFICATION_STORE_H_

#include <sqlite3.h>

#include <chrono>
#include <cstdint>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>

// Owns announcement schema and a separate SQLite connection. The existing
// database path is injected so upgrades retain announcements in place.
class NotificationStore {
 public:
  enum class OpenMode { ReadWrite, ReadOnly };
  explicit NotificationStore(const std::string& db_path,
                             OpenMode mode = OpenMode::ReadWrite);
  ~NotificationStore();
  NotificationStore(const NotificationStore&) = delete;
  NotificationStore& operator=(const NotificationStore&) = delete;

  nlohmann::json List(bool include_drafts, int offset,
                      bool summary_only = false);
  nlohmann::json Save(int64_t id, int64_t revision, const std::string& title,
                      const std::string& body, bool published);
  // Deletes only an unpublished announcement at the specified revision.
  nlohmann::json Delete(int64_t id, int64_t revision);
  // Set/clear on the owning reader worker, around a single HTTP request.
  void SetReadDeadline(std::chrono::steady_clock::time_point deadline);
  bool ClearReadDeadline();

 private:
  void InitializeSchema();
  sqlite3* db_ = nullptr;
  std::mutex mutex_;
  std::chrono::steady_clock::time_point read_deadline_ =
      (std::chrono::steady_clock::time_point::max)();
};

#endif
