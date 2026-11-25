/*
 * @Author: DI JUNKUN
 * @Date: 2026-11-25
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _SESSION_ROUTES_H_
#define _SESSION_ROUTES_H_

#include <map>
#include <mutex>
#include <string>
#include <utility>

class SessionRoutes {
 public:
  void Begin(const std::string& transmission, const std::string& guest,
             const std::string& host, const std::string& generation) {
    std::lock_guard<std::mutex> lock(mutex_);
    sessions_[{transmission, guest}] = {host, generation, {}, {}};
  }

  std::string Generation(const std::string& transmission,
                         const std::string& guest) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find({transmission, guest});
    return it == sessions_.end() ? std::string{} : it->second.generation;
  }

  void End(const std::string& transmission, const std::string& guest) {
    std::lock_guard<std::mutex> lock(mutex_);
    sessions_.erase({transmission, guest});
  }

  void Report(const std::string& transmission, const std::string& reporter,
              const std::string& peer, const std::string& generation,
              const std::string& route) {
    if (generation.size() != 32 || (route != "direct" && route != "relay")) return;
    std::lock_guard<std::mutex> lock(mutex_);
    auto as_host = sessions_.find({transmission, peer});
    if (as_host != sessions_.end() && as_host->second.host == reporter &&
        as_host->second.generation == generation) {
      as_host->second.host_route = route;
      return;
    }
    auto as_guest = sessions_.find({transmission, reporter});
    if (as_guest != sessions_.end() && as_guest->second.host == peer &&
        as_guest->second.generation == generation) {
      as_guest->second.guest_route = route;
    }
  }

  std::string Get(const std::string& transmission, const std::string& guest,
                  const std::string& generation) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find({transmission, guest});
    if (it == sessions_.end() || it->second.generation != generation) {
      return "unknown";
    }
    const auto& session = it->second;
    if (!session.host_route.empty()) return session.host_route;
    return session.guest_route.empty() ? "unknown" : session.guest_route;
  }

 private:
  struct Session {
    std::string host, generation, host_route, guest_route;
  };
  mutable std::mutex mutex_;
  std::map<std::pair<std::string, std::string>, Session> sessions_;
};

#endif
