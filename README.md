# FujiNet Go — Atari 2600

A self-contained Atari 2600 with a built-in [FujiNet](https://fujinet.online/):
boot the FujiNet CONFIG client, browse a network host from the console, boot
cartridge images over the network, and let the booted game keep talking to
the network — all in one desktop app. A member of the FujiNet Go desktop
family (`fujinet-go-adam-desktop`, `-apple2-`, `-coco-`, `-msx-`, `-intv-`,
`-astrocade-`, `-coleco-`).

| | |
|---|---|
| **Emulator** | [Stella](https://github.com/tschak909/stella) (branch `add-fujinet-support`), GPL-2.0-or-later, compiled as a library with its SDL frontend left out and its debugger engine kept. The FujiNet cartridge (`CartFUJI`) speaks the same mailbox protocol as the RP2040 cartridge, from the firmware's own vendored sources. |
| **FujiNet** | The firmware's `RS232` PC target, built in-process as `libfujinet` and dialled by the cartridge over loopback (BoIP on 11504, web admin on 11505). |
| **Frontends** | GNOME (GTK4/libadwaita), KDE (Qt6 Widgets), macOS (AppKit), Windows (Win32/GDI) — each with the display, a floating keypad window for both keyboard controllers, Preferences, a live FujiNet console log, and a debugger exposing every one of Stella's debugger commands. |
| **Packaging** | Per-frontend DEB/RPM/TGZ, two Flatpaks, a Windows zip and NSIS installer, and macOS bundles for **arm64 and x86_64**, all through GitHub Actions. |

## What it is

- **Stella, whole.** Every cartridge mapper, the cycle-exact TIA, the
  6507/RIOT, Stella's palettes and TV effects, and its ROM database — the
  emulator is Stella's own sources staged at a pinned commit and compiled
  here with three small overrides (see `COMPLIANCE.md`). No emulation code is
  reimplemented.
- **Stella's debugger, in every frontend.** The Prompt tab runs the
  DebuggerParser itself — all of `help`, `break`, `breakIf`, `trap`,
  `trapWrite`, `watch`, `timer`, `saveStateIf`, `logTrace`, `define`,
  `function`, `code`/`data`/`gfx`/`pgfx`/`row` directives, `saveDis`,
  `saveRom` … with Tab completion — beside native tabs for CPU & RAM
  (editable), Disassembly (DiStella listing per bank, click to toggle a
  breakpoint, follow PC), TIA (every register, the collision matrix, audio,
  and the picture being drawn), I/O (RIOT, timer, switches), Breaks & Traps,
  and States & Cart (save/load slots, save-to-file through the native
  picker, cartridge and link status). Stepping: Step, Trace, Scan+1,
  Frame+1, Rewind, Unwind, Run To.
- **Controllers you choose.** Preferences sets each port to Auto (Stella's
  ROM database and controller detector), Joystick, Paddles, Driving or
  Keypad, applied live — a game booted through FujiNet survives the change.
  Three switches decide whether a gamepad's analog sticks may drive a
  joystick, a paddle, or a driving controller.
- **A floating keypad window** for both keyboard controllers (`1 2 3 / 4 5 6
  / 7 8 9 / * 0 #`), each over its port's fire buttons, with the console
  switches (Select, Reset, Color/B&W, both difficulty switches, Reboot to
  CONFIG) and Map mode for rebinding any control to a key or gamepad
  button. The window is a fixed-size floating panel in every toolkit, so a
  tiling compositor never stretches the keys. Buttons are *held*, not
  pulsed: the machine samples them once a frame.
- **Gamepads that come and go.** SDL3 hotplug: a pad plugged in while a game
  runs is assigned to the next free port within a second and shown in
  Preferences; unplugging releases it. Pads with no gamepad mapping fall
  back to raw buttons and hats.
- **Open Cartridge…** runs a local `.a26`/`.bin`/`.rom`/`.fuji` directly
  (Stella autodetects the mapper); **Import Cartridge to SD…** copies one
  into FujiNet's SD root, where the CONFIG client lists it. Dropping a file
  on the window does the first. **Escape** reboots to CONFIG — on hardware a
  FujiNet-booted game cannot be undone with the RESET switch, so this is the
  power cycle.

The icon is the family mark inverted (black discs and lines, white centre)
on `#ffa645`, and the same orange marks held keys, the Map target, the
current line in the disassembly and the Run button while stopped.

## Building

```sh
cmake -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Both dependencies (Stella and fujinet-firmware) are cloned at their pinned
commits by the configure step — a plain `git clone` with no
`--recurse-submodules` is enough. To develop against working checkouts:

```sh
cmake -B build -DSTELLA_SRC=~/Workspace/stella \
               -DFUJINET_SRC=~/Workspace/fujinet-firmware
```

A dirty Stella checkout re-stages automatically; `-DSTELLA_RESTAGE=ON`
forces it. `-DFRONTEND=none` builds just the core and its tests;
`-DWITH_FUJINET=OFF` skips the firmware build (the machine boots the CONFIG
client reporting the link down).

Stella is C++23: GCC 13+, Clang 16+, or MSYS2 UCRT64's GCC. The macOS
bundles need macOS 13.3 or later (the first libc++ with floating-point
`std::to_chars`, which Stella's `std::format` calls use).

### Cross-building Windows on Linux

```sh
curl -LO https://github.com/libsdl-org/SDL/releases/download/release-3.4.12/SDL3-devel-3.4.12-mingw.tar.gz
tar xzf SDL3-devel-3.4.12-mingw.tar.gz
cmake -B build-win -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/mingw-w64.cmake \
      -DFRONTEND=windows -DWITH_FUJINET=OFF \
      -DCMAKE_PREFIX_PATH="$PWD/SDL3-3.4.12/x86_64-w64-mingw32"
cmake --build build-win
cp SDL3-3.4.12/x86_64-w64-mingw32/bin/SDL3.dll build-win/frontends/windows/
WINEPATH=/usr/x86_64-w64-mingw32/bin wine build-win/frontends/windows/fujinet-go-atari2600-windows.exe
```

This is the desk-side check; the release build is native MSYS2 (see below).

## Ports

FujiNet's BoIP listener is on **11504** and its web admin UI on **11505** —
high ports of this app's own, continuing the family's table (Astrocade
11500/11501, ColecoVision 11502/11503), so a standalone `fujinet-pc` or a
sibling app never collides. Stella's own default of 9995 is deliberately not
used: that is what the pico MAME dev harness and a standalone
`fujinet-pc-rs232` already occupy.

FujiNet listens and the cartridge dials in, so the session starts FujiNet
first and waits for its listener before creating the console. The listener's
backlog is one, so every console replacement (Open, Eject, Reboot to CONFIG,
a network boot) disconnects the old cartridge before the new one dials.

## Keys

| | |
|---|---|
| Left joystick / fire | arrows / Space (Stella's defaults; every control is remappable in the keypad window) |
| Right joystick / fire | Y G H J / F |
| Left keypad | `1 2 3 / Q W E / A S D / Z X C` — right: `8 9 0 / I O P / K L ; / , . /` |
| Paddles / driving | ← → (A, or the driving wheel) and ↑ ↓ (B) |
| Select / Reset / Color·B&W | F1 / F2 / F3 — the difficulty switches are on the Machine menu |
| Reboot to CONFIG | **Escape** (Ctrl+R on the menu) |
| Keypads / Fullscreen / Debugger | F9 / F11 / F12 (debugger: F5 run/stop, F7 step, F8 trace, Shift+F8 frame) |

## How this was verified

- **The whole stack on Linux** — the CONFIG client boots at 60 fps with
  FujiNet in-process and the cartridge reporting the link up, then again
  after a reboot to CONFIG; a breakpoint stops the machine in Stella's
  debugger offscreen, `step`/`frame`/`tia` run, and the console is replaced
  under a live debugger. `ctest` covers the host, the session, bindings,
  media routing, gamepads (pure functions plus an SDL start/stop with no
  hardware), the debugger contract, and the FujiNet link (`fujibus_smoke`).
- **Live desktops** — the GNOME and KDE frontends were run on a Wayland
  desktop with "FujiNet connected" in the header, the keypad floating and
  the debugger stopping and resuming the machine.
- **Windows** — cross-built with mingw-w64 and run under Wine with the
  keypad and debugger open; the core tests pass under Wine. That run found
  two real Windows bugs in Stella's fork (an MSVC-only stream constructor
  in `FSNodeWINDOWS.hxx`, and a connect wait that never noticed a refused
  connection on Winsock), both fixed as overrides in `core/stella/host/`.
- **macOS** — source-complete, compiled only by CI on both an Apple Silicon
  and an Intel runner, as every macOS frontend in the family was.

## Cutting a release

Pushing a `v*` tag builds every platform and, only if all of it passes,
publishes what it produced as a **draft** release:

| Asset | Contents |
|---|---|
| `fujinet-go-atari2600-gnome-<version>-Linux.{deb,rpm,tar.gz}` | the GNOME frontend, packaged with CPack |
| `fujinet-go-atari2600-kde-<version>-Linux.{deb,rpm,tar.gz}` | the KDE frontend, packaged with CPack |
| `FujiNet-Go-Atari2600-<version>-windows.zip` | the exe, `fujinet.dll`, and the `fujinet/` runtime tree |
| `FujiNet-Go-Atari2600-<version>-windows-setup.exe` | NSIS installer, per-user, no admin rights |
| `FujiNet-Go-Atari2600-<version>-macos-arm64.zip` | the `.app` bundle for Apple Silicon, FujiNet inside |
| `FujiNet-Go-Atari2600-<version>-macos-x86_64.zip` | the same for Intel Macs |
| `online.fujinet.go.atari2600.{gnome,kde}.flatpak` | single-file bundles: `flatpak install ./…flatpak` |

The version is declared in the tree (`project(… VERSION …)` and both
metainfo files, which template it), not derived from the tag; `check-version`
stops the release if they disagree. To release 0.2.0: set the version in
`CMakeLists.txt`, add a `<release>` entry with its date to both
`frontends/*/data/*.metainfo.xml.in`, commit, then
`git tag -a v0.2.0 && git push origin v0.2.0`.

The Windows release build is native MSYS2/UCRT64, not the cross-compile
above, and the `release.yml` job checks the exe's and `fujinet.dll`'s import
tables against a system-DLL whitelist. The macOS job runs once per
architecture, signs and notarises when the `MACOS_*` secrets are set, and
checks the bundle for leaked Homebrew dylibs.

## Licence

GPL-3.0-or-later. See `COMPLIANCE.md` for per-component provenance.
