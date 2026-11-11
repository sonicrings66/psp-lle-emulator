#pragma once

#include <cstdint>

#include "../bus/system_bus.h"
#include "../memory_map.h"
#include "../util/nand_image.h"

// Wires real console hardware state this project's boot driver already
// loads from config/disk (the real per-console NAND dump, the real Fuse
// ID) into the live, running SystemBus, so real guest code that depends on
// them -- lfatfs.prx's own real NAND page/spare descramble chain (ported
// in the old project as format::Flash0Fs, host-side only there; here the
// goal is the real thing, run by real guest code), any later real sceNand
// command -- sees real data instead of this bus's generic accept-anything
// stub behavior. Split out of psp_boot_driver.cpp so the driver's own
// per-run setup reads as a short list of real steps instead of growing
// another inline block.
namespace beta::firmware {

// Attaches `nand` to the live NandController (`SystemBus::attachRealNandImage`)
// so real NAND commands get real page/spare data back, and writes the real
// Fuse ID into the real sysreg registers (0xBC100090/0x94, jpcsp
// MMIOHandlerSystemControl) real firmware's own NAND-descramble code reads
// it from. This is a SEPARATE real register from this project's own KIRK-
// decrypt path (fuse_config.h), which already has the Fuse ID directly --
// this is the one real guest-executed code would read.
//
// `nand` must outlive the SystemBus (SystemBus::attachRealNandImage keeps
// a raw, non-owning pointer) -- true for psp_boot_driver.cpp's own local,
// which lives for the whole boot run.
//
// Also writes the real Tachyon version into the same HW_RAM_SIZE register
// (0xBC100040) real firmware's own sceSysregGetTachyonVersion reads --
// jpcsp MMIOHandlerSystemControl's real format is (tachyonVersion << 8) |
// ramSize. This console is a confirmed real PSP-3000 03g (Tachyon
// 0x00600000, see memory reference_real_psp3000_pspident.md), the same
// console the NAND dump comes from -- not a guess. lfatfs.prx/lowio.prx
// read this to pick the real large-NAND block-zone layout (0x1F8 zones vs
// the wrong 0x1F0 default with Tachyon left at 0). Independently correct
// and kept, but NOTE: re-tested directly this session and confirmed this
// register is NOT what gates the real self-loop at pc=0x8816F538 -- that
// halt is loadexec.c's own real runReboot() dead end, reached from
// sceKernelExitVSHKernel unconditionally on every cold VSH boot (see
// memory project_psp-lle-beta-rework.md's 2026-11-07 entry). Deferred
// this late (same as the Fuse ID writes) for the same documented reason:
// real firmware/the old project's own finding is that writing it at reset
// breaks the real pre-IPL/IPL chain.
inline void attachRealNandAndFuse(SystemBus& bus, const NandImage& nand, uint32_t fuseId90, uint32_t fuseId94) {
    bus.attachRealNandImage(&nand);
    constexpr uint32_t kTachyonVersion03g = 0x00600000u;
    constexpr uint32_t kRamType64Mb = 0x2u;
    bus.write32(0xBC100040u, (kTachyonVersion03g << 8) | kRamType64Mb);
    bus.write32(MemoryMap::kSysconBase + 0x90u, fuseId90);
    bus.write32(MemoryMap::kSysconBase + 0x94u, fuseId94);
}

} // namespace beta::firmware
