# Provide the Stella checkout (see cmake/Dependencies.cmake) and stage its
# src/ tree into core/stella-generated, which core/CMakeLists.txt compiles
# into the stella_core static library.
#
# Stella is built here the way every sibling app builds its emulator: the
# sources are copied and compiled as if they were ours, with our own flags,
# rather than driving Stella's configure + Makefile (which is SDL-centric and
# produces an executable, not a library). Nothing is patched. FOUR files are
# overridden. src/common/MediaFactory.hxx, the header that selects which
# OSystem/FBBackend/Sound/EventHandler implementation a build gets. Stella
# picks by BSPF_* / __LIB_RETRO__ / SDL_SUPPORT; ours (core/stella/host/
# MediaFactory.hxx) picks the FujiNet Go host classes instead, exactly as
# Stella's own libretro port plugs in. The override is a copy over the staged
# file, so the staged tree still builds with Stella's include layout and a
# diff against the pinned checkout shows exactly the overridden files. The
# others are src/os/windows/FSNodeWINDOWS.hxx, a three-line portability fix
# for libstdc++, src/emucore/fujinet/FujiNetLink.cxx, a one-line Winsock
# fix, and src/common/Variant.hxx, a float-parsing fallback for Apple's
# libc++ (see the STELLA_SHADOW_* variables below).
#
# What is NOT staged (see the exclude list): Stella's own frontends (src/os/
# libretro, the SDL backends stay but are never compiled), the Xcode project,
# and the bundled third-party trees this build does not link (libpng, zlib,
# sqlite, httplib, nanojpeg, tinyexif). src/lib/json and src/lib/dr_libs are
# header-only and used unguarded by the emulator core, so those two stay.
#
# Staging is automatic: it runs when the staged tree is missing, when the pin
# has moved, when this file or the shadow header changes, or on demand with
# -DSTELLA_RESTAGE=ON (which is also how to pick up uncommitted edits in a
# working checkout pointed at by STELLA_SRC).

set(STELLA_GEN "${CMAKE_SOURCE_DIR}/core/stella-generated")
set(STELLA_SHADOW_MEDIAFACTORY "${CMAKE_SOURCE_DIR}/core/stella/host/MediaFactory.hxx")
# The second override: src/os/windows/FSNodeWINDOWS.hxx opens its streams
# from a std::wstring, an MSVC-only overload; the shadow passes a
# std::filesystem::path, which libstdc++ (MSYS2/mingw-w64, the Windows
# release toolchain) and MSVC both take. Nothing else differs.
set(STELLA_SHADOW_FSNODEWINDOWS "${CMAKE_SOURCE_DIR}/core/stella/host/FSNodeWINDOWS.hxx")
# The third: src/emucore/fujinet/FujiNetLink.cxx waits for a non-blocking
# connect on the write set only; Winsock reports a refused connect on the
# exception set, so on Windows every attempt with no FujiNet listening
# waited out the 3 s timeout on the emulation thread. The shadow adds the
# exception set (a no-op on POSIX). Nothing else differs.
set(STELLA_SHADOW_FUJINETLINK "${CMAKE_SOURCE_DIR}/core/stella/host/FujiNetLink.cxx")
# The fourth: src/common/Variant.hxx parses floats with std::from_chars,
# which Apple's libc++ gates behind macOS 26 (the Xcode 16 SDK has no
# floating-point overload at all). On Apple the shadow parses with
# strtof_l/strtod_l in the "C" locale; other platforms are untouched.
set(STELLA_SHADOW_VARIANT "${CMAKE_SOURCE_DIR}/core/stella/host/Variant.hxx")

option(STELLA_RESTAGE "Re-stage the Stella sources from the checkout" OFF)

a2600_provide_dependency(
  NAME stella
  PATH third_party/stella
  URL "${STELLA_URL}"
  COMMIT "${STELLA_COMMIT}"
  SENTINEL src/emucore/OSystem.cxx
  OVERRIDE STELLA_SRC
  RESULT STELLA_DIR)

