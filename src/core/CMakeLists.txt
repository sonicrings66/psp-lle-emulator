# Phase A, nearly done: crypto + format + util + mmio/* + Bus/SystemBus +
# Cpu/CpuState/VFPU/VME + format/elf.cpp + module_link.cpp all ported (real
# KIRK decryption chain, the full MMIO device set, the composition-root bus,
# the real Allegrex interpreter, real ELF loading + inter-module linking).
# me/me_core.h + gpu/ (Phase C) are not ported yet; the thin boot driver
# (tests/driver/psp_boot_driver.cpp) and Milestone A's state-hash-parity
# boot test are next -- see docs/PORTING_LOG.md for exactly where this left
# off.
add_library(beta_core STATIC
    crypto/kirk.cpp
    crypto/prx_decrypt.cpp
    crypto/prx_tags.cpp
    crypto/des.cpp
    format/pbp.cpp
    format/psar.cpp
    format/psar_names.cpp
    format/elf.cpp
    format/module_link.cpp
    util/inflate.cpp
    util/kl4e.cpp
    firmware/ipl_extractor.cpp
    firmware/pspbtcnf_parser.cpp
    firmware/module_stager.cpp
    memory.cpp
    net/host_resolver.cpp
    net/host_tcp.cpp
    net/host_udp.cpp
    bus/system_bus.cpp
    cpu.cpp
    me/vme.cpp
)

target_include_directories(beta_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(beta_core PUBLIC kirk_engine puff beta_config)
if(WIN32)
    target_link_libraries(beta_core PUBLIC ws2_32)
endif()

if(MSVC)
    target_compile_options(beta_core PRIVATE /W4)
else()
    target_compile_options(beta_core PRIVATE -Wall -Wextra)
endif()
