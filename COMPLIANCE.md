# Compliance

Per-component provenance for `fujinet-go-atari2600-desktop`, written before
the first public build, in the family tradition (see
`fujinet-go-adam-desktop/COMPLIANCE.md`, `fujinet-go-coleco-desktop/COMPLIANCE.md`).

## What ships

| Component | Origin | Licence | How it enters the build |
|---|---|---|---|
| This application | this repository | GPL-3.0-or-later | — |
| **Stella** (the emulator: 6507, TIA, RIOT, every cartridge mapper, the debugger engine, DiStella) | [`tschak909/stella`](https://github.com/tschak909/stella), branch `add-fujinet-support` — a fork of [`stella-emu/stella`](https://github.com/stella-emu/stella) | GPL-2.0-or-later, © 1995-2026 Bradford W. Mott, Stephen Anthony and the Stella Team | pinned in `cmake/Dependencies.cmake`, its `src/` tree staged into `core/stella-generated/` by `cmake/StageStella.cmake` and compiled as the `stella_core` static library |
| **The FujiNet cartridge in Stella** (`CartFUJI`, `FujiNetLink`, the `fujinet/` protocol sources and the CONFIG client ROM `FujiConfigROM.hxx`) | same checkout; the protocol sources are the firmware's own `pico/atari-2600` files, vendored into Stella by its `sync.sh` | as marked in each file (the FujiNet project's, © Thomas Cherryhomes); the fork as a whole is distributed under Stella's GPL-2.0-or-later | part of `stella_core` |
| **FujiNet firmware** (`libfujinet`) | [`FujiNetWIFI/fujinet-firmware`](https://github.com/FujiNetWIFI/fujinet-firmware), PC target `RS232` | GPL-3.0-or-later | built as a shared library by `tools/fujinet/build-fujinet-desktop.sh`, `dlopen`'d at run time |
| nlohmann/json (`src/lib/json`) | bundled by Stella | MIT | header-only, used by Stella's `KeyMap` |
| dr_libs (`src/lib/dr_libs`) | bundled by Stella | public domain / MIT-0 (dual) | header-only, used by Stella's Supercharger cartridge loader |
| SDL3 | libsdl-org | Zlib | system package on Linux; linked statically on macOS and Windows |
| mbedTLS 3.6.x | Mbed-TLS | Apache-2.0 | for `libfujinet`: system package where it is a usable 3.x, otherwise the pinned source |

GPL-2.0-or-later (Stella) and GPL-3.0-or-later (this application, the
firmware) are compatible: the combined work is distributed under GPL-3.0.

## What is changed in Stella, and what is not

Stella is compiled from its own sources with this project's flags; it is
**not patched**. Four files are overridden in the staged tree, all kept in
`core/stella/host/` and copied over the staged file by `cmake/StageStella.cmake`:

- `src/common/MediaFactory.hxx` — the header that selects which
  `OSystem`/`FrameBufferBackend`/`Sound`/`EventHandler` implementation a
  build gets. Ours selects the FujiNet Go host classes (`OSystemFNGO`,
  `FBBackendFNGO`, `SoundFNGO`, `EventHandlerFNGO`), exactly as Stella's own
  libretro port plugs itself in.
- `src/os/windows/FSNodeWINDOWS.hxx` — three lines: the file streams are
  opened from a `std::filesystem::path` rather than a `std::wstring`. The
  `std::wstring` overloads are an MSVC extension; libstdc++ (MSYS2 UCRT64
  and mingw-w64, which build this application's Windows release) only has
  the standard `path` overload. Worth sending upstream.
- `src/emucore/fujinet/FujiNetLink.cxx` — one line: the wait for a
  non-blocking connect also selects on the exception set, which is where
  Winsock (unlike POSIX) reports a refused connection. Without it, every
  connect attempt on Windows with no FujiNet listening waited out the 3 s
  timeout on the emulation thread. Worth sending upstream.
- `src/common/Variant.hxx` — on Apple platforms only, the float and double
  string parses use `strtof_l`/`strtod_l` in the "C" locale instead of
  `std::from_chars`, which Apple's libc++ marks unavailable before macOS 26
  (the Xcode 16 SDK has no floating-point overload at all). Upstream Stella
  builds its own macOS binaries with a macOS 26 deployment target for this
  reason; this app's bundles run on macOS 13.3 and later.

A diff of `core/stella-generated/src` against the pinned checkout shows
exactly those four files. Stella's SDL frontend, its image/zip/http/sqlite
libraries and its cheat code support are staged but never compiled
(`SDL_SUPPORT`, `IMAGE_SUPPORT`, `ZIP_SUPPORT`, `HTTP_LIB_SUPPORT`,
`CHEATCODE_SUPPORT` are not defined), so the core library links against none
of them.

## System ROMs — there are none

The Atari 2600 has **no BIOS**: the console boots straight into the
cartridge. The only built-in image is FujiNet's own CONFIG client
(`FujiConfigROM.hxx`, 16 KB), which is FujiNet-project code with the
firmware's licence, so there is nothing copyrighted-by-a-third-party to
redistribute or to leave out, and unlike the ADAM, ColecoVision, Astrocade
and MSX ports this one needs no ROM import step and no `no_embedded_roms`
test. Game cartridges are the user's own: opened from a local file, or
served by FujiNet from the SD folder or a network host.

## Deliberately not used

- **Stella's own frontend** (`src/os/*` except the `FSNode` classes,
  `src/common/*SDL*`) — the four native frontends here replace it. Its
  in-tree GUI (`src/gui`, `src/debugger/gui`) is compiled because the
  debugger engine is built on it, but it renders offscreen into this
  project's pixel surfaces and is never shown.
- **z26, javatari, MAME's a2600 driver** — not consulted; nothing taken.

## Icon and name

The icon is the FujiNet Go family mark (the same artwork as the other
desktops) on the Atari 2600 accent `#ffa645`, with the mark inverted so
that the outer discs and lines are black and the centre disc white
(`tools/icons/make-icons.py`). "Atari" and "Atari 2600" are trademarks of
Atari Interactive, Inc.; they are used here to name the machine being
emulated, and the project is not affiliated with or endorsed by Atari.

## Trademarks and names

"FujiNet" is the FujiNet project's name. "Stella" is the Stella Team's.
Neither project endorses this application; both are credited in the About
box of every frontend.
