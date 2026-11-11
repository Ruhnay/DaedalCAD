#include "usage.h"

#include <algorithm>
#include <chrono>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#else
#include <unistd.h>
#include <cstdio>
#include <cstring>
#endif

namespace {

double wallSeconds() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

#ifdef _WIN32
double fileTimeSeconds(const FILETIME& t) {
  ULARGE_INTEGER v;
  v.LowPart = t.dwLowDateTime;
  v.HighPart = t.dwHighDateTime;
  return static_cast<double>(v.QuadPart) * 1e-7;  // 100 ns ticks
}
#endif

// Seconds of CPU the process has used (all threads), and its memory in MB.
bool readProcess(double* cpuSeconds, double* memoryMb, double* peakMb, double* totalMb) {
#ifdef _WIN32
  FILETIME created, exited, kernel, user;
  if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return false;
  *cpuSeconds = fileTimeSeconds(kernel) + fileTimeSeconds(user);
  PROCESS_MEMORY_COUNTERS counters{};
  counters.cb = sizeof(counters);
  // K32GetProcessMemoryInfo lives in kernel32 itself: no extra library to link.
  if (K32GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
    *memoryMb = counters.WorkingSetSize / (1024.0 * 1024.0);
    *peakMb = counters.PeakWorkingSetSize / (1024.0 * 1024.0);
  }
  MEMORYSTATUSEX status{};
  status.dwLength = sizeof(status);
  if (GlobalMemoryStatusEx(&status)) *totalMb = status.ullTotalPhys / (1024.0 * 1024.0);
  return true;
#elif defined(__linux__)
  FILE* stat = std::fopen("/proc/self/stat", "r");
  if (!stat) return false;
  char buffer[1024] = {};
  const size_t got = std::fread(buffer, 1, sizeof(buffer) - 1, stat);
  std::fclose(stat);
  buffer[got] = 0;
  // Fields after the ")" that ends the program name: utime and stime are the 12th and 13th.
  const char* p = std::strrchr(buffer, ')');
  if (!p) return false;
  unsigned long utime = 0, stime = 0;
  if (std::sscanf(p + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &utime, &stime) != 2) return false;
  *cpuSeconds = static_cast<double>(utime + stime) / static_cast<double>(sysconf(_SC_CLK_TCK));
  if (FILE* status = std::fopen("/proc/self/status", "r")) {
    char line[256];
    while (std::fgets(line, sizeof(line), status)) {
      long kb = 0;
      if (std::sscanf(line, "VmRSS: %ld kB", &kb) == 1) *memoryMb = kb / 1024.0;
      if (std::sscanf(line, "VmHWM: %ld kB", &kb) == 1) *peakMb = kb / 1024.0;
    }
    std::fclose(status);
  }
  const long pages = sysconf(_SC_PHYS_PAGES), pageSize = sysconf(_SC_PAGE_SIZE);
  if (pages > 0 && pageSize > 0) *totalMb = static_cast<double>(pages) * pageSize / (1024.0 * 1024.0);
  return true;
#else
  (void)cpuSeconds;
  (void)memoryMb;
  (void)peakMb;
  (void)totalMb;
  return false;
#endif
}

}  // namespace

Usage UsageMeter::sample() {
  Usage u;
  u.cores = std::max(1u, std::thread::hardware_concurrency());
  double cpuSeconds = 0;
  if (!readProcess(&cpuSeconds, &u.memoryMb, &u.peakMb, &u.totalMb)) return u;
  const double now = wallSeconds();
  if (lastCpuSeconds >= 0 && now > lastWallSeconds) {
    u.cpu = std::clamp((cpuSeconds - lastCpuSeconds) / ((now - lastWallSeconds) * u.cores) * 100.0, 0.0, 100.0);
  }
  lastCpuSeconds = cpuSeconds;
  lastWallSeconds = now;
  peak = std::max({peak, u.peakMb, u.memoryMb});
  u.peakMb = peak;
  u.valid = true;
  return u;
}
