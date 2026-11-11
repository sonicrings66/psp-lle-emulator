# HLE inventory

This project's standing rule is LLE first, HLE only where documented as a deliberate
exception (see `README.md`, `docs/PORTING_LOG.md`). This file is that documentation:
every place real hardware/firmware behavior is currently stood in for rather than
executed/modeled for real, plus the emulator-side patch sites that look similar but
aren't actually HLE. It replaces the forward references to `docs/HLE.md` already sitting
in `docs/PORTING_LOG.md` (e.g. "Phase B... populate a registry from `docs/HLE.md`'s real
patch sites") that pointed at a file that didn't exist until now.

Four categories below: (B) genuine, permanent HLE stand-ins; (C) inline PC-keyed patches
in the boot driver that look like HLE but are actually workarounds for this project's own
modeling gaps; (D) an HLE-capable mechanism nobody uses yet; and stale doc references
found while compiling this. (Category "A", real LLE with no HLE at all — KIRK, flash0/NAND
decrypt, PSAR module decryption, syscon buttons/analog, the low-32 interrupt sources, the
system timer/alarm — is the majority of the codebase and isn't itemized here; it's the
baseline this file is measuring exceptions against.)

## B. Genuine HLE stand-ins (by design)

1. **GE 3D rasterization** — `src/core/gpu/ge_renderer.h` (2048 lines) wired into
   `src/core/mmio/ge_edram.h`'s `GeCoreStub`. Real GE command dispatch (JUMP/CALL/RET/BASE,
   stall address, SIGNAL/FINISH/END raising the real status bits and IRQ 25) is genuine
   LLE — `ge.prx`'s own queue/sync logic runs for real against it. The pixel-producing half
   is a from-scratch host software rasterizer, not firmware code, because the GE is
   fixed-function silicon with no kernel-module equivalent to trace — this one is
   structurally unavoidable, not a shortcut around unfinished tracing work.
   **Correction while compiling this**: `ge_edram.h`'s own header comment (lines 43-46)
   still says "MINIMAL... nothing is rendered" — that's stale. `GeRenderer` is a real,
   substantial rasterizer and the XMB now renders and is interactive (2026-11-11). The
   comment should be updated to describe what actually renders today rather than the
   original command-walk-only milestone.
2. **ME audio decode** — `src/core/me/me_codec.h`. Answers AT3+/MP3 decode RPCs with
   silence instead of real decoded audio, using the real work-area protocol (field offsets,
   real inSize/outSize formulas) so the caller's bookkeeping stays consistent. The old
   project (`psp-lle-emu`) has real ffmpeg-derived ATRAC3/ATRAC3+ decoders and a minimp3
   MP3 decoder; neither has been ported here yet (`system_bus.h`'s own header comment notes
   this explicitly as a deliberate omission, not an oversight).
3. **ME RPC completion when no ME core is attached** — `src/core/mmio/system_control.h:73-77`.
   `sceSysregInterruptToOther` normally wakes the real second Allegrex core (the Media
   Engine); with `meLle` off there is no second core running this emulator's CPU, so the
   write instead completes the RPC instantly and raises `PSP_MECODEC_INTR` (31) itself. When
   `meLle` is on, the real ME core answers through `onInterruptToMe` for real — the genuine
   LLE path exists and is preferred; this stand-in is only the fallback.
4. **Memory Stick controller presence/ready** — `src/core/mmio/ms_controller.h`. STATUS
   always reads back "ready" instead of modeling jpcsp's ~1500-line real TPC/sector/boot-page
   protocol — explicitly called out in its own header as "a deliberate, minimal, HONEST
   shortcut... unlike KIRK, whose real crypto this project actually runs." A real protocol
   engine (`mmio/ms_engine.h`) exists and takes over when a card image is attached
   (`attachCard`); without one, this always-ready stub is what runs.
5. **WLAN device command set** — `src/core/mmio/wlan_device.h`. Real TPC-style
   command/data-port protocol (matching jpcsp) for every command this console's own
   firmware actually issues on the Wi-Fi / Internet Connection Test path; roughly 15 other
   real commands/sub-cases are stubbed (`event("... (not modeled)", ...); resultCode = -1;`)
   because nothing traced so far issues them.

## C. Boot-driver patch sites — mostly NOT hardware HLE

