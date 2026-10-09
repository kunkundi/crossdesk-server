#include "credential_queue.h"
#include "device_auth_limiter.h"

#include <iostream>
#include <string>
#include <vector>

int main() {
  int failures = 0;
  auto expect = [&](bool value, const char* message) {
    if (!value) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
  };
  using Clock = CredentialQueue::Clock;
  auto time = Clock::time_point{};
  auto now = [&] { return time; };
  CredentialWorkLimiter limiter(now);
  for (int i = 0; i < 2000; ++i)
    expect(limiter.Admit("source-" + std::to_string(i)) == 0,
           "2000 independent clients have no shared minute quota");
  for (int i = 0; i < 60; ++i)
    expect(limiter.Admit("nat") == 0, "source permits its own budget");
  expect(limiter.Admit("nat") == 60, "source throttle remains enforced");
  time += std::chrono::seconds(61);
  expect(limiter.Admit("nat") == 0, "expired source budget resets");

  int admissions = 0, expirations = 0;
  std::vector<uint64_t> started;
  auto make_job = [&](uint64_t id, std::string source, bool* alive = nullptr) {
    return CredentialQueue::Job{
        id, std::move(source), time + std::chrono::seconds(120),
        [alive] { return !alive || *alive; },
        [&] { ++admissions; return 0; },
        [&, id](size_t) { started.push_back(id); },
        [&] { ++expirations; }};
  };
  {
    CredentialQueue queue(2, 4, 2, now);
    expect(queue.Enqueue(make_job(1, "a")), "first job queues");
    expect(queue.Enqueue(make_job(2, "a")), "second source job queues");
    expect(!queue.Enqueue(make_job(3, "a")), "per-source outstanding cap");
    expect(queue.Enqueue(make_job(3, "b")), "unrelated source queues");
    expect(queue.Enqueue(make_job(4, "c")), "global capacity includes waiting");
    expect(!queue.Enqueue(make_job(5, "d")), "bounded global capacity");
    expect(admissions == 0, "queued and rejected work consumes no source tokens");
    queue.Pump();
    expect(started == std::vector<uint64_t>({1, 3}), "workers fairly select sources");
    expect(queue.Active() == 2 && queue.Waiting() == 2, "concurrency stays bounded");
    queue.Cancel(4);
    queue.Complete(0);
    queue.Pump();
    expect(started.back() == 2 && admissions == 3, "canceled work never consumes quota");
    queue.Complete(0); queue.Complete(1);
    expect(queue.Waiting() == 0 && queue.Active() == 0, "completion releases all capacity");
  }
  {
    CredentialQueue queue(1, 4, 4, now);
    auto delayed = make_job(10, "a");
    const auto ready = time + std::chrono::seconds(60);
    delayed.admit = [&] { return time < ready ? 60 : 0; };
    queue.Enqueue(std::move(delayed)); queue.Enqueue(make_job(11, "b"));
    queue.Pump();
    expect(started.back() == 11, "rate-limited source does not block another source");
    queue.Complete(0); queue.Pump();
    expect(queue.Active() == 0 && queue.Waiting() == 1, "source retry waits server-side");
    time = ready; queue.Pump();
    expect(started.back() == 10, "legacy request resumes without a client retry");
    queue.Complete(0);
  }
  {
    CredentialQueue queue(1, 4, 4, now);
    bool alive = true;
    queue.Enqueue(make_job(20, "a")); queue.Enqueue(make_job(21, "b", &alive));
    queue.Enqueue(make_job(22, "c")); queue.Pump();
    const auto before = admissions;
    alive = false; time += std::chrono::seconds(121); queue.Pump();
    expect(queue.Active() == 1 && queue.Waiting() == 0,
           "expiry and disconnect are pruned even when workers are full");
    expect(expirations == 1 && admissions == before,
           "only live expired work receives failure, without consuming quota");
    queue.Complete(0);
  }
  {
    CredentialQueue queue(2, 4096, 256, now);
    const auto before = started.size();
    for (int i = 0; i < 2000; ++i)
      expect(queue.Enqueue(make_job(100 + i, std::to_string(i))),
             "2000-client restart burst is accepted");
    queue.Pump();
    for (int i = 0; i < 1000; ++i) {
      expect(queue.Active() == 2, "burst never exceeds hashing concurrency");
      time += std::chrono::milliseconds(10);
      queue.Complete(0); queue.Complete(1); queue.Pump();
    }
    expect(started.size() - before == 2000 && queue.Waiting() == 0,
           "all legacy requests complete without reconnecting");
    expect(queue.GetStats().full == 0 && queue.GetStats().expired == 0,
           "restart burst does not reject or expire healthy work");
  }
  {
    CredentialQueue queue(1, 1, 1, now);
    auto job = make_job(30, "before");
    job.expire = [&] {
      expect(queue.Enqueue(make_job(30, "after")),
             "expiry callback may enqueue next request on the same connection");
      queue.Pump();
    };
    queue.Enqueue(std::move(job));
    time += std::chrono::seconds(121);
    queue.Pump();
    expect(queue.Active() == 1 && started.back() == 30,
           "reentrant expiry safely starts new request");
    queue.Cancel(30);
    expect(queue.Active() == 1 && !queue.Enqueue(make_job(30, "after")),
           "cancel does not release a running worker or duplicate its connection");
    queue.Complete(0);
    expect(queue.Enqueue(make_job(30, "after")), "completion releases connection id");
    queue.Cancel(30);
  }
  {
    DeviceAuthLimiter auth(now);
    int verifies = 0;
    for (int i = 0; i < 5; ++i)
      auth.Verify("attacker", "device", [&] { ++verifies; return false; });
    auto result = auth.Verify("another-source", "device", [&] { ++verifies; return true; });
    expect(!result.authenticated && result.retry_after == 900 && verifies == 5,
           "device failure cooldown remains effective across source changes");
  }
  return failures ? 1 : 0;
}
