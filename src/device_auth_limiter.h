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
#include <utility>

// Failure budgets keyed by transport peer IP and credential target, never a
// connection handle. Use separate instances for independent auth domains.
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

  explicit DeviceAuthLimiter(Now now = Clock::now,
                             size_t source_failure_limit = kSourceFailures,
                             size_t target_failure_limit = kTargetFailures);
  Result Verify(const std::string& source, const std::string& target,
                const std::function<bool()>& verify);

 private:
  struct Failures {
    size_t count = 0;
    size_t pending = 0;
    Clock::time_point expires;
  };
  using Buckets = std::unordered_map<std::string, Failures>;
  void Prune(Buckets& buckets, Clock::time_point now);
  static void Record(Buckets& buckets, const std::string& key, size_t limit,
                     Clock::time_point now);

  Now now_;
  const size_t source_failure_limit_, target_failure_limit_;
  std::mutex mutex_;
  Buckets sources_, targets_;
  Clock::time_point next_cleanup_{};
};

// Admission before expensive work, including successful authentication,
// registration and password changes. Separate from credential failure budgets.
class CredentialWorkLimiter {
 public:
  explicit CredentialWorkLimiter(
      DeviceAuthLimiter::Now now = DeviceAuthLimiter::Clock::now)
      : now_(std::move(now)) {}
  int Admit(const std::string& source);

 private:
  struct Window {
    size_t count = 0;
    DeviceAuthLimiter::Clock::time_point expires{};
  };
  DeviceAuthLimiter::Now now_;
  std::mutex mutex_;
  Window global_;
  std::unordered_map<std::string, Window> sources_;
};

#endif
