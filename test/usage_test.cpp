// The usage readout: CPU while busy and idle, memory that follows what the program holds.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
#include <vector>
#include "usage.h"

int main() {
  int failures = 0;
  auto expect = [&](bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "  ok  " : "  FAIL", what);
    if (!ok) ++failures;
  };
  UsageMeter meter;
  const Usage first = meter.sample();
  expect(first.valid && first.memoryMb > 1 && first.totalMb > first.memoryMb, "memory read");
  // One busy core for half a second: a share of the whole machine, about 100 / cores %.
  const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
  volatile double x = 0;
  while (std::chrono::steady_clock::now() < until) x += std::sqrt(x + 1.0);
  const Usage busy = meter.sample();
  std::printf("  busy: %.1f %% of %d cores\n", busy.cpu, busy.cores);
  expect(busy.cpu > 50.0 / busy.cores && busy.cpu < 100.0 / busy.cores + 15, "one busy core shows as its share");
  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  const Usage idle = meter.sample();
  std::printf("  idle: %.1f %%\n", idle.cpu);
  expect(idle.cpu < 10, "idle shows near zero");
  // 300 MB held (and touched) shows in the memory.
  std::vector<char> block(300u * 1024 * 1024, 1);
  const Usage held = meter.sample();
  std::printf("  memory %.0f MB -> %.0f MB\n", idle.memoryMb, held.memoryMb);
  expect(held.memoryMb > idle.memoryMb + 250 && held.peakMb >= held.memoryMb, "held memory shows");
  std::printf(failures ? "%d failure(s)\n" : "all usage checks passed\n", failures);
  return failures ? 1 : 0;
}
