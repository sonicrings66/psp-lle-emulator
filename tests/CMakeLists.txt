add_executable(beta_tests
    test_main.cpp
    test_state_hash.cpp
    test_golden_trace.cpp
    test_firmware_extraction.cpp
    test_system_bus.cpp
    test_cpu.cpp
    test_vfpu.cpp
    test_module_stager.cpp
)
target_link_libraries(beta_tests PRIVATE beta_testsupport beta_config beta_core)
target_compile_definitions(beta_tests PRIVATE BETA_TESTS_SRC_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

enable_testing()
add_test(NAME beta_tests COMMAND beta_tests)

# Phase A milestone driver (see the plan's tests/driver/psp_boot_driver.cpp
# entry): a thin app driver, not a unit test -- its own executable, not
# folded into beta_tests.
add_executable(psp_boot_driver driver/psp_boot_driver.cpp)
target_link_libraries(psp_boot_driver PRIVATE beta_testsupport beta_config beta_core)
