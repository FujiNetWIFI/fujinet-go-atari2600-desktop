# The emulator core (Stella, staged) and the FujiNet Go host layer.
#
# stella_core: Stella's emucore + common + debugger (engine AND its GUI, see
# below) + gui, compiled from the staged tree with this project's flags, plus
# the host classes in stella/host/ that MediaFactory.hxx (the one overridden
# header) selects. No SDL, no libpng/zlib/sqlite/httplib: the feature defines
# that would pull those in are simply not set, and the corresponding source
# files compile to empty translation units or are left out.
#
# Why src/gui and src/debugger/gui are compiled even though nothing native
# ever shows them: every Cart*.hxx links its Cart*Widget under
# DEBUGGER_SUPPORT, and Debugger derives from DialogContainer. Leaving them
# out would mean patching ~60 cart headers and the debugger engine. Compiled
# in, the debugger dialog exists offscreen in our own pixel surfaces
# (FBSurfaceFNGO) and the engine -- DebuggerParser's 112 commands, CpuDebug,
# TIADebug, RiotDebug, CartDebug -- works unchanged. StellaHost is the only
# thing that touches it, on the Stella thread.

set(STELLA_SRC "${STELLA_GEN}/src")

# ---- source list ------------------------------------------------------
# Derived from the staged tree by directory, with an exclusion list, so an
# upstream file added to one of these modules is picked up on re-stage.
# CONFIGURE_DEPENDS keys the glob on the staging marker, which changes
# whenever the tree is regenerated.
function(stella_glob out)
  set(_files)
  foreach(_pat IN LISTS ARGN)
    file(GLOB _g CONFIGURE_DEPENDS "${STELLA_SRC}/${_pat}")
    list(APPEND _files ${_g})
  endforeach()
  set(${out} ${_files} PARENT_SCOPE)
endfunction()

