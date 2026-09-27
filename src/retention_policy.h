/*
 * @Author: DI JUNKUN
 * @Date: 2026-09-28
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _RETENTION_POLICY_H_
#define _RETENTION_POLICY_H_

#include <cstdlib>
#include <stdexcept>
#include <string>

struct RetentionPolicy {
  int log_days = 185;
  int offline_days = 180;
  int interval_seconds = 3600;

  static RetentionPolicy FromEnvironment() {
    return {Read("CROSSDESK_LOG_RETENTION_DAYS", 185, 1, 3650),
            Read("CROSSDESK_OFFLINE_RETENTION_DAYS", 180, 1, 3650),
            Read("CROSSDESK_RETENTION_INTERVAL_SECONDS", 3600, 60, 86400)};
  }

 private:
  static int Read(const char* name, int fallback, int minimum, int maximum) {
    const char* raw = std::getenv(name);
    if (!raw || !*raw) return fallback;
    int value = 0;
    for (const char* p = raw; *p; ++p) {
      if (*p < '0' || *p > '9' || value > maximum / 10)
        throw std::invalid_argument(std::string(name) + " is out of range");
      value = value * 10 + (*p - '0');
      if (value > maximum)
        throw std::invalid_argument(std::string(name) + " is out of range");
    }
    if (value < minimum)
      throw std::invalid_argument(std::string(name) + " is out of range");
    return value;
  }
};

#endif