cmake_minimum_required(VERSION 3.20)

# Must be set before project() to take effect. Left unset, Clang defaults the
# bundle's minimum macOS version to whatever SDK the CI runner's Xcode ships
# -- e.g. the macos-latest runner's SDK can be newer than a real tester's OS,
# which produced "You can't use this version of the application ... with this
# version of macOS" on an M1 Mac for the sibling MSX port. 11.0 is the oldest
# macOS that shipped on Apple Silicon.
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin" AND NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")
endif()

# The version lives here and nowhere else: the About boxes, the macOS bundle
# and the release workflow all take it from PROJECT_VERSION, and cutting a tag
# checks that the tag agrees with it (see "Cutting a release" in the README).
project(fujinet-go-atari2600-desktop VERSION 0.1.0 LANGUAGES C CXX)

add_compile_definitions(A2600_VERSION_STRING="${PROJECT_VERSION}")

# The session layer and every frontend are C11; the emulator core is Stella,
# which is C++23 (it uses #elifdef, std::span, deducing this and friends), so
# the whole tree needs a C++23 compiler: GCC 13+, Clang 16+, MSYS2 UCRT64's
# GCC, or Xcode 15+.
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Which native frontend(s) to build. Each is found-or-skipped so a lean
# machine can still build the others (or just the core + tests).
set(FRONTEND "all" CACHE STRING
    "Frontend to build: gnome, kde, macos, windows, all, none")

# Embed the FujiNet web UI (WebKitGTK on GNOME, QtWebEngine on KDE) instead of
# opening it in the system browser. Never a hard dependency: when the engine
# is missing the frontend falls back to the browser.
option(WITH_WEBVIEW "Embed the FujiNet web UI (WebKitGTK / QtWebEngine)" ON)

# There is no ROM option, unlike every sibling: the Atari 2600 has no BIOS,
# and the only built-in image is FujiNet's own CONFIG client, which Stella
# embeds (src/emucore/fujinet/FujiConfigROM.hxx). See COMPLIANCE.md.

# Included here, at the top of the tree, so every subdirectory sees the
# *variables* it defines: GNUInstallDirs caches CMAKE_INSTALL_DATADIR as an
# empty string and only sets its non-cache fallback (share) in the including
# scope. Including it in a subdirectory (as core/ would) therefore leaves the
# frontends with an empty DATADIR -- and "${CMAKE_INSTALL_DATADIR}/applications"
# then resolves to the absolute path /applications, outside the prefix.
include(GNUInstallDirs)

include(cmake/Dependencies.cmake)
include(cmake/StageStella.cmake)

enable_testing()
add_subdirectory(core)

# The FujiNet runtime: built from the pinned firmware sources as part of the
# normal build and installed alongside the app (the session provisions the
# user's runtime tree from the installed copy on first start). Set up before
# the frontends so the macOS bundle can depend on the library it packs.
include(cmake/FujiNetRuntime.cmake)

# Each frontend is native to one platform; with FRONTEND=all (the default)
# only the host's viable frontends are considered, so a Windows/macOS
# configure never descends into the Linux (pkg-config) frontends and vice
# versa. Naming a frontend explicitly still forces just that one.
#
# add_frontend also skips a directory that has no CMakeLists.txt yet, which is
# how a milestone that has not landed stays out of the way instead of breaking
# the configure for everyone. Naming it explicitly is an error, so nobody can
# ask for a frontend and silently get nothing.
function(add_frontend name)
  if(NOT EXISTS "${CMAKE_SOURCE_DIR}/frontends/${name}/CMakeLists.txt")
    if(FRONTEND STREQUAL name)
      message(FATAL_ERROR "The ${name} frontend is not implemented yet.")
    endif()
    message(STATUS "${name} frontend: not implemented yet -- skipping")
    return()
  endif()
  add_subdirectory("frontends/${name}")
endfunction()

if(FRONTEND STREQUAL "gnome" OR (FRONTEND STREQUAL "all" AND UNIX AND NOT APPLE))
  add_frontend(gnome)
