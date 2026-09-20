# ctest suites. Each is a small program; the ones that need a running
# machine boot FujiNet's CONFIG client, which is always available (it is
# embedded in Stella), so nothing here ever skips for want of a ROM.

# M1: the Stella host layer on its own, before the C session API exists.
add_executable(host_test host_test.cxx)
target_link_libraries(host_test PRIVATE stella_core)
add_test(NAME host_test COMMAND host_test)
set_tests_properties(host_test PROPERTIES TIMEOUT 120)