# The FujiNet cartridge is what makes this Stella THE Stella for this app:
# refuse a checkout without it rather than building a plain emulator that
# boots to a black screen with no explanation.
if(NOT EXISTS "${STELLA_DIR}/src/emucore/CartFUJI.cxx"
   OR NOT EXISTS "${STELLA_DIR}/src/emucore/fujinet/FujiConfigROM.hxx")
  message(FATAL_ERROR
    "${STELLA_DIR} has no src/emucore/CartFUJI.cxx or fujinet/FujiConfigROM.hxx "
    "-- the pin must be on the add-fujinet-support branch of "
    "${STELLA_URL}; see cmake/Dependencies.cmake.")
endif()

# What the staged tree was made from: the source identity plus hashes of the
# two inputs that transform it (this file, the shadow header), so that editing
# either re-stages rather than silently leaving the old result in place.
set(_stella_head "")
if(GIT_EXECUTABLE)
  execute_process(
    COMMAND ${GIT_EXECUTABLE} -C "${STELLA_DIR}" rev-parse HEAD
    OUTPUT_VARIABLE _stella_head OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
  execute_process(
    COMMAND ${GIT_EXECUTABLE} -C "${STELLA_DIR}" status --porcelain
    OUTPUT_VARIABLE _stella_dirty OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
  if(_stella_dirty)
    set(_stella_head "${_stella_head}-dirty")
  endif()
endif()
file(SHA256 "${CMAKE_CURRENT_LIST_FILE}" _stage_hash)
file(SHA256 "${STELLA_SHADOW_MEDIAFACTORY}" _shadow_hash)
file(SHA256 "${STELLA_SHADOW_FSNODEWINDOWS}" _shadow2_hash)
file(SHA256 "${STELLA_SHADOW_FUJINETLINK}" _shadow3_hash)
file(SHA256 "${STELLA_SHADOW_VARIANT}" _shadow4_hash)
set(_stella_want "${STELLA_DIR}\n${_stella_head}\n${_stage_hash}\n${_shadow_hash}\n${_shadow2_hash}\n${_shadow3_hash}\n${_shadow4_hash}\n")

set(_stella_have "")
if(EXISTS "${STELLA_GEN}/.source-info")
  file(READ "${STELLA_GEN}/.source-info" _stella_have)
endif()

if(STELLA_RESTAGE OR NOT _stella_have STREQUAL _stella_want
   OR NOT EXISTS "${STELLA_GEN}/src/emucore/OSystem.cxx")
  message(STATUS "stella: staging ${STELLA_DIR}/src -> ${STELLA_GEN}")
  file(REMOVE_RECURSE "${STELLA_GEN}")
  file(MAKE_DIRECTORY "${STELLA_GEN}")
  file(COPY "${STELLA_DIR}/src" DESTINATION "${STELLA_GEN}"
       PATTERN ".deps" EXCLUDE
       PATTERN "*.o" EXCLUDE
       PATTERN "*.d" EXCLUDE
       PATTERN "src/os/libretro" EXCLUDE
       PATTERN "src/os/macos/stella.xcodeproj" EXCLUDE
       PATTERN "src/lib/libpng" EXCLUDE
       PATTERN "src/lib/zlib" EXCLUDE
       PATTERN "src/lib/sqlite" EXCLUDE
       PATTERN "src/lib/httplib" EXCLUDE
       PATTERN "src/lib/nanojpeg" EXCLUDE
       PATTERN "src/lib/tinyexif" EXCLUDE)
  # The overrides (see the header comment).
  file(COPY_FILE "${STELLA_SHADOW_MEDIAFACTORY}"
       "${STELLA_GEN}/src/common/MediaFactory.hxx" ONLY_IF_DIFFERENT)
  file(COPY_FILE "${STELLA_SHADOW_FSNODEWINDOWS}"
       "${STELLA_GEN}/src/os/windows/FSNodeWINDOWS.hxx" ONLY_IF_DIFFERENT)
  file(COPY_FILE "${STELLA_SHADOW_FUJINETLINK}"
       "${STELLA_GEN}/src/emucore/fujinet/FujiNetLink.cxx" ONLY_IF_DIFFERENT)
  file(COPY_FILE "${STELLA_SHADOW_VARIANT}"
       "${STELLA_GEN}/src/common/Variant.hxx" ONLY_IF_DIFFERENT)
  file(WRITE "${STELLA_GEN}/.source-info" "${_stella_want}")
endif()

# Everything that comes out of the staged tree is regenerated from the pin; a
# stale CMake cache must not keep an old file list alive across a re-stage.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${STELLA_GEN}/.source-info")
