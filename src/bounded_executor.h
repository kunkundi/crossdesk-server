/*
 * @Author: DI JUNKUN
 * @Date: 2026-09-14
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _BOUNDED_EXECUTOR_H_
#define _BOUNDED_EXECUTOR_H_

#include <condition_variable>
#include <deque>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>

// A single FIFO preserves login/logout and database write order. Reserved slots
// are for lifecycle work only; callers must bound the number of live owners.
class BoundedExecutor {
 public:
  BoundedExecutor(size_t capacity, size_t reserve,
                  std::function<void(std::exception_ptr)> on_error)
      : capacity_(capacity),
        reserve_(reserve),
        on_error_(std::move(on_error)),
        thread_([this] { Run(); }) {}
  ~BoundedExecutor() { Stop(); }
  BoundedExecutor(const BoundedExecutor&) = delete;
  BoundedExecutor& operator=(const BoundedExecutor&) = delete;

  bool Submit(std::function<void()> job, bool lifecycle = false) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_ || jobs_.size() >= capacity_ + (lifecycle ? reserve_ : 0))
      return false;
    jobs_.push_back(std::move(job));
    cv_.notify_one();
    return true;
  }
  void Stop() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopping_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
  }

 private:
  void Run() {
    for (;;) {
      std::function<void()> job;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
        if (jobs_.empty()) return;
        job = std::move(jobs_.front());
        jobs_.pop_front();
      }
      try {
        job();
      } catch (...) {
        on_error_(std::current_exception());
      }
    }
  }
  const size_t capacity_, reserve_;
  std::function<void(std::exception_ptr)> on_error_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<std::function<void()>> jobs_;
  bool stopping_ = false;
  std::thread thread_;
};

#endif
