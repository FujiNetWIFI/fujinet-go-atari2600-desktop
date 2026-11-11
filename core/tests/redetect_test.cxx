/*
 * redetect_test -- a running cartridge survives Stella's frame-layout
 * redetect.
 *
 * Console::redetectFrameLayout() saves the machine, runs the TIA for sixty
 * frames and loads it back. It runs after every FujiNet boot (and on a forced
 * change to Auto). When that load read zeros, the CPU restarted at PC 0 with
 * RAM cleared: Combat, whose BRK vector points into its kernel, came up as a
 * garbled screen at 19 fps.
 *
 * The image is built here (no ROM ships with the tests): reset marks $80 with
 * $A5 and then counts frames in $81; the BRK vector marks $82 with $EE.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <thread>

#include "Console.hxx"
#include "OSystem.hxx"
#include "System.hxx"
#include "TIA.hxx"

#include "StellaHost.hxx"

namespace {
  int failures = 0;

  void check(bool ok, const std::string& what)
  {
    std::printf("%s: %s\n", ok ? "ok" : "FAIL", what.c_str());
    if(!ok) ++failures;
  }

  bool waitFrames(StellaHost& host, int count, int timeoutMs)
  {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    const uInt64 start = host.frameCount();
    while(std::chrono::steady_clock::now() < deadline)
    {
      if(host.frameCount() >= start + static_cast<uInt64>(count)) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
  }

  // A 2K image at $F000: 259-line frames, three of them VSYNC.
  constexpr std::array<uInt8, 48> code = {
    0x78,             // F000 SEI
    0xD8,             // F001 CLD
    0xA2, 0xFF,       // F002 LDX #$FF
    0x9A,             // F004 TXS
    0xA9, 0xA5,       // F005 LDA #$A5
    0x85, 0x80,       // F007 STA $80   reset ran
    0xA9, 0x00,       // F009 LDA #0
    0x85, 0x81,       // F00B STA $81
    0x85, 0x82,       // F00D STA $82
    0xE6, 0x81,       // F00F INC $81   frame:
    0xA9, 0x02,       // F011 LDA #2
    0x85, 0x00,       // F013 STA VSYNC
    0x85, 0x02,       // F015 STA WSYNC
    0x85, 0x02,       // F017 STA WSYNC
    0x85, 0x02,       // F019 STA WSYNC
    0xA9, 0x00,       // F01B LDA #0
    0x85, 0x00,       // F01D STA VSYNC
    0xA2, 0x00,       // F01F LDX #0
    0x85, 0x02,       // F021 STA WSYNC   256 lines
    0xCA,             // F023 DEX
    0xD0, 0xFB,       // F024 BNE $F021
    0x4C, 0x0F, 0xF0, // F026 JMP $F00F
    0xA9, 0xEE,       // F029 LDA #$EE  BRK/NMI:
    0x85, 0x82,       // F02B STA $82
    0x4C, 0x2D, 0xF0, // F02D JMP $F02D
  };

  bool writeImage(const std::filesystem::path& path)
  {
    std::array<uInt8, 2048> rom{};
    std::copy(code.begin(), code.end(), rom.begin());
    rom[0x7FA] = 0x29; rom[0x7FB] = 0xF0;   // NMI
    rom[0x7FC] = 0x00; rom[0x7FD] = 0xF0;   // RESET
    rom[0x7FE] = 0x29; rom[0x7FF] = 0xF0;   // BRK
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(rom.data()), rom.size());
    return static_cast<bool>(out);
  }

  struct Snapshot { int mark, frames, brk; uInt32 lines; };

  Snapshot snapshot(StellaHost& host)
  {
    return host.withStella([](OSystem& os) {
      System& sys = os.console().system();
      return Snapshot{ sys.peekOob(0x80), sys.peekOob(0x81), sys.peekOob(0x82),
                       os.console().tia().scanlinesLastFrame() };
    });
  }
}

int main()
{
  const auto tmp = std::filesystem::temp_directory_path() /
    ("a2600-redetect-test-" + std::to_string(std::random_device{}()));
  std::filesystem::create_directories(tmp);
  const auto rom = tmp / "frames.a26";
  check(writeImage(rom), "wrote the image");

  StellaHost host;
  StellaHost::Config cfg;
  cfg.baseDir = tmp.string() + "/";
  cfg.options["fujinet"] = false;
  cfg.startRom = rom.string();

  std::string err;
  check(host.start(cfg, err), "host starts the image");
  if(!err.empty()) { std::printf("error: %s\n", err.c_str()); return 1; }
  check(waitFrames(host, 30, 5000), "30 frames arrive");

  Snapshot before = snapshot(host);
  check(before.mark == 0xA5 && before.brk != 0xEE, "the image is running before the redetect");

  // What Console::cartridgeSwapped() does after a FujiNet boot
  host.withStella([](OSystem& os) { os.console().setFormat(0, true); return 0; });
  check(waitFrames(host, 10, 5000), "frames flow after the redetect");

  const Snapshot after = snapshot(host);
  char what[160];
  std::snprintf(what, sizeof what, "RAM survived the redetect ($80 = $%02X, want $A5)", after.mark);
  check(after.mark == 0xA5, what);
  check(after.brk != 0xEE, "the CPU never went through the BRK vector");
  std::snprintf(what, sizeof what, "frames are the image's 259 lines (%u)", after.lines);
  check(after.lines == 259, what);

  waitFrames(host, 5, 5000);
  const Snapshot later = snapshot(host);
  std::snprintf(what, sizeof what, "the frame loop still runs ($81 %d -> %d)", after.frames, later.frames);
  check(later.frames != after.frames, what);

  host.stop();
  std::error_code ec;
  std::filesystem::remove_all(tmp, ec);
  std::printf("%d failure(s)\n", failures);
  return failures ? 1 : 0;
}
