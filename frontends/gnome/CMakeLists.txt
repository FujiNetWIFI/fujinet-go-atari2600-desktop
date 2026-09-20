# The GNOME frontend: GTK4 + libadwaita.
#
# Found-or-skipped: a machine without the development packages still builds
# the core, the tests and the other frontends. Naming this frontend explicitly
# with -DFRONTEND=gnome turns the same situation into an error, so nobody asks
# for it and silently gets nothing.

find_package(PkgConfig QUIET)
if(NOT PkgConfig_FOUND)
  message(STATUS "gnome frontend: pkg-config not found -- skipping")
  return()
endif()

pkg_check_modules(GTK4 IMPORTED_TARGET gtk4>=4.10)
pkg_check_modules(ADW IMPORTED_TARGET libadwaita-1>=1.4)
if(NOT GTK4_FOUND OR NOT ADW_FOUND)
  message(STATUS "gnome frontend: gtk4/libadwaita not found -- skipping")
  return()
endif()

add_executable(fujinet-go-atari2600-gnome
  main.c
  window.c
  display.c
  prefs.c
  fujilog.c
  keypad/keypad_window.c
  debugger/dbg_window.c
)
target_link_libraries(fujinet-go-atari2600-gnome
  PRIVATE a2600_session PkgConfig::GTK4 PkgConfig::ADW)
target_compile_options(fujinet-go-atari2600-gnome PRIVATE -Wall -Wextra)
# Where the in-tree icon theme lives, for running uninstalled (see main.c).
target_compile_definitions(fujinet-go-atari2600-gnome PRIVATE
  A2600_SOURCE_ICON_DIR="${CMAKE_SOURCE_DIR}/data/icons")

# The FujiNet web UI, embedded when WebKitGTK is available and WITH_WEBVIEW
# is on; otherwise the configuration opens in the system browser. Never a
# hard dependency.
if(WITH_WEBVIEW)
  pkg_check_modules(WEBKIT IMPORTED_TARGET webkitgtk-6.0)
  if(WEBKIT_FOUND)
    target_link_libraries(fujinet-go-atari2600-gnome PRIVATE PkgConfig::WEBKIT)
    target_compile_definitions(fujinet-go-atari2600-gnome PRIVATE HAVE_WEBKIT=1)
    message(STATUS "gnome frontend: embedding the FujiNet web UI (WebKitGTK)")
  else()
    message(STATUS "gnome frontend: webkitgtk-6.0 not found -- the FujiNet web UI opens in the browser")
  endif()
endif()

install(TARGETS fujinet-go-atari2600-gnome RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# The desktop entry and AppStream metainfo are validated as ctests rather than
# only at package time: a malformed launcher is far cheaper to find at build
# time than after installing.
configure_file(data/online.fujinet.go.atari2600.gnome.desktop.in
               ${CMAKE_CURRENT_BINARY_DIR}/online.fujinet.go.atari2600.gnome.desktop
               @ONLY)
configure_file(data/online.fujinet.go.atari2600.gnome.metainfo.xml.in
               ${CMAKE_CURRENT_BINARY_DIR}/online.fujinet.go.atari2600.gnome.metainfo.xml
               @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/online.fujinet.go.atari2600.gnome.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/online.fujinet.go.atari2600.gnome.metainfo.xml
        DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)

find_program(DESKTOP_FILE_VALIDATE desktop-file-validate)
if(DESKTOP_FILE_VALIDATE)
  add_test(NAME desktop_file_gnome
           COMMAND ${DESKTOP_FILE_VALIDATE}
                   ${CMAKE_CURRENT_BINARY_DIR}/online.fujinet.go.atari2600.gnome.desktop)
endif()
find_program(APPSTREAMCLI appstreamcli)
if(APPSTREAMCLI)
  add_test(NAME metainfo_gnome
           COMMAND ${APPSTREAMCLI} validate --no-net --pedantic
                   ${CMAKE_CURRENT_BINARY_DIR}/online.fujinet.go.atari2600.gnome.metainfo.xml)
endif()
