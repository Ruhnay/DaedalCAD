#pragma once

// How hard DaedalCAD works the computer: its share of all CPU cores since the last sample, and the
// memory it holds. For the readout in the status bar.

struct Usage {
  double cpu = 0;        // percent of the whole machine (all cores), like Task Manager
  double memoryMb = 0;   // what the program holds in RAM now (working set)
  double peakMb = 0;     // the most it held so far
  double totalMb = 0;    // the computer's RAM
  int cores = 1;
  bool valid = false;
};

class UsageMeter {
 public:
  // The first call only starts the clock (cpu 0); each later one measures since the one before.
  Usage sample();

 private:
  double lastCpuSeconds = -1;
  double lastWallSeconds = 0;
  double peak = 0;
};