endif()
if(FRONTEND STREQUAL "kde" OR (FRONTEND STREQUAL "all" AND UNIX AND NOT APPLE))
  add_frontend(kde)
endif()
if(FRONTEND STREQUAL "macos" OR (FRONTEND STREQUAL "all" AND APPLE))
  add_frontend(macos)
endif()
if(FRONTEND STREQUAL "windows" OR (FRONTEND STREQUAL "all" AND WIN32))
  add_frontend(windows)
endif()

# Application icons (rendered from source art by tools/icons/make-icons.py).
# Desktop environments look the icon up by the desktop-entry id, so the same
# art is installed under each frontend's id -- at every hicolor size, which is
# what makes it show up crisply everywhere from the 16px window list to the
# 256px app grid.
#
# Only for frontends actually built here: a package should not ship files for
# an app it does not contain.
set(A2600_ICON_SIZES 16 24 32 48 64 128 192 256 512)
set(A2600_ICON_TARGETS_AND_IDS
  fujinet-go-atari2600-gnome online.fujinet.go.atari2600.gnome
  fujinet-go-atari2600-kde online.fujinet.go.atari2600.kde)
list(LENGTH A2600_ICON_TARGETS_AND_IDS _a2600_icon_n)
math(EXPR _a2600_icon_last "${_a2600_icon_n} - 1")
foreach(_i RANGE 0 ${_a2600_icon_last} 2)
  list(GET A2600_ICON_TARGETS_AND_IDS ${_i} _target)
  math(EXPR _j "${_i} + 1")
  list(GET A2600_ICON_TARGETS_AND_IDS ${_j} _id)
  if(TARGET ${_target})
    foreach(_size IN LISTS A2600_ICON_SIZES)
      install(FILES data/icons/hicolor/${_size}x${_size}/apps/fujinet-go-atari2600.png
              DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/${_size}x${_size}/apps
              RENAME ${_id}.png)
    endforeach()
  endif()
endforeach()

# ---- packaging --------------------------------------------------------
#
# One package per frontend, not one package for both: GNOME (GTK4/libadwaita)
# and KDE (Qt6) pull in entirely different runtime library stacks, and a user
# who wants one should not be made to depend on the other.
if(FRONTEND STREQUAL "gnome")
  set(CPACK_PACKAGE_NAME "fujinet-go-atari2600-gnome")
elseif(FRONTEND STREQUAL "kde")
  set(CPACK_PACKAGE_NAME "fujinet-go-atari2600-kde")
else()
  set(CPACK_PACKAGE_NAME "fujinet-go-atari2600")
endif()

set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_VENDOR "FujiNet")
set(CPACK_PACKAGE_CONTACT "https://github.com/FujiNetWIFI/fujinet-go-atari2600-desktop/issues")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://fujinet.online/")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "Self-contained Atari 2600 with built-in FujiNet")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE")
set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
set(CPACK_STRIP_FILES ON)

if(UNIX AND NOT APPLE)
  set(CPACK_GENERATOR "DEB;RPM;TGZ")

  # Auto-detected from the actual linked libraries (dpkg-shlibdeps /
  # rpmbuild's own dependency generator) rather than hand-listed: GTK4 vs Qt6
  # alone makes one fixed list wrong for whichever frontend was not built, and
  # a hand-list silently goes stale the moment a dependency's package name
  # changes across distributions.
  set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
  set(CPACK_DEBIAN_PACKAGE_SECTION "games")
  set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "${CPACK_PACKAGE_HOMEPAGE_URL}")

  set(CPACK_RPM_PACKAGE_AUTOREQPROV ON)
  set(CPACK_RPM_PACKAGE_LICENSE "GPL-3.0-or-later")
  set(CPACK_RPM_PACKAGE_GROUP "Amusements/Games")
  set(CPACK_RPM_PACKAGE_URL "${CPACK_PACKAGE_HOMEPAGE_URL}")
endif()

include(CPack)
