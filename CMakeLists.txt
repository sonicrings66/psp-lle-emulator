cmake_minimum_required(VERSION 3.20)
project(psp_lle_beta CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Phase A, in progress: third_party (vendored) + src/core (real KIRK-based
# firmware extraction so far) -- see docs/PORTING_LOG.md.
add_subdirectory(third_party/libkirk)
add_subdirectory(third_party/puff)
add_subdirectory(src/config)
add_subdirectory(src/core)
add_subdirectory(testsupport)
add_subdirectory(tests)
