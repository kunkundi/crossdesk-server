#include "resource_monitor.h"
#ifndef _WIN32
#include <sys/resource.h>
#endif
#ifdef __linux__
#include <dirent.h>
#elif defined(__APPLE__)
#include <libproc.h>
#include <unistd.h>

#include <vector>
#endif

FileDescriptorUsage ReadFileDescriptorUsage() {
  FileDescriptorUsage usage;
#ifndef _WIN32
  rlimit limit{};
  if (getrlimit(RLIMIT_NOFILE, &limit) == 0) {
    usage.soft_limit = limit.rlim_cur == RLIM_INFINITY ? -1 : limit.rlim_cur;
  }
#endif
#ifdef __linux__
  if (auto* directory = opendir("/proc/self/fd")) {
    int64_t count = 0;
    while (auto* entry = readdir(directory))
      if (entry->d_name[0] != '.') ++count;
    closedir(directory);
    usage.open = count - 1;  // Exclude the sampler's directory descriptor.
  }
#elif defined(__APPLE__)
  int size = proc_pidinfo(getpid(), PROC_PIDLISTFDS, 0, nullptr, 0);
  if (size > 0) {
    std::vector<proc_fdinfo> entries(size / sizeof(proc_fdinfo) + 64);
    int bytes = proc_pidinfo(getpid(), PROC_PIDLISTFDS, 0, entries.data(),
                             entries.size() * sizeof(proc_fdinfo));
    if (bytes > 0) usage.open = bytes / sizeof(proc_fdinfo);
  }
#endif
  return usage;
}
