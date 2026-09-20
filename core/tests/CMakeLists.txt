# ctest suites. Each is a small program; the ones that need a running
# machine boot FujiNet's CONFIG client, which is always available (it is
# embedded in Stella), so nothing here ever skips for want of a ROM.

# M1: the Stella host layer on its own, before the C session API exists.
add_executable(host_test host_test.cxx)
target_link_libraries(host_test PRIVATE stella_core)
add_test(NAME host_test COMMAND host_test)
set_tests_properties(host_test PROPERTIES TIMEOUT 120)

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

endif()
