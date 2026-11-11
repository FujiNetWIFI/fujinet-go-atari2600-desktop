# The KDE frontend: plain Qt6 Widgets.
#
# Deliberately NOT KDE Frameworks. It picks up Breeze through the platform
# theme anyway, and staying framework-free keeps this frontend usable outside
# a KDE session -- and keeps the door open to reusing it elsewhere.
#
# Found-or-skipped, like the GNOME one: a machine without Qt6 development
# packages still builds the core, the tests and the other frontend.

set(CMAKE_AUTOMOC ON)

# Qt 6.7 for QRhiWidget (the GPU display); ShaderTools bakes its shaders.
find_package(Qt6 6.7 COMPONENTS Widgets GuiPrivate ShaderTools QUIET)
if(NOT Qt6_FOUND)
  message(STATUS "kde frontend: Qt6 Widgets not found -- skipping")
  return()
endif()

add_executable(fujinet-go-atari2600-kde
  main.cpp
  MainWindow.cpp
  DisplayWidget.cpp
  SoftDisplayWidget.cpp
  FrameFeed.cpp
  KeyForward.cpp
  SettingsDialog.cpp
  FujiNetWindows.cpp
  help/HelpWindow.cpp
  keypad/KeypadWindow.cpp
  debugger/DebuggerWindow.cpp
  debugger/VideoTab.cpp
)
target_link_libraries(fujinet-go-atari2600-kde PRIVATE a2600_session Qt6::Widgets Qt6::GuiPrivate)
# The display's shaders, baked for every backend and embedded as resources.
qt_add_shaders(fujinet-go-atari2600-kde "display_shaders"
  PREFIX "/"
  FILES shaders/display.vert shaders/display.frag)
target_compile_options(fujinet-go-atari2600-kde PRIVATE -Wall -Wextra)
target_compile_definitions(fujinet-go-atari2600-kde PRIVATE
  A2600_SOURCE_ICON_DIR="${CMAKE_SOURCE_DIR}/data/icons")

# The FujiNet web UI, embedded with QtWebEngine when available and
# WITH_WEBVIEW is on; otherwise the configuration opens in the system
# browser. Never a hard dependency: QtWebEngine is enormous and absent on
# many systems.
if(WITH_WEBVIEW)
  find_package(Qt6 COMPONENTS WebEngineWidgets QUIET)
  if(Qt6WebEngineWidgets_FOUND)
    target_link_libraries(fujinet-go-atari2600-kde PRIVATE Qt6::WebEngineWidgets)
    target_compile_definitions(fujinet-go-atari2600-kde PRIVATE HAVE_WEBENGINE=1)
    message(STATUS "kde frontend: embedding the FujiNet web UI (QtWebEngine)")
  else()
    message(STATUS "kde frontend: QtWebEngine not found -- the FujiNet web UI opens in the browser")
  endif()
endif()

install(TARGETS fujinet-go-atari2600-kde RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

configure_file(data/online.fujinet.go.atari2600.kde.desktop.in
               ${CMAKE_CURRENT_BINARY_DIR}/online.fujinet.go.atari2600.kde.desktop
               @ONLY)
configure_file(data/online.fujinet.go.atari2600.kde.metainfo.xml.in
               ${CMAKE_CURRENT_BINARY_DIR}/online.fujinet.go.atari2600.kde.metainfo.xml
               @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/online.fujinet.go.atari2600.kde.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/online.fujinet.go.atari2600.kde.metainfo.xml
        DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)

# The Debugger window (and F12 from the main window), on the offscreen
# platform, against a running session whose RAM holds a hand-assembled loop.
find_package(Qt6 COMPONENTS Test QUIET)
if(Qt6Test_FOUND)
  add_executable(debugger_test debugger_test.cpp debugger/DebuggerWindow.cpp
                 debugger/VideoTab.cpp
                 MainWindow.cpp DisplayWidget.cpp SoftDisplayWidget.cpp
                 FrameFeed.cpp SettingsDialog.cpp KeyForward.cpp
                 FujiNetWindows.cpp help/HelpWindow.cpp keypad/KeypadWindow.cpp)
  set_target_properties(debugger_test PROPERTIES CXX_STANDARD 17)
  target_link_libraries(debugger_test PRIVATE a2600_session Qt6::Widgets
                        Qt6::GuiPrivate Qt6::Test)
  qt_add_shaders(debugger_test "display_shaders_test"
    PREFIX "/"
    FILES shaders/display.vert shaders/display.frag)
  target_include_directories(debugger_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  target_compile_options(debugger_test PRIVATE -Wall -Wextra)
  target_compile_definitions(debugger_test PRIVATE
    A2600_SOURCE_ICON_DIR="${CMAKE_SOURCE_DIR}/data/icons")
  if(WITH_WEBVIEW AND Qt6WebEngineWidgets_FOUND)
    target_link_libraries(debugger_test PRIVATE Qt6::WebEngineWidgets)
    target_compile_definitions(debugger_test PRIVATE HAVE_WEBENGINE=1)
  endif()
  add_test(NAME debugger_kde COMMAND debugger_test)
  set_tests_properties(debugger_kde PROPERTIES
                       ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()

find_program(DESKTOP_FILE_VALIDATE desktop-file-validate)
if(DESKTOP_FILE_VALIDATE)
  add_test(NAME desktop_file_kde
           COMMAND ${DESKTOP_FILE_VALIDATE}
                   ${CMAKE_CURRENT_BINARY_DIR}/online.fujinet.go.atari2600.kde.desktop)
endif()
find_program(APPSTREAMCLI appstreamcli)
if(APPSTREAMCLI)
  add_test(NAME metainfo_kde
           COMMAND ${APPSTREAMCLI} validate --no-net --pedantic
                   ${CMAKE_CURRENT_BINARY_DIR}/online.fujinet.go.atari2600.kde.metainfo.xml)
endif()
