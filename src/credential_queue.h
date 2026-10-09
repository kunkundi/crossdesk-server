#ifndef CROSSDESK_CREDENTIAL_QUEUE_H
#define CROSSDESK_CREDENTIAL_QUEUE_H

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

// Owned exclusively by the application worker. Waiting requests do not occupy
// a hashing worker. Round-robin sources prevent a NAT or reconnecting client
// from keeping unrelated sources behind its rate-limited requests.
class CredentialQueue {
 public:
  using Clock = std::chrono::steady_clock;
  using Now = std::function<Clock::time_point()>;
  struct Job {
    uint64_t id;
    std::string source;
    Clock::time_point deadline;
    std::function<bool()> alive;
    std::function<int()> admit;  // Consume a source token; or retry seconds.
    std::function<void(size_t)> start;  // Reserved, idle hashing worker index.
    std::function<void()> expire;
    Clock::time_point queued_at{};
  };
  struct Stats {
    uint64_t accepted = 0, started = 0, completed = 0, canceled = 0;
    uint64_t full = 0, source_full = 0, expired = 0, source_waits = 0;
    int64_t max_wait_ms = 0, max_execution_ms = 0;
  };

  CredentialQueue(size_t workers, size_t capacity, size_t source_capacity,
                  Now now = Clock::now)
      : capacity_(capacity), source_capacity_(source_capacity),
        now_(std::move(now)), running_(workers) {
    if (!workers || !capacity || !source_capacity)
      throw std::invalid_argument("Credential queue limits must be positive");
  }

  // Does not start work until Pump(), so callers can publish pending state.
  bool Enqueue(Job job) {
    if (ids_.size() >= capacity_) { ++stats_.full; return false; }
    if (ids_.count(job.id)) return false;
    if (job.source.empty()) job.source = "unknown";
    auto found = sources_.find(job.source);
    if (found != sources_.end() && found->second.outstanding >= source_capacity_) {
      ++stats_.source_full;
      return false;
    }
    job.queued_at = now_();
    ids_.insert(job.id);
    auto& source = sources_[job.source];
    ++source.outstanding;
    source.waiting.push_back(std::move(job));
    ++stats_.accepted;
    return true;
  }

  // Running hashes finish normally; their result must still check liveness.
  void Cancel(uint64_t id) {
    for (auto it = sources_.begin(); it != sources_.end(); ++it) {
      auto& source = it->second;
      auto job = std::find_if(source.waiting.begin(), source.waiting.end(),
                              [id](const Job& value) { return value.id == id; });
      if (job == source.waiting.end()) continue;
      source.waiting.erase(job);
      ids_.erase(id);
      ++stats_.canceled;
      if (--source.outstanding == 0) sources_.erase(it);
      return;
    }
  }

  void Complete(size_t worker) {
    auto& active = running_.at(worker);
    if (!active) throw std::logic_error("Credential worker completed twice");
    stats_.max_execution_ms = std::max(stats_.max_execution_ms,
                                      Millis(now_() - active->started_at));
    ids_.erase(active->id);
    auto source = sources_.find(active->source);
    if (--source->second.outstanding == 0) sources_.erase(source);
    active.reset();
    ++stats_.completed;
  }

  void Pump() {
    if (pumping_) return;
    pumping_ = true;
    struct Reset { bool& flag; ~Reset() { flag = false; } } reset{pumping_};
    const auto now = now_();
    std::vector<std::function<void()>> expired;
    // Also prune when every worker is busy, bounding waiting credentials and
    // preventing disconnected/expired requests from retaining capacity.
    for (auto it = sources_.begin(); it != sources_.end();) {
      auto& source = it->second;
      for (auto job = source.waiting.begin(); job != source.waiting.end();) {
        const bool alive = job->alive();
        if (alive && now < job->deadline) { ++job; continue; }
        if (alive) {
          ++stats_.expired;
          expired.push_back(std::move(job->expire));
        } else {
          ++stats_.canceled;
        }
        ids_.erase(job->id);
        --source.outstanding;
        job = source.waiting.erase(job);
      }
      if (source.outstanding == 0) it = sources_.erase(it);
      else ++it;
    }
    // Callbacks may enqueue new work. No container references survive them.
    for (auto& callback : expired) callback();
    for (size_t worker = 0; worker < running_.size(); ++worker) {
      if (running_[worker]) continue;
      auto source = sources_.upper_bound(last_source_);
      const auto count = sources_.size();
      for (size_t attempt = 0; attempt < count; ++attempt) {
        if (source == sources_.end()) source = sources_.begin();
        auto& queue = source->second;
        if (queue.waiting.empty() || now < queue.not_before) { ++source; continue; }
        const auto delay = queue.waiting.front().admit();
        if (delay > 0) {
          queue.not_before = now + std::chrono::seconds(delay);
          ++stats_.source_waits;
          ++source;
          continue;
        }
        auto job = std::move(queue.waiting.front());
        queue.waiting.pop_front();
        last_source_ = source->first;
        running_[worker] = Running{job.id, job.source, now};
        stats_.max_wait_ms = std::max(stats_.max_wait_ms, Millis(now - job.queued_at));
        ++stats_.started;
        job.start(worker);
        break;
      }
    }
  }

  size_t Active() const {
    return std::count_if(running_.begin(), running_.end(),
                         [](const auto& slot) { return slot.has_value(); });
  }
  size_t Waiting() const { return ids_.size() - Active(); }
  size_t Capacity() const { return capacity_; }
  const Stats& GetStats() const { return stats_; }

 private:
  static int64_t Millis(Clock::duration value) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(value).count();
  }
  struct Source {
    std::deque<Job> waiting;
    size_t outstanding = 0;
    Clock::time_point not_before{};
  };
  struct Running {
    uint64_t id;
    std::string source;
    Clock::time_point started_at;
  };
  const size_t capacity_, source_capacity_;
  Now now_;
  std::map<std::string, Source> sources_;
  std::unordered_set<uint64_t> ids_;
  std::vector<std::optional<Running>> running_;
  std::string last_source_;
  Stats stats_;
  bool pumping_ = false;
};

#endif
