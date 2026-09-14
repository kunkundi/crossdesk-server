/*
 * @Author: DI JUNKUN
 * @Date: 2026-09-14
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _RESOURCE_MONITOR_H_
#define _RESOURCE_MONITOR_H_

#include <cstdint>

struct FileDescriptorUsage {
  int64_t open = -1;
  int64_t soft_limit = -1;
};

FileDescriptorUsage ReadFileDescriptorUsage();

#endif
