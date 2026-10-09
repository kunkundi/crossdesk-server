/*
 * @Author: DI JUNKUN
 * @Date: 2026-10-09
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _DEVICE_AUTH_LIMITER_H_
#define _DEVICE_AUTH_LIMITER_H_

#include <chrono>
#include <cstddef>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

// Shared by every device-password entry point. Keys are the transport peer IP
// and the exact device ID, never a connection handle or client-supplied user
// ID.
class DeviceAuthLimiter {
 public:
  using Clock = std::chrono::steady_clock;
  using Now = std::function<Clock::time_point()>;
  static constexpr size_t kSourceFailures = 20;
  static constexpr size_t kTargetFailures = 5;
  static constexpr size_t kMaxEntries = 16384;  // Per dimension.
  static constexpr std::chrono::seconds kWindow{900};

  struct Result {
    bool authenticated = false;
    int retry_after = 0;  // Positive only when verification was not attempted.
  };

  explicit DeviceAuthLimiter(Now now = Clock::now);
  Result Verify(const std::string& source, const std::string& target,
                const std::function<bool()>& verify);

 private:
  struct Failures {
    size_t count = 0;
    Clock::time_point expires;
  };
  using Buckets = std::unordered_map<std::string, Failures>;
  void Prune(Buckets& buckets, Clock::time_point now);
  static void Record(Buckets& buckets, const std::string& key, size_t limit,
                     Clock::time_point now);

  Now now_;
  std::mutex mutex_;
  Buckets sources_, targets_;
  Clock::time_point next_cleanup_{};
};

#endif