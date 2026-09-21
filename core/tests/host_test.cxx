/*
 * host_test -- the Stella host layer, end to end, with no session and no
 * SDL: boot FujiNet's CONFIG client, watch frames flow, stop in the
 * debugger, step, resume, then replace the console and do it again.
 *
 * This is the M1 risk retirer: it proves Stella runs with the FNGO
 * backends, that the offscreen debugger dialog Stella insists on building
 * does not stop the engine from working, and that console replacement
 * (what "reboot to CONFIG" and "open cartridge" do) survives the FujiNet
 * cartridge's worker thread.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <chrono>
#include <random>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "Console.hxx"
#include "CpuDebug.hxx"
#include "Debugger.hxx"
#include "OSystem.hxx"

#include "StellaHost.hxx"

namespace {
  int failures = 0;

  void check(bool ok, const char* what)
  {
    std::printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if(!ok) ++failures;
  }

  bool waitFrames(StellaHost& host, uInt64& serial, int count, int timeoutMs)
  {
    std::vector<uInt32> px; StellaHost::FrameInfo info;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    int got = 0;
    while(std::chrono::steady_clock::now() < deadline)
    {
      if(host.copyFrame(px, info, &serial))
      {
        if(++got >= count) return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
  }

  bool frameNonBlankNow(StellaHost& host)
  {
    std::vector<uInt32> px; StellaHost::FrameInfo info; uInt64 s = 0;
    if(!host.copyFrame(px, info, &s)) return false;
    std::printf("frame %ux%u serial %llu rate %d\n", info.width, info.height,
                static_cast<unsigned long long>(info.serial), info.refreshRate);
    if(info.width != 160 || info.height < 100 || info.height > 320) return false;
    uInt32 distinct = 0; uInt32 last = px[0];
    for(uInt32 p : px) if(p != last) { ++distinct; last = p; }
    return distinct > 50;
  }

  // The CONFIG client paints within its first few dozen frames on an idle
  // machine; on a loaded CI runner those frames can take seconds, so wait
  // for the picture rather than asserting it at a fixed frame count.
  bool frameNonBlank(StellaHost& host, int timeoutMs = 5000)
  {
    const auto start = std::chrono::steady_clock::now();
    for(;;)
    {
      if(frameNonBlankNow(host)) return true;
      if(std::chrono::steady_clock::now() - start > std::chrono::milliseconds(timeoutMs))
        return false;
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }
}

int main()
{
  const auto tmp = std::filesystem::temp_directory_path() /
    ("a2600-host-test-" + std::to_string(std::random_device{}()));
  std::filesystem::create_directories(tmp);

  StellaHost host;
  StellaHost::Config cfg;
  cfg.baseDir = tmp.string() + "/";
  cfg.options["fujinet"] = true;          // the cart runs link-down: nothing listens
  cfg.options["fujinet.host"] = "127.0.0.1";
  cfg.options["fujinet.port"] = static_cast<Int32>(11504);

  int stops = 0, replaced = 0;
  StellaHost::Callbacks cb;
  cb.onStopped = [&](StellaHost::StopReason r, const std::string& m, int a) {
    ++stops;
    std::printf("stopped: reason %d msg '%s' addr %d\n", static_cast<int>(r), m.c_str(), a);
  };
  cb.onConsoleReplaced = [&] { ++replaced; };
  host.setCallbacks(cb);

  std::string err;
  check(host.start(cfg, err), "host starts and boots the CONFIG client");
  if(!err.empty()) { std::printf("error: %s\n", err.c_str()); return 1; }

  uInt64 serial = 0;
  check(waitFrames(host, serial, 60, 5000), "60 frames arrive within 5 s");
  check(frameNonBlank(host), "the CONFIG client painted something (frame is not blank)");

  // Pacing: 60 frames should take about a second at 60 Hz. The lower bound
  // is the one that matters (an unthrottled core free-runs and every other
  // check still passes); the upper bound is only asserted where the OS
  // itself can sleep for a frame without gross overshoot -- a loaded CI
  // virtual machine (the macOS runners especially) can turn a 16 ms sleep
  // into 50, and that says nothing about the pacing code.
  {
    const auto s0 = std::chrono::steady_clock::now();
    for(int i = 0; i < 10; ++i)
      std::this_thread::sleep_for(std::chrono::microseconds(16'667));
    const double tenSleeps = std::chrono::duration<double>(std::chrono::steady_clock::now() - s0).count();
    const bool accurateSleeps = tenSleeps < 0.25;   // 167 ms nominal
    std::printf("ten 16.7 ms sleeps took %.3f s (%s)\n", tenSleeps,
                accurateSleeps ? "accurate" : "coarse: upper pacing bound not asserted");

    uInt64 s2 = serial;
    const auto t0 = std::chrono::steady_clock::now();
    waitFrames(host, s2, 60, 10000);
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("60 frames in %.3f s\n", secs);
    check(secs > 0.8, "the machine is throttled (60 frames take at least 0.8 s)");
    check(!accurateSleeps || secs < 1.5, "the machine paces at roughly its refresh rate");
  }

  // Stop in the debugger.
  host.dbgBreak();
  check(host.isStopped(), "dbgBreak stops the machine in DEBUGGER state");
  check(stops == 1, "the stop callback fired once");

  const int pcBefore = host.withStella([](OSystem& os) { return os.debugger().cpuDebug().pc(); });
  std::string out = host.dbgRun("step");
  std::printf("step -> '%s'\n", out.c_str());
  const int pcAfter = host.withStella([](OSystem& os) { return os.debugger().cpuDebug().pc(); });
  std::printf("pc %04x -> %04x\n", pcBefore, pcAfter);
  check(pcAfter != pcBefore, "'step' advances the program counter");

  out = host.dbgRun("frame");
  check(host.isStopped(), "'frame' leaves the machine stopped");
  out = host.dbgRun("tia");
  check(out.find("VSYNC") != std::string::npos || out.size() > 40, "'tia' prints TIA state");
  out = host.dbgRun("listBreaks");
  std::printf("listBreaks -> '%s'\n", out.c_str());
  out = host.dbgRun("break");
  std::printf("break -> '%s'\n", out.c_str());
  out = host.dbgRun("listBreaks");
  check(out.find("breakpoint") != std::string::npos || out.find("Breakpoint") != std::string::npos
        || out.find("$") != std::string::npos, "'break' sets a breakpoint at the current PC");
  out = host.dbgRun("clearBreaks");

  host.dbgContinue();
  check(!host.isStopped(), "'run' resumes emulation");
  {
    uInt64 s3 = serial;
    check(waitFrames(host, s3, 30, 5000), "frames flow again after resume");
  }

  // Replace the console: what reboot-to-CONFIG and open-cartridge do.
  err = host.rebootToConfig();
  check(err.empty(), "reboot to CONFIG creates a fresh console");
  check(replaced >= 2, "the console-replaced callback fired for boot and reboot");
  {
    uInt64 s4 = 0;
    check(waitFrames(host, s4, 30, 5000), "the new console produces frames");
    check(frameNonBlank(host), "the new console painted the CONFIG client");
  }

  // Break again on the new console (proves the recreated debugger works).
  host.dbgBreak();
  check(host.isStopped(), "the recreated debugger stops the new console");
  out = host.dbgRun("step");
  host.dbgContinue();
  check(!host.isStopped(), "and resumes it");

  host.stop();
  check(!host.running(), "host stops cleanly");

  std::error_code ec;
  std::filesystem::remove_all(tmp, ec);
  std::printf("%d failure(s)\n", failures);
  return failures ? 1 : 0;
}
