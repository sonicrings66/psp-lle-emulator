# Vendored verbatim from ppsspp-master/ext/libkirk (the "kirk-engine" open
# reverse-engineering project). See individual file headers for original
# authors/licenses: kirk_engine.c/h and bn.c/ec.c are GPLv2-or-v3, AES.c is
# public domain, amctrl.c/SHA1.c carry their own headers. Not held to this
# project's own warning level since it isn't our code.
add_library(kirk_engine STATIC
    kirk_engine.c
    AES.c
    SHA1.c
    bn.c
    ec.c
    amctrl.c
)

target_include_directories(kirk_engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if(MSVC)
    target_compile_options(kirk_engine PRIVATE /w)
else()
    target_compile_options(kirk_engine PRIVATE -w)
endif()
