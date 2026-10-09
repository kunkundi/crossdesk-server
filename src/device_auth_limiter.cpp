#include "device_auth_limiter.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

DeviceAuthLimiter::DeviceAuthLimiter(Now now, size_t source_failure_limit,
                                     size_t target_failure_limit)
    : now_(std::move(now)),
      source_failure_limit_(source_failure_limit),
      target_failure_limit_(target_failure_limit) {
  if (source_failure_limit == 0 || target_failure_limit == 0)
    throw std::invalid_argument("Authentication failure limits must be positive");
}

void DeviceAuthLimiter::Prune(Buckets& buckets, Clock::time_point now) {
  for (auto it = buckets.begin(); it != buckets.end();) {
    if (it->second.expires <= now)
      it = buckets.erase(it);
    else
      ++it;
  }
}

void DeviceAuthLimiter::Record(Buckets& buckets, const std::string& key,
                               size_t limit, Clock::time_point now) {
  auto result = buckets.try_emplace(key, Failures{0, now + kWindow});
  auto& failures = result.first->second;
  // A full cooldown starts at the threshold; blocked requests do not extend it.
  if (++failures.count == limit) failures.expires = now + kWindow;
}

DeviceAuthLimiter::Result DeviceAuthLimiter::Verify(
    const std::string& source, const std::string& target,
    const std::function<bool()>& verify) {
  // Keep check, verification and accounting atomic across concurrent callers.
  std::lock_guard<std::mutex> lock(mutex_);
  const auto now = now_();
  if (now >= next_cleanup_) {
    Prune(sources_, now);
    Prune(targets_, now);
    next_cleanup_ = now + std::chrono::minutes(1);
  }
  // Missing source metadata must not become a per-connection bypass.
  const std::string source_key = source.empty() ? "unknown" : source;
  auto retry = [now](Buckets& buckets, const std::string& key, size_t limit) {
    const auto it = buckets.find(key);
    if (it == buckets.end()) return 0;
    if (it->second.expires <= now) {
      buckets.erase(it);
      return 0;
    }
    if (it->second.count < limit) return 0;
    return static_cast<int>(
        std::chrono::ceil<std::chrono::seconds>(it->second.expires - now)
            .count());
  };
  const int source_retry = retry(sources_, source_key, source_failure_limit_);
  const int target_retry = retry(targets_, target, target_failure_limit_);
  const int retry_after = std::max(source_retry, target_retry);
  if (retry_after > 0) return {false, retry_after};

  // Do not evict live failures when flooded with new IDs/IPs. Deny new keys
  // until expiry frees capacity, bounding memory without resetting lockouts.
  if ((!sources_.count(source_key) && sources_.size() >= kMaxEntries) ||
      (!targets_.count(target) && targets_.size() >= kMaxEntries)) {
    return {false, static_cast<int>(kWindow.count())};
  }
  if (verify()) return {true, 0};

  // Success never clears shared failure history: an attacker may know a valid
  // password for another device. Unknown devices accrue identical failures.
  Record(sources_, source_key, source_failure_limit_, now);
  Record(targets_, target, target_failure_limit_, now);
  return {false, 0};
}
