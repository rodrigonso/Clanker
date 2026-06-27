#include "base/sysinfo.h"

#if defined(_WIN32)
#include <windows.h>
// psapi.h must follow windows.h
#include <psapi.h>

u64 PeakRssBytes() {
  PROCESS_MEMORY_COUNTERS pmc = {};
  if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) return (u64)pmc.PeakWorkingSetSize;
  return 0;
}

#else
#include <sys/resource.h>

u64 PeakRssBytes() {
  struct rusage r;
  if (getrusage(RUSAGE_SELF, &r) != 0) return 0;
#if defined(__APPLE__)
  return (u64)r.ru_maxrss;  // bytes on macOS
#else
  return (u64)r.ru_maxrss * 1024;  // kilobytes on Linux
#endif
}
#endif
