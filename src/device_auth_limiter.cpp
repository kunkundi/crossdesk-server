#include "device_auth_limiter.h"

#include <algorithm>
#include <exception>
#include <stdexcept>
#include <utility>

namespace {
int RemainingSeconds(DeviceAuthLimiter::Clock::time_point deadline,
                     DeviceAuthLimiter::Clock::time_point now) {
  return deadline > now
      ? static_cast<int>(std::chrono::ceil<std::chrono::seconds>(deadline - now).count())
      : 0;
}
}  // namespace

DeviceAuthLimiter::DeviceAuthLimiter(Now now, size_t source_failure_limit,
                                     size_t target_failure_limit, Policy policy)
    : now_(std::move(now)),
      source_failure_limit_(source_failure_limit),
      target_failure_limit_(target_failure_limit),
      policy_(policy) {
  if (source_failure_limit == 0 || target_failure_limit == 0)
    throw std::invalid_argument("Authentication failure limits must be positive");
}

void DeviceAuthLimiter::Prune(Buckets& buckets, Clock::time_point now) {
  for (auto it = buckets.begin(); it != buckets.end();) {
    if (it->second.expires <= now && it->second.pending == 0)
      it = buckets.erase(it);
    else
      ++it;
  }
}

void DeviceAuthLimiter::ResetExpired(Failures& failures, Clock::time_point now) {
  if (failures.expires > now) return;
  failures.count = 0;
  failures.expires = now + kWindow;
  failures.blocked_until = {};
  failures.backoff = std::chrono::seconds::zero();
}

void DeviceAuthLimiter::Record(Failures& failures, size_t limit,
                               Clock::time_point now) {
  if (failures.count < limit) ++failures.count;
  if (failures.count < limit) return;
  const bool exponential = policy_ == Policy::Exponential;
  failures.backoff = exponential
      ? (failures.backoff.count() == 0 ? kInitialBackoff
                                       : std::min(failures.backoff * 2, kMaxBackoff))
      : kWindow;
  failures.blocked_until = now + failures.backoff;
  // Keep escalation beyond the cooldown so waiting does not grant a fresh
  // batch of guesses. Only an actual failed verification advances this state.
  failures.expires = failures.blocked_until +
      (exponential ? kBackoffRecovery : std::chrono::seconds::zero());
}

int DeviceAuthLimiter::CooldownRemaining(const std::string& source,
                                         const std::string& target) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto now = now_();
  auto remaining = [now](const Buckets& buckets, const std::string& key) {
    const auto it = buckets.find(key);
    return it == buckets.end() ? 0 : RemainingSeconds(it->second.blocked_until, now);
  };
  return std::max(remaining(sources_, source.empty() ? "unknown" : source),
                  remaining(targets_, target));
}

DeviceAuthLimiter::Result DeviceAuthLimiter::Verify(
    const std::string& source, const std::string& target,
    const std::function<bool()>& verify) {
  // Reserve both budgets atomically, then release the lock for slow hashing.
  std::unique_lock<std::mutex> lock(mutex_);
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
    auto& bucket = it->second;
    ResetExpired(bucket, now);
    if (const int delay = RemainingSeconds(bucket.blocked_until, now); delay > 0)
      return delay;
    // After cooldown, allow only one in-flight check per dimension. Parallel
    // requests must not turn the next probe into another batch of guesses.
    const size_t available = bucket.count < limit ? limit - bucket.count : 1;
    return bucket.pending >= available ? 1 : 0;
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
  for (auto entry : {std::make_pair(&sources_, source_key),
                     std::make_pair(&targets_, target)}) {
    auto& bucket =
        entry.first->try_emplace(entry.second, Failures{0, 0, now + kWindow})
            .first->second;
    ++bucket.pending;
  }
  lock.unlock();
  bool authenticated = false;
  std::exception_ptr error;
  try {
    authenticated = verify();
  } catch (...) {
    error = std::current_exception();
  }
  lock.lock();
  const auto finished = now_();
  auto finish = [&](Buckets& buckets, const std::string& key, size_t limit) {
    auto& bucket = buckets.at(key);
    --bucket.pending;
    ResetExpired(bucket, finished);
    if (!authenticated) Record(bucket, limit, finished);
    if (bucket.count == 0 && bucket.pending == 0) buckets.erase(key);
  };
  finish(sources_, source_key, source_failure_limit_);
  finish(targets_, target, target_failure_limit_);
  lock.unlock();
  if (error) std::rethrow_exception(error);
  return {authenticated, 0};
}

int CredentialWorkLimiter::Admit(const std::string& source) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto now = now_();
  constexpr auto window = std::chrono::minutes(1);
  if (now >= next_cleanup_) {
    for (auto it = sources_.begin(); it != sources_.end();) {
      if (it->second.expires <= now)
        it = sources_.erase(it);
      else
        ++it;
    }
    next_cleanup_ = now + window;
  }
  const auto key = source.empty() ? "unknown" : source;
  if (!sources_.count(key) && sources_.size() >= DeviceAuthLimiter::kMaxEntries)
    return 60;
  auto& local =
      sources_.try_emplace(key, Window{0, now + window}).first->second;
  if (local.expires <= now) local = {0, now + window};
  auto retry = [now](const Window& value, size_t limit) {
    return value.count < limit
               ? 0
               : static_cast<int>(std::chrono::ceil<std::chrono::seconds>(
                                      value.expires - now)
                                      .count());
  };
  const int delay = retry(local, source_limit_);
  if (delay > 0) return delay;
  ++local.count;
  return 0;
}
