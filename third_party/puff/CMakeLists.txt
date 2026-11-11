# Vendored from zlib's official contrib/puff (github.com/madler/zlib),
# zlib-licensed. A minimal, dependency-free reference DEFLATE decoder --
# used because this toolchain has no zlib available, and every file inside
# a PSAR firmware archive is zlib-compressed.
add_library(puff STATIC
    puff.c
)

target_include_directories(puff PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if(MSVC)
    target_compile_options(puff PRIVATE /w)
else()
    target_compile_options(puff PRIVATE -w)
endif()
