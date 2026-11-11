add_library(beta_testsupport STATIC
    golden_trace.cpp
)
target_include_directories(beta_testsupport PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
