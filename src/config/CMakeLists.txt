add_library(beta_config STATIC
    options_cli.cpp
)
target_include_directories(beta_config PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
