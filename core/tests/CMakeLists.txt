# ctest suites. Each is a small program; the ones that need a running
# machine boot FujiNet's CONFIG client, which is always available (it is
# embedded in Stella), so nothing here ever skips for want of a ROM.

# M1: the Stella host layer on its own, before the C session API exists.
add_executable(host_test host_test.cxx)
target_link_libraries(host_test PRIVATE stella_core)
add_test(NAME host_test COMMAND host_test)
set_tests_properties(host_test PROPERTIES TIMEOUT 120)

# The debugger's FujiNet trace decoder, pure: synthetic SLIP bytes both
# ways, split input, folded repeats. No emulator, no network.
add_executable(fuji_trace_test fuji_trace_test.c "${CMAKE_SOURCE_DIR}/core/src/fuji_trace.cpp")
target_include_directories(fuji_trace_test PRIVATE
  "${CMAKE_SOURCE_DIR}/core/include" "${CMAKE_SOURCE_DIR}/core/src")
target_link_libraries(fuji_trace_test PRIVATE Threads::Threads)
add_test(NAME fuji_trace COMMAND fuji_trace_test)

# These need the session layer, which needs SDL3. On a build without it
# (a cross-toolchain, or a lean machine) the core's own tests still run.
if(TARGET a2600_session)

# The frontend contract, against a real temporary XDG tree: settings persist,
# paths resolve, the CONFIG client boots and paints, input reaches the ports.
add_executable(session_test session_test.c)
target_link_libraries(session_test PRIVATE a2600_session)
add_test(NAME session COMMAND session_test)
set_tests_properties(session PROPERTIES TIMEOUT 120)

# The remappable binding table and the keyboard translator, pure.
add_executable(bindings_test bindings_test.c)
target_link_libraries(bindings_test PRIVATE a2600_session)
add_test(NAME bindings COMMAND bindings_test)

# The external-FujiNet address field, pure.
add_executable(hostport_test hostport_test.c)
target_link_libraries(hostport_test PRIVATE a2600_session)
add_test(NAME hostport COMMAND hostport_test)

# "Use external FujiNet" end to end: a bad address refuses, a good one is
# dialled by the cartridge (a stand-in listener sees it), no runtime starts.
add_executable(external_test external_test.c)
target_link_libraries(external_test PRIVATE a2600_session)
add_test(NAME external COMMAND external_test)
set_tests_properties(external PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)

# Where a dropped file goes.
add_executable(media_test media_test.c)
target_link_libraries(media_test PRIVATE a2600_session)
add_test(NAME media COMMAND media_test)

# The real in-process FujiNet: brings libfujinet up on its port, boots the
# CONFIG client and confirms the cartridge's link came up. SKIPs (77) when
# the runtime was not built (-DWITH_FUJINET=OFF). Serial: it owns the port.
add_executable(fujibus_smoke fujibus_smoke.c)
target_link_libraries(fujibus_smoke PRIVATE a2600_session)
add_test(NAME fujibus_smoke COMMAND fujibus_smoke)
set_tests_properties(fujibus_smoke PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120 RUN_SERIAL ON)

# The gamepad layer's pure helpers, and that the SDL subsystem starts and
# stops with no hardware attached.
add_executable(gamepad_test gamepad_test.c)
target_link_libraries(gamepad_test PRIVATE a2600_session)
target_include_directories(gamepad_test PRIVATE "${CMAKE_SOURCE_DIR}/core/src")
add_test(NAME gamepad COMMAND gamepad_test)

# The debugger contract against the running CONFIG client: stop, registers,
# step, disassembly with the PC line, breakpoints, TIA state, resume.
add_executable(debug_test debug_test.c)
target_link_libraries(debug_test PRIVATE a2600_session)
add_test(NAME debug COMMAND debug_test)
set_tests_properties(debug PROPERTIES TIMEOUT 120)

endif()
