/*
 * vsync_jobs_test -- a job posted while the core is phase-locked to a
 * frontend's frame clock must run at once, not after pace()'s 2-frame
 * timeout. The GNOME frontend ticks on the GTK thread and also calls
 * withStella() from it (its 1 s status label): the tick cannot arrive while
 * that call blocks, so a job that waits for the tick costs ~50 ms and the
 * game hitches once a second.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <random>
#include <thread>
#include <vector>

#include "StellaHost.hxx"

using Clock = std::chrono::steady_clock;

int main()
{
  const auto tmp = std::filesystem::temp_directory_path() /
    ("a2600-vsyncjobs-test-" + std::to_string(std::random_device{}()));
  std::filesystem::create_directories(tmp);

  StellaHost host;
  StellaHost::Config cfg;
  cfg.baseDir = tmp.string() + "/";
  cfg.options["fujinet"] = true;
  cfg.options["fujinet.host"] = "127.0.0.1";
  cfg.options["fujinet.port"] = static_cast<Int32>(11504);
  std::string err;
  if(!host.start(cfg, err)) { std::printf("FAIL start: %s\n", err.c_str()); return 1; }

  // The GTK thread: tick at 60 Hz; every 10th tick make a synchronous call.
  const auto period = std::chrono::nanoseconds(16'666'667);
  auto next = Clock::now();
  std::vector<double> lat;
  for(int i = 0; i < 20; ++i)   // let the lock engage
  {
    host.notifyVsync(0);
    next += period;
    std::this_thread::sleep_until(next);
  }
  const uInt64 frames0 = host.frameCount();
  const int ticks = 600;
  for(int i = 0; i < ticks; ++i)
  {
    host.notifyVsync(0);
    if(i % 10 == 5)
    {
      // Mid-frame: the emulator has finished its frame and sits in pace().
      std::this_thread::sleep_for(std::chrono::milliseconds(8));
      const auto t0 = Clock::now();
      host.withStella([](OSystem&) { return 0; });
      const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
      lat.push_back(ms);
    }
    next += period;
    std::this_thread::sleep_until(next);
  }
  const uInt64 frames = host.frameCount() - frames0;
  host.stop();

  // The median, not the worst: a loaded CI runner can delay any one call by
  // tens of ms, but the old code delayed EVERY call by the tick timeout.
  std::sort(lat.begin(), lat.end());
  const double median = lat[lat.size() / 2];
  std::printf("median withStella latency %.1f ms (worst %.1f), %llu frames for %d ticks\n",
              median, lat.back(), static_cast<unsigned long long>(frames), ticks);
  int bad = 0;
  if(median > 25.0) { std::printf("FAIL: job waited for the tick timeout\n"); ++bad; }
  if(frames < ticks * 0.9 || frames > ticks * 1.1) { std::printf("FAIL: frames not locked to ticks\n"); ++bad; }
  if(!bad) std::printf("PASS\n");
  return bad ? 1 : 0;
}
