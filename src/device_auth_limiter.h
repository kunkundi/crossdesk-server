/*
 * @Author: DI JUNKUN
 * @Date: 2026-10-09
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _DEVICE_AUTH_LIMITER_H_
#define _DEVICE_AUTH_LIMITER_H_

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// Two independent budgets with caller-supplied keys, never connection handles.
// Password signaling uses an authenticated initiator (or legacy source IP) and
// an initiator/credential pair. Admin authentication uses its separate instance.
class DeviceAuthLimiter {
 public:
  using Clock = std::chrono::steady_clock;
  using Now = std::function<Clock::time_point()>;
  static constexpr size_t kSourceFailures = 20;
  static constexpr size_t kTargetFailures = 5;
  static constexpr size_t kMaxEntries = 16384;  // Per dimension.
  static constexpr std::chrono::seconds kWindow{900};
  static constexpr std::chrono::seconds kInitialBackoff{60};
  static constexpr std::chrono::seconds kMaxBackoff{4 * 60 * 60};
  static constexpr std::chrono::seconds kBackoffRecovery{30 * 60};
  enum class Policy { Fixed, Exponential };

  struct Result {
    bool authenticated = false;
    int retry_after = 0;  // Positive only when verification was not attempted.
  };

  explicit DeviceAuthLimiter(Now now = Clock::now,
                             size_t source_failure_limit = kSourceFailures,
                             size_t target_failure_limit = kTargetFailures,
                             Policy policy = Policy::Fixed);
  // Inspect existing cooldowns without reserving or changing budgets. Verify
  // must still recheck after queueing, immediately before password computation.
  int CooldownRemaining(const std::string& source, const std::string& target);
  struct Entry {
    bool source;
    std::string target;
    size_t failures, threshold, pending;
    int retry_after, reset_after;
  };
  std::vector<Entry> Snapshot();
  void Clear(bool source, const std::string& target);
  void ClearMatching(const std::function<bool(bool, const std::string&)>& matches);
  Result Verify(const std::string& source, const std::string& target,
                const std::function<bool()>& verify);

 private:
  struct Failures {
    size_t count = 0;
    size_t pending = 0;
    Clock::time_point expires;
    Clock::time_point blocked_until{};
    std::chrono::seconds backoff{0};
    uint64_t generation = 0;
  };
  using Buckets = std::unordered_map<std::string, Failures>;
  void Prune(Buckets& buckets, Clock::time_point now);
  static void ResetExpired(Failures& failures, Clock::time_point now);
  void Record(Failures& failures, size_t limit, Clock::time_point now);

  Now now_;
  const size_t source_failure_limit_, target_failure_limit_;
  const Policy policy_;
  std::mutex mutex_;
  Buckets sources_, targets_;
  Clock::time_point next_cleanup_{};
};

// Admission before expensive work, including successful authentication,
// registration and password changes. Separate from credential failure budgets.
class CredentialWorkLimiter {
 public:
  explicit CredentialWorkLimiter(
      DeviceAuthLimiter::Now now = DeviceAuthLimiter::Clock::now,
      size_t source_limit = 60)
      : now_(std::move(now)), source_limit_(source_limit) {
    if (!source_limit_)
      throw std::invalid_argument("Credential source limit must be positive");
  }
  int Admit(const std::string& source);

 private:
  struct Window {
    size_t count = 0;
    DeviceAuthLimiter::Clock::time_point expires{};
  };
  DeviceAuthLimiter::Now now_;
  std::mutex mutex_;
  const size_t source_limit_;
  DeviceAuthLimiter::Clock::time_point next_cleanup_{};
  std::unordered_map<std::string, Window> sources_;
};

#endif