stella_glob(STELLA_EMUCORE
  emucore/*.cxx emucore/tia/*.cxx emucore/tia/frame-manager/*.cxx
  emucore/elf/*.cxx emucore/fujinet/*.cxx)
stella_glob(STELLA_COMMON
  common/*.cxx common/audio/*.cxx common/tv_filters/*.cxx common/repository/*.cxx)
stella_glob(STELLA_DEBUGGER
  debugger/*.cxx debugger/gui/*.cxx)
stella_glob(STELLA_GUI gui/*.cxx)

set(STELLA_YACC
  "${STELLA_SRC}/debugger/yacc/YaccParser.cxx"
  "${STELLA_SRC}/debugger/yacc/stella.tab.cxx")

set(STELLA_EXCLUDE_NAMES
  # standalone-only / needs sqlite
  OSystemStandalone.cxx ProfilingRunner.cxx
  # SDL backends and the SDL main
  main.cxx EventHandlerSDL.cxx FBBackendSDL.cxx FBSurfaceSDL.cxx SoundSDL.cxx
  # image / zip support (compile to nothing without their defines, but they
  # would still need their bundled libs' headers)
  PNGLibrary.cxx JPGLibrary.cxx ZipHandler.cxx FSNodeZIP.cxx
  # test programs
  FujiBusTest.cxx EncodingTest.cxx LinkerTest.cxx calctest.cxx)

set(STELLA_SOURCES ${STELLA_EMUCORE} ${STELLA_COMMON} ${STELLA_DEBUGGER}
                   ${STELLA_GUI} ${STELLA_YACC})
foreach(_name IN LISTS STELLA_EXCLUDE_NAMES)
  list(FILTER STELLA_SOURCES EXCLUDE REGEX "/${_name}$")
endforeach()

if(WIN32)
  list(APPEND STELLA_SOURCES
    "${STELLA_SRC}/os/windows/FSNodeWINDOWS.cxx"
    "${STELLA_SRC}/os/windows/SerialPortWINDOWS.cxx")
  set(STELLA_OS_INCLUDE "${STELLA_SRC}/os/windows")
else()
  list(APPEND STELLA_SOURCES
    "${STELLA_SRC}/os/unix/FSNodePOSIX.cxx"
    "${STELLA_SRC}/os/unix/SerialPortPOSIX.cxx")
  set(STELLA_OS_INCLUDE "${STELLA_SRC}/os/unix")
endif()

set(FNGO_HOST_SOURCES
  stella/host/FNGOHostHooks.cxx
  stella/host/SoundFNGO.cxx
  stella/StellaHost.cxx)

add_library(stella_core STATIC ${STELLA_SOURCES} ${FNGO_HOST_SOURCES})

# The same feature set Stella's configure produces for a desktop build,
# minus SDL, images, zip, http, cheats and physical joysticks (the SDL
# gamepad layer is the session's own). VCS_CART_HOST_MAPPER is unconditional
# in Stella's configure: the vendored vcs_cart.h then leaves booted games'
# mappers to Stella's sixty-odd schemes.
target_compile_definitions(stella_core PUBLIC
  SOUND_SUPPORT DEBUGGER_SUPPORT GUI_SUPPORT WINDOWED_SUPPORT
  VCS_CART_HOST_MAPPER FUJINET_SUPPORT)
if(WIN32)
  target_compile_definitions(stella_core PUBLIC BSPF_WINDOWS)
elseif(APPLE)
  target_compile_definitions(stella_core PUBLIC BSPF_MACOS)
else()
  target_compile_definitions(stella_core PUBLIC BSPF_UNIX)
endif()

target_include_directories(stella_core PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/stella/host"
  "${CMAKE_CURRENT_SOURCE_DIR}/stella"
  "${STELLA_SRC}/emucore"
  "${STELLA_SRC}/emucore/tia"
  "${STELLA_SRC}/emucore/tia/frame-manager"
  "${STELLA_SRC}/emucore/elf"
  "${STELLA_SRC}/emucore/fujinet"
  "${STELLA_SRC}/common"
  "${STELLA_SRC}/common/audio"
  "${STELLA_SRC}/common/tv_filters"
  "${STELLA_SRC}/common/repository"
  "${STELLA_SRC}/gui"
  "${STELLA_SRC}/debugger"
  "${STELLA_SRC}/debugger/gui"
  "${STELLA_SRC}/debugger/yacc"
  "${STELLA_SRC}/lib"
  "${STELLA_OS_INCLUDE}")

# Stella's own warning set; upstream builds clean with it. Our host files
# get the same.
target_compile_options(stella_core PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wunused -Woverloaded-virtual -Wno-unused-parameter -Wno-ignored-qualifiers>)
set_target_properties(stella_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

find_package(Threads REQUIRED)
target_link_libraries(stella_core PUBLIC Threads::Threads)
if(WIN32)
  target_link_libraries(stella_core PUBLIC ws2_32 shell32 winmm)   # winmm: timeBeginPeriod
endif()


# ---- a2600_session: the frontend contract ----------------------------------
#
# A separate target from stella_core so the core stays SDL-free: audio_sdl.c
# and gamepad_sdl.c live here, and host_test links stella_core alone to keep
# that honest. Found-or-skipped, exactly as the frontends are: stella_core is
# SDL-free by construction, so `-DFRONTEND=none` on a machine (or a
# cross-toolchain) with no SDL3 must still configure and build the emulator
# and its tests.
find_package(SDL3 CONFIG QUIET)
if(NOT SDL3_FOUND)
  message(STATUS "a2600_session: SDL3 not found -- building the core and its "
                 "tests only (no audio, no gamepads, no frontends)")
  add_subdirectory(tests)
  return()
endif()

set(A2600_SESSION_C_SOURCES
  src/settings.c
  src/paths.c
  src/media.c
  src/fujinet_runtime.c
  src/audio_sdl.c
  src/bindings.c
  src/hid_keys.c)
set(A2600_SESSION_CXX_SOURCES
  src/session.cpp
  src/debug.cpp)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/gamepad_sdl.c")
  list(APPEND A2600_SESSION_C_SOURCES src/gamepad_sdl.c)
else()
  set(A2600_GAMEPAD_STUB ON)
endif()

add_library(a2600_session STATIC
  ${A2600_SESSION_C_SOURCES} ${A2600_SESSION_CXX_SOURCES})
set_target_properties(a2600_session PROPERTIES
  C_STANDARD 11
  POSITION_INDEPENDENT_CODE ON)
target_include_directories(a2600_session PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(a2600_session PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(a2600_session PUBLIC stella_core SDL3::SDL3)
if(A2600_GAMEPAD_STUB)
  target_compile_definitions(a2600_session PRIVATE A2600_GAMEPAD_STUB=1)
endif()
# setenv()/getaddrinfo() under a strict -std=c11
target_compile_definitions(a2600_session PRIVATE _GNU_SOURCE _DEFAULT_SOURCE)
set_source_files_properties(${A2600_SESSION_C_SOURCES} ${A2600_SESSION_CXX_SOURCES}
  PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra")

# Where an uninstalled build and an installed copy find the FujiNet runtime
# (core/src/paths.c probes these after the executable's own directory).
target_compile_definitions(a2600_session PRIVATE
  A2600_DEV_FUJINET_OUT="${CMAKE_SOURCE_DIR}/tools/fujinet/work/out"
  A2600_INSTALL_DATADIR="${CMAKE_INSTALL_FULL_DATADIR}/fujinet-go-atari2600"
  A2600_INSTALL_LIBDIR="${CMAKE_INSTALL_FULL_LIBDIR}/fujinet-go-atari2600")

add_subdirectory(tests)