`tests/driver/psp_boot_driver.cpp` has 8 inline `if (cpu.state().pc == 0x...)` checks. These
are exactly the kind of site the original rework plan's never-built `HleRegistry` (Phase B)
was meant to hold — so this table is the closest thing to that registry today. Only 3 of the
8 are genuine stand-ins for real hardware behavior; the other 5 are workarounds for gaps or
bugs in this project's own sysmem/loadcore/allocator modeling, not substitutes for anything
real hardware does differently.

| PC | What it does | Kind |
|---|---|---|
| `0x88110B04` | reserve the kernel partition's already-staged early-boot module range before sysmem's heap allocator can overwrite it | real-bug workaround (sysmem reservation gap) |
| `0x8816F58C` | JIT array extension: inject the 60 real pspbtcnf `extraModules` into `bootInfo`/`g_init` right before `InitInit`'s one-time scan reads them | real-bug workaround (one-time-scan timing/addressing) |
| `0xBFC00000` (Status.EXL set) | fail loudly instead of silently masking an unexpected real exception at the boot vector | diagnostic guard, not HLE |
| `0xBFC00000` (cIPL RAM-clear call, matched by its real first instruction word) | no-op the real cIPL RAM-clear utility — its real effect (clean RAM) is already achieved by this driver's own pre-staging order | **genuine HLE**, documented, ported from the old project |
| `0x8811a6b8` (first check) | one-shot restore of a hand-placed module's pristine snapshot right before loadcore's own validator reads it | real-bug workaround (cross-module heap corruption, root sysmem partition call not yet modeled) |
| `0x8811a900` | `[HLE exception: early-boot-array CheckLatestSubType, documented]` — forces v0=1 for every non-`memlmd` hand-placed module so a real-but-irrelevant preliminary pass doesn't panic | **genuine HLE**, documented, verified live that the real mechanism never matters for boot |
| `0x8811c83cu` | memlmd decrypt hook: runs the real KIRK decrypt host-side via `crypto::decryptPrx` | **genuine HLE** — this project doesn't model real `KirkMmio` access from CPU-executed code |
| `0x8811a6b8` (second check, same PC) | one-shot restore of the real import-stub trampoline region before the first module validation reads it | real-bug workaround (unroot-caused address-computation bug in real `FUN_88118080`) |

## D. HLE-capable framework with zero registered users

`src/core/syscall_resolver.h`'s `tryHostSyscall`/`registerHostSyscall` (implemented in
`src/core/bus/system_bus.h`) is a real syscall-number → host-callback mechanism, built
specifically so a future syscall-level HLE patch (e.g. a real `sceIo` file operation)
wouldn't need its own ad-hoc PC-keyed check. **Nothing currently registers a callback
through it anywhere in `src/` or `tests/`** (confirmed by grep) — every HLE/workaround site
that exists today is PC-keyed in the boot driver instead (category C above), which is
exactly the "one behavior, one home" problem the plan's `HleRegistry` was meant to solve.

## Stale/aspirational references found while compiling this

- `hle/mem_stick.h` — referenced by name twice (`ms_controller.h:16`, `system_control.h:82-83`)
  as "where every real file access is already fully handled at the sceIo layer." This file
  does not exist, and **no `sceIo`/`IoFileMgr`-level HLE layer exists anywhere in this
  project yet** (grep for `sceIo`/`IoFileMgr` across `src/` returns only those two comments
  referencing it, not an implementation). Boots work today only because nothing yet
  exercises real Memory Stick file I/O this early in the boot sequence.
- `ge_edram.h`'s "MINIMAL... nothing is rendered" comment (see item B1) — outdated now that
  the renderer actually produces the live, interactive XMB.

## Tally

- Real LLE, no HLE at all: the majority of `src/core` — KIRK, flash0/NAND decrypt, PSAR/KIRK
  module decryption, syscon buttons/analog, the interrupt controller's low 32 sources, the
  system timer/alarm.
- Genuine, documented HLE stand-ins: **5** (GE rasterization, ME audio decode, ME-RPC
  completion without an ME core, Memory Stick presence, ~15 unmodeled WLAN sub-commands).
- Boot-driver PC-keyed patch sites: **8** total — **3** genuine HLE, **5** real-bug
  workarounds that aren't HLE at all.
- HLE-capable mechanism with **0** registered callbacks: `registerHostSyscall`/`tryHostSyscall`.
- Stale doc references found: **2** (`hle/mem_stick.h` x2 mentions; `ge_edram.h`'s rendering
  comment).
- Plan item still open: `HleRegistry` (plan Phase B) was never built; this file's section C
  table is what it would hold today, laid out by hand instead of by a registry.
