# Porting log

Running log of what moved from `psp-lle-emu` into `psp-lle-beta`, what changed, and why.
See `C:\Users\quinn\.claude\plans\polished-swinging-wall.md` for the full rework plan.

## Phase 0 — Scaffolding + parity harness (2026-11-03)

New, not ported (no old-project equivalent existed as a standalone module):

- `src/config/options.h`, `options_cli.h/.cpp`, `logging.h`, `logging_sinks.h` — the
  injected config/logging layer that replaces every `getenv()` site below the launcher
  boundary. Old project: ~171 combined `getenv()` sites (45 in `src/core`, 126 in
  `tests/test_ipl_realfuse.cpp`), including the one that silently gated the GAME-boot
  warm-reboot restart behind `PSP_WINDOW` and cost a ~3.5-hour wasted verification run.
- `testsupport/state_hash.h` — `StateHasher`, a from-scratch implementation pinned to
  match `test_ipl_realfuse.cpp:3849-3855`'s `PSP_STATE_HASH` semantics exactly (same
  FNV-1a offset basis `1469598103934665603` / prime `1099511628211`, same CPU-state vs.
  RAM split), specifically so the old project's own golden `[STATE]` trace lines remain
  valid comparison baselines for beta without re-deriving anything.
- `testsupport/golden_trace.h/.cpp` — `GoldenTrace`, parses the old project's
  `[STATE] instr ... cpu=... ram=... pc=... count=...` log lines directly. Verified
  against a real sample pulled from a live `psp-lle-emu` run this session (see
  `tests/fixtures/sample_state_trace.log`).

Milestone 0 (per the plan): "`testsupport` can load an old-project `PSP_STATE_HASH` log
file and diff against a second one, byte-for-byte, as a standalone unit test, no PSP code
running yet" — **met**, `tests/beta_tests.exe`, 9/9 passing, including two mutation tests
(`StateHash.Fnv1aMutationChangesHash`, `GoldenTrace.FirstMismatchFindsTheExactDivergingEntry`)
confirming the checks can actually fail, per `docs/TESTING.md`'s "a test that cannot fail
is not a test" rule.

Standing check established: `rg "getenv\(" src/` returns matches only in
`src/config/options_cli.cpp` (the other 3 hits in that same grep are comment prose
explaining the rule, not calls — confirmed by reading each line).

## Phase A, part 1 — real KIRK decryption chain (2026-11-03)

Ported, close to verbatim (namespace `allegrex::` -> `beta::`, debug-tap cleanup noted
below), all genuinely reusable per the Phase-A research:

- `third_party/libkirk/`, `third_party/puff/` — vendored wholesale, unchanged.
- `src/core/crypto/kirk.h/.cpp` — verbatim port, zero changes beyond namespace.
- `src/core/crypto/des.h/.cpp`, `prx_tags.h/.cpp`, `prx_decrypt.h/.cpp` — verbatim port
  (flagged in the plan's §9 as PPSSPP-derived/GPL; licensing decision still pending,
  not blocking functional porting).
- `src/core/util/inflate.h/.cpp` — verbatim port.
- `src/core/util/nand_image.h` — verbatim port (had a bare `namespace allegrex {`, not
  `allegrex::util`, since the old project put `NandImage` at the top-level namespace
  despite living under `util/` — preserved as `namespace beta {` for the same reason).
- `src/core/util/kl4e.h/.cpp` — **merged** the old project's two independent KL4E
  transcriptions (`format/kl4e.h`, header-only/bool-returning/consumed-bytes, vs.
  `util/kl4e.h/.cpp`, throwing/`.at()`-bounds-checked) per the plan's "one behavior, one
  home" principle. Kept the latter's safer style, added the former's `consumedOut`
  out-parameter so both call sites (the ME bootstrap's two-segment image, Phase C; and
  `crypto/prx_decrypt.cpp`, ported now) are served by one implementation.
- `src/core/format/pbp.h/.cpp` — verbatim port.
- `src/core/format/psar.h/.cpp`, `psar_names.h/.cpp` — ported with the duplicate
  `psarDebug()`/`PSP_PSAR_DEBUG` getenv-gated helper (copy-pasted across both `.cpp`
  files in the old project) **dropped entirely**, along with its 3 call sites — all were
  pure one-off diagnostic prints with no behavioral effect, exactly the kind of noise
  Phase A's own policy says to drop rather than thread a Logger through for.
- `src/core/firmware/ipl_extractor.h/.cpp` — **new module**, the real NAND-based IPL
  (second-stage bootloader) extraction ported from
  `test_ipl_realfuse.cpp:235-517`'s `extractAndLoadRealIpl`/`extractAndLoadRealIplCached`
  (verbatim-shared with the old project's `test_pbp.cpp`). Decoupled from `Bus&` (which
  doesn't exist in beta yet) via a `WriteWordFn` callback the caller supplies — cleaner
  than the old signature, not just a stopgap. Printf diagnostics dropped; return-value
  contract (0 vs. a real entry point on every failure/success branch) preserved exactly,
  since those values carry real meaning the old code's control flow depended on.

**Deliberately NOT ported:** `crypto/kirk_stub.h` — a historical placeholder from before
the real `KirkEngine` was wired in; `ipl_loader.h/.cpp` (which used it) hasn't been ported
yet either, and when it is, beta's version will just take a real `KirkEngine&` directly.

**Real-firmware verification (not synthetic test data)** — directly answers the user's
request that firmware/kirk actually do the real decrypting of boot modules:

- `FirmwareExtraction.RealFirmwareVersionDecryptsViaRealKirk`: `format::readFirmwareVersion`
  against the user's real `661.PBP`, through the real demangle (KIRK CMD7) + type-0 PRX
  decrypt (real `KirkEngine::command()`) chain, returns `"6.61"` — the project's own
  documented "first real, checkable proof" this chain works end to end.
- `FirmwareExtraction.RealPsarEntriesResolveViaRealKirk`: walks the whole real PSAR
  archive, finds `sysmem.prx` with real, non-empty decrypted+decompressed content.
- `FirmwareExtraction.RealNandIplExtractionMatchesOldProjectCachedResult`: runs the real
  NAND-based IPL extraction (real KIRK decrypt + SHA1 + ECDSA_VERIFY chain) against the
  user's real NAND dump (`UNKNOWN_db40916a_64M.bin`). **Found and corrected a stale claim**
  in the old project's own file header (a 2026-09-11 comment claiming this always fails
  ECDSA_VERIFY) — cross-checked against the old project's own cached extraction result on
  disk (`scratch/cache/ipl_0000000000000000_C5720C1906C5F5C9.bin`, placeholder fuse) and
  confirmed the real, current, verified entry point is `0x040E0000`, which this port
  reproduces exactly. Pinned as the real parity baseline instead of the stale comment.

All 12/12 `beta_tests` passing, zero compiler warnings (`-Wall -Wextra`).

## Phase A, part 2 — Bus interface split (2026-11-03)

Split the old project's `bus.h` (15+ virtuals on one interface, confirmed by direct
read) into four headers per the plan's §1a, each with its full original doc-comment
provenance preserved:

- `src/core/bus.h` — `Bus`: read8/16/32, write8/16/32, `pendingInterrupts()`. The only
  thing every addressable thing (RAM, IPL region, every MMIO device, test fixtures)
  must implement.
- `src/core/timing_source.h` — `TimingSource`: `setTimerInterruptPending`, `tick`,
  `tickN`, `ticksUntilEvent`, `advanceTicks`.
- `src/core/jit_hints.h` — `JitHints`: `ramWindow`, `ramDirectOk`, `ramWriteCounter`,
  `hasSideEffects`, `tracksCodeWrites`, `watchCode`, `codeWriteEpoch`,
  `storeNeedsInterpreter`, `codeFetchIsTimingNeutral`, `ramWatchMap`.
- `src/core/syscall_resolver.h` — `SyscallResolver`: `resolveSyscall`, `registerSyscall`,
  `tryHostSyscall`/`HostSyscallFn`, `registerHostSyscall`.

Verified with a standalone multiple-inheritance compile check (a minimal `TestBus :
public Bus, TimingSource, JitHints, SyscallResolver` implementing only the 6 pure
`Bus` methods) — compiles clean with `-Wall -Wextra`. `SystemBus` (not yet ported) will
compose all four; a unit-test fixture bus only needs `Bus`.

Next: `SystemBus` composition root (device registry per §1b, keeping typed accessors
only for the ~10 devices cross-cutting code needs by name), then `format/elf.cpp`/
`module_link.cpp` (need `Bus`), then `mmio/*`, then `Cpu`/`CpuState` (architectural/
debug-state split).

## Phase A, part 3 — all 22 `mmio/*` files + `SystemBus` (2026-11-03)

Ported every `mmio/*` file (22 total) and the `SystemBus` composition root
(`memory.h/.cpp`, `memory_map.h` had already landed in part 2). `net/{host_resolver,
host_tcp,host_udp,tcp_proxy,udp_proxy,virtual_ap}.*` and `util/ms_image.h` ported too
(needed by `wlan_device.h`/`ms_controller.h`). All 13 no-getenv mmio files ported
verbatim via `port_ns` (line counts matched the originals exactly): `generic_stub.h`,
`ddr_controller.h`, `display_controller.h`, `ge_edram.h` (ported to disk but NOT wired
into anything, see below), `i2c.h`, `interrupt_controller.h`, `lcd_controller_slim.h`,
`me_controller.h`, `power_regs.h`, `system_control.h`, `system_time.h`, `timer.h`
(given the `TimingSource` mixin for its `tick()` override -- confirmed by grep across
all 22 files that `tick()` is the ONLY non-`Bus` override anywhere in `mmio/`).

### getenv classification (the real point of this pass)

Every one of the 9 remaining getenv-bearing mmio files was read in full and each site
classified individually, not bulk-guessed:

**Dropped as pure diagnostic (no behavioral effect, confirmed by reading every use of
the backing variable, not just the read site):**
- `audio.h`: `PSP_AUDIO_LOG` (gated two printfs only).
- `dmac.h`: `PSP_AUDIO_LOG`/`PSP_AUDIO_LOG2` (`dbgOn()`, 6 call sites, all printf-only).
- `gpio.h`: `PSP_SYSCON_GATE_LOG` (a "TEMPORARY" investigation printf).
- `ms_controller.h`: `PSP_MS_REGDBG` (4 sites, all printf-only).
- `ms_engine.h`: `PSP_MS_CTRL_LOG` -> `logLevel_` (checked all 7 uses: every one only
  gates print verbosity; the real diagnostic STATE this device tracks -- `statUnimpl`,
  `statTpc`, `statCmd`, `lastUnimpl` -- are not getenv-gated at all and were kept as-is,
  since they're genuine counters a caller's summary can read, not debug noise).
- `nand_controller.h`: `PSP_NAND_IDSTORAGE_LOG` ("TEMPORARY" investigation printf).
- `syscon_chip.h`: `PSP_SYSCON_LOG` (printf only).
- `umd_engine.h`: `PSP_UMD_LOG`/`PSP_UMD_LOG_FROM` -> `log_`/`logFromInstr_` (checked
  all 10 uses across the file: every one gates a printf; also removed the now-pointless
  public `setLogFromInstr()` method and `logActive()` helper, both existing purely to
  serve the dropped mechanism). One removal (`if (!iso_) { diagnostic-only }` inside
  `umdCommand`'s case 0x0A) initially broke the surrounding `if/else` into a dangling
  `else` with no `if` -- caught by re-reading the edited block immediately after, fixed
  by inverting the condition (`if (iso_) { ...real logic... }`, semantically identical
  since the removed branch did nothing but print).
- `wlan_device.h`: `PSP_WLAN_LOG`/`PSP_WLAN_VERBOSE` -> `limit_`/`count_`/`verbose`
  (checked all 6 uses: all printf-only). `event()` is now an unconditional no-op rather
  than having every one of its ~15 call sites throughout the file individually removed
  -- a deliberate, documented trade-off (see the function's own comment) to avoid a much
  larger diff for zero behavioral difference.

**Threaded into new `*Options` structs in `src/config/options.h`** (new fields:
`AudioOptions::dmaMs`/`wavDumpDir`, new `UmdOptions::readyMs`, new
`WlanOptions::allowInternet`/`dnsOverride`, new `SysconOptions::wlanSwitchOn`/
`rtcSeedFlags`, new `BusOptions::flushPlainMemory`/`tickBatch` -- all with the SAME
default the old getenv-unset behavior had), each device constructor now takes its own
`const XOptions&` (defaulted, so existing single-arg call sites still work where no
options are needed):
- `audio.h` `PSP_AUDIO_DMA_MS` (real DMA-FIFO pacing threshold) and `PSP_AUDIO_OUT`
  (the real per-line `.wav` dump feature -- not diagnostic noise, a real optional output).
- `umd_engine.h` `PSP_UMD_READY_MS` (real simulated disc-tray-ready delay).
- `wlan_device.h` `PSP_WLAN_DNS`/`PSP_WLAN_NO_INTERNET` (real upstream-DNS override and
  real network-disable toggle).
- `syscon_chip.h` `PSP_RTC_SEED` (real emulated-clock pre-seeding) and `PSP_WLAN_SWITCH`
  (the real physical WLAN slider state). The latter was trickier: `wlanSwitchOn()` was a
  *static* function with lazy-static state (`wlanSwitchState`/`wlanEverOn`, shared
  because exactly one `SysconChip` exists per process) reading getenv on first CALL.
  Fixed by moving the read to the first `SysconChip` CONSTRUCTION instead, from
  `SysconOptions` -- behaviorally equivalent in practice (the one instance is always
  constructed before any register access can reach `wlanSwitchOn()`), documented
  explicitly in both the constructor and the now-getenv-free static accessor.
- `system_bus.cpp`'s `needsExactTime()` (`PSP_FLUSH_PLAIN`, legacy always-flush
  performance toggle) and the constructor's `PSP_TICK_BATCH` (device-time batching
  granularity) both became `BusOptions` fields.

### `SystemBus` (`src/core/bus/system_bus.{h,cpp}`, new `bus/` subdirectory)

Inherits all four split interfaces (`Bus, TimingSource, JitHints, SyscallResolver`) --
it's the one class that genuinely needs every one of them (confirmed by checking exactly
which old `Bus` virtuals it overrode: `pendingInterrupts`/`tick`/`tickN`
→ TimingSource; `ramWindow`/`ramWriteCounter`/`ramWatchMap`/`ticksUntilEvent`/
`advanceTicks`/`hasSideEffects`/`tracksCodeWrites`/`codeFetchIsTimingNeutral`/
`watchCode`/`codeWriteEpoch`/`storeNeedsInterpreter` → JitHints;
`resolveSyscall`/`registerSyscall`/`tryHostSyscall`/`registerHostSyscall` →
SyscallResolver). The real address-decode algorithm (`locate()`, `locateGenericStub()`,
the RAM-fast-path classification in `plainMemory()`/`initFastRam()`) ported verbatim --
this is exactly the "good, reusable" part the Phase-A research flagged.

**Deliberately deferred to Phase C, not wired in (see the header's own class
comment):**
- **GE** (`GeCoreStub`/`GeRenderer`, real base `0xBD400000`) -- depends on
  `gpu/ge_renderer.h` (2007 lines) and `gpu/ge_capture.h`, not ported. The GE address
  range is now genuinely unmapped (a read there throws, same as any other unmodeled
  region) rather than silently stubbed -- verified by
  `SystemBus.UnmappedReadThrows`/`UnmappedWriteIsSilentlyDiscarded` in
  `tests/test_system_bus.cpp`. `mmio/ge_edram.h` itself WAS ported to disk (verbatim,
  no getenv) since it's small and self-contained, but it's not `#include`d by
  `system_bus.h` and not in any CMake source list -- it'll be wired in exactly once
  Phase C lands `gpu/ge_renderer.h`.
- **ME** (`MeCore`, `enableMe()`/`me()`) -- depends on `me/me_core.h`, not ported. One
  real, documented side effect inherited as-is from the old project (not a new bug):
  `SysconChip::nowUsec` (the function that makes the emulated RTC clock actually
  advance) is only ever wired up *inside* `enableMe()` in the old code, so in this
  Phase-A `SystemBus`, `SysconOptions::rtcSeedFlags` seeds the clock's starting value
  but it never advances afterward. Documented in the header; resolves itself once
  Phase C ports `MeCore`/`enableMe()` for real.

**Device-registry decision (§1b):** a real-call-site audit (`grep -rn` across the old
project's `tests/` and `launcher/`) found the exact usage counts: `geCore()` ×23 (Phase
C, moot for now), `umdDrive()` ×6, `systemControl()` ×5, `nandController()` ×5,
`memoryStickController()` ×3, `attachRealNandImage()` ×3, `interruptController()` ×2,
`sysconChip()` ×1, `me()`/`enableMe()` ×1 each (Phase C), `audio()` ×1 -- and
**zero** external callers of `dmac0()`/`dmac1()`/`dmacplus()`'s accessors (only
`locate()` reaches them internally, same as every plain `MmioStub`). Conclusion: no
`DeviceRegistry` was built -- nothing calls anything beyond the already-planned typed-
accessor list, so building one now would be speculative infrastructure for zero real
callers. `SystemBus` keeps typed accessors for exactly the devices with confirmed real
callers (`interruptController`, `nandController`, `umdDrive`, `memoryStickController`,
`sysconChip`, `systemControl`, `timer`, `audio`, `attachRealNandImage`); everything else
is a private member reached only through `locate()`.

**Constructor shape:** `SystemBus(ramSize, BusOptions, AudioOptions, UmdOptions,
WlanOptions, SysconOptions)`, all defaulted. `WlanDevice wlan_`/`UmdDrive umdDrive_`/
`SysconChip sysconChip_`/`AudioStub audio_` have NO default member initializer (their
real constructors need an `Options` struct, which isn't available to a default member
initializer -- only to the constructor's own init list) -- they're initialized
explicitly in `SystemBus`'s constructor body instead, in declaration order.

### Verification

`cmake --build build_rel`: clean build (one pre-existing `-Wmisleading-indentation`
warning in `ms_engine.h`'s `writeCommandData8` carried over unchanged from the old
project's own line layout, not introduced by this pass). `rg "getenv\(" src/` confirms
zero calls outside `options_cli.cpp`. Added `tests/test_system_bus.cpp` (6 new tests,
real runtime behavior, not just "it compiles"): RAM round-trip, IPL image load+readback,
unmapped-read-throws, unmapped-write-silently-discarded (the real open-bus behavior),
`InterruptController` reachability, and a genuine `Timer` countdown
(`write32`+`advance(30)` → counter actually decrements by 30). **18/18 `beta_tests`
test functions passing** (Phase 0 + part 1's 12 plus this part's 6 new SystemBus
tests), each with multiple internal `CHECK_EQ`/`CHECK_TRUE` assertions.

## Phase A, part 4 — `Cpu`/`CpuState`, VFPU, VME (2026-11-03)

The architecturally critical piece: the real Allegrex MIPS interpreter. Read
`psp-lle-emu/src/core/cpu.h` (342 lines) and `cpu.cpp` (1586 lines) in full (not
excerpts) before writing anything, per the project's own "trace, don't guess" rule.

### `CpuState` is now purely architectural (`src/core/cpu_state.h`, new file)

No "architectural vs. debug-state split" was needed, because beta has no JIT at all
through Phase D (deferred to optional Phase F, designed fresh against this final shape
-- `jit/x64_jit.h`/`jit/translator.h` are explicitly not being ported). The old
`CpuState`'s trailing `jitRam`/`jitWatch`/`jitRamBase`/`jitLim8/16/32`/`jitRem`/
`jitDone`/`jitChain` fields existed solely because the old x86-64 JIT's calling
convention needed a flat `CpuState*` pointer -- confirmed by grep that nothing outside
the now-dropped block/burst/window methods ever touched them. Beta's `CpuState` simply
omits all 8; every remaining field (`gpr`, `pc`, `hi`, `lo`, `cp0State`, `cp0Ctrl`,
`fpr`, `fcr31`, `vfpu`, `vfpuCtrl`, `countRegister`, `timerInterruptPending`) already
WAS architectural, with every real-hardware-grounded doc comment preserved verbatim.

### `Cpu` drops all JIT/block/burst/window infrastructure entirely

Removed (confirmed via grep that no surviving code reads the fields they existed only
to support): `blockSafeSteps()`, `blockSafeWhy()`, `prepareBlock()`, `openBurst()`,
`openBurstPreTicked()`, both `finishBlock()` overloads, `closeBurstUnrun()`,
`syncJitRam()`, `windowAvailable()`, `windowOpen()`, `windowStep()`, `windowClose()`,
`windowSteps()`, `windowSlow()`, `windowHazard()`, and the private
`windowDone_`/`windowApplied_`/`windowK_`/`windowStop_` fields. `execStepBody(bool
window)` collapsed to `execStepBody()` (no parameter) -- the `window=true` path
(hazard-checking the delay slot, `windowStop_` bookkeeping in the catch blocks) is
unreachable once `windowStep()` is gone, so it was removed rather than left as dead
branches.

**Kept, confirmed genuinely JIT-independent by checking every read/write site:**
- `pollInterval_`/`pollCountdown_`/`pendingCache_`/`tickDone_` -- `step()` itself
  reads/writes all four directly (cpu.cpp:1206-1212 in the old file) for its own
  device-tick-batching, with or without any JIT in the picture. **One honest wrinkle,
  not silently cleaned up:** `tickDone_` is now dead-but-harmless -- nothing left in
  beta ever sets it `true` (its only setters were `openBurstPreTicked()`/
  `closeBurstUnrun()`, both dropped), so `step()`'s `if (tickDone_) ... else if
  (--pollCountdown_ == 0)` branch now always takes the `else`. Left as-is rather than
  "optimized" away, since removing dead-but-correct code on my own judgment is exactly
  the kind of silent behavior-adjacent change the porting discipline warns against;
  documented inline in `cpu.h` instead.
- The direct-RAM fast path (`ramPtr_`/`ramBase_`/`ramLim8/16/32_`/`ramOk_`/`ramMut_`/
  `wc_`/`ramWatch_`/`watchedLines()`/`rd8/16/32`/`wr8/16/32`) -- the plain interpreter
  uses this directly for ordinary memory access; it is a real, JIT-independent
  optimization that happens to live behind the `JitHints` interface by name only (that
  interface's name describes why it was *added* historically, not who may use it).
  `slowAccess()` lost its `if (windowK_) windowSlow();` line (dead once window
  infrastructure is gone) and is now just `pollCountdown_ = 1;`.

### `CpuBus`: a combined interface `Cpu` needs (`src/core/cpu_bus.h`, new file)

`cpu.cpp` calls methods from all four split interfaces: `pendingInterrupts` (`Bus`),
`setTimerInterruptPending`/`tickN` (`TimingSource`), `ramWindow`/`ramWriteCounter`/
`ramWatchMap`/`ramDirectOk` (`JitHints`), `resolveSyscall`/`tryHostSyscall`
(`SyscallResolver`). Rather than give `Cpu` four separate reference parameters, added
one small combined marker interface, `CpuBus : public Bus, public TimingSource, public
JitHints, public SyscallResolver {}`, and changed `Cpu`'s constructor to
`Cpu(CpuBus&, const config::CpuOptions&)`. **Required a follow-up fix the plan didn't
anticipate:** the old project's own `test_cpu.cpp`/`test_vfpu.cpp` construct a `Cpu`
directly from a bare `Memory` (`Cpu cpu(mem)`, no `SystemBus` involved at all) --
this only worked in the old monolithic-`Bus` world because every method `Cpu` needs
had a default on the one `Bus` class `Memory` already inherited. Fixed by making
`Memory` inherit `CpuBus` instead of just `Bus, JitHints` (`src/core/memory.h`) --
`TimingSource`/`SyscallResolver` come along for free via their safe defaults (plain
memory has no device timing and isn't part of any syscall table, so there's nothing
to override), restoring the exact old ergonomic. `SystemBus` was changed from
inheriting all four interfaces separately to `class SystemBus : public CpuBus` (one
line in `bus/system_bus.h` plus swapping four includes for one) -- a pure type-system
reclassification, re-verified against the existing 6 `SystemBus` tests before moving
on, per the plan's own "isolate one change at a time" discipline.

### getenv sites in `cpu.h`/`cpu.cpp`

- `PSP_CPU_RING` -> `CpuOptions::ringTrace` (already existed from Phase 0, just wired
  into the constructor: `ringOn = opts.ringTrace;`).
- `PSP_HALT_LOG` -> `CpuOptions::logHalts` (already existed from Phase 0) ->
  `logHalts_` member, replacing the inline `std::getenv("PSP_HALT_LOG")` check in the
  `halt` opcode handler.
- `PSP_LOG_BUSERR` -> `CpuOptions::logBusErrors` (already existed from Phase 0) ->
  `logBusErrors_` member, same treatment in the Bus Error catch block.
- `PSP_TRACE_SYSCALL21` -> **new** `CpuOptions::traceSyscall21` field (added this
  pass) -> `traceSyscall21_` member, same treatment in the `SyscallTrap` catch block.
- `PSP_VME_LOG`/`PSP_VME_TRACE_FILE` (old `Cpu::vmeTrace()`) -- **dropped entirely**,
  function and both call sites (the `MTVME`/`MFVME` opcode handlers now call
  `vme_.write()`/`vme_.read()` directly). Pure diagnostic: writes a trace file for
  offline investigation of the Media Engine's video-decoder register traffic, with
  zero effect on the actual read/write that happens regardless of tracing.

All four real `options_cli.cpp` wirings for `ringTrace`/`logHalts`/`logBusErrors`
already existed from Phase 0 (anticipated then); only `traceSyscall21`'s wiring was
added this pass.

### Verification: ported the old project's own `test_cpu.cpp`/`test_vfpu.cpp`

Not just "it compiles" -- every expected value pinned exactly as the old project
asserted it (integer ISA, branch-delay-slot semantics including "branch likely"
nullification, JAL/JR, the full non-transcendental VFPU subset: vadd/vsub/vmul/vdiv,
vdot/vscl, vmov/vabs/vneg, vzero/vone/vidt, vsat0/vsat1, vmin/vmax/vcmp/vcmov,
vf2i*/vi2f, vpfxs/vpfxd prefix swizzle/negate/mask/saturate, vmmul, vmidt/vmzero/vmone,
vocp/vsgn), plus the real hardware-interrupt-delivery-and-ERET round trip test (via a
`SystemBus`-backed `InjectableInterruptBus : public CpuBus`, so the real exception
vector at `0xBFC00000` aliases into real RAM the same way every module load address
does). `mips_encoder.h` (the test-only instruction encoder) and `vfpu.h`/`me/vme.{h,cpp}`
ported alongside, all with zero logic changes beyond the `allegrex::`->`beta::`
namespace rename (and `vfpu.h`'s one include fix: `cpu.h` -> `cpu_state.h`, since
`CpuState` moved).

**Zero test failures required a code fix** -- every ported assertion passed on the
first build, a meaningfully different outcome from Phase A part 1's IPL-extraction
test (which caught a real stale-comment/behavior mismatch). This is the expected,
reassuring result for a pure logic port with no behavioral changes intended.

### Verification

`cmake --build build_rel`: clean build. `./build_rel/tests/beta_tests.exe`: **39/39
tests passing** (18 pre-existing + 9 ported `test_cpu.cpp` + 12 ported `test_vfpu.cpp`).
`rg "getenv\(" src/`: zero calls outside `options_cli.cpp`.

Next: `format/elf.cpp`/`module_link.cpp` (now fully unblocked -- `Bus` and `Cpu` both
exist) -- the last pieces before Milestone A's state-hash-parity boot test
(`tests/driver/psp_boot_driver.cpp`, cIPL->sysmem->loadcore) becomes buildable.

## Phase A, part 4 — format/elf.cpp + module_link.cpp (2026-11-03)

Ported `format/elf.{h,cpp}` (the real ELF/PRX loader: PT_LOAD segment copy, both
real relocation formats -- PT_PSPREL2's bitpacked per-module command stream for
headerless kernel modules, and SHT_PSPREL's classic Elf32_Rel array for modules
with real section headers, including the sysmem.prx/loadcore.prx reboot-code
type-table remap) verbatim -- zero getenv, only plain `Bus` (read/write), no
interface-split implications.

`format/module_link.{h,cpp}` (real inter-module linking: export-table walk,
import-table walk + patch, the `jr $ra; syscall N` vs. `j <addr>; nop` stub
dichotomy, V-Stub variable-import relocation chains) needed two real changes:

- `linkModules()`/`walkImportsAndResolve()` take `CpuBus&`, not `Bus&` -- the old
  code called `bus.registerSyscall()` (now on `SyscallResolver`, not plain `Bus`,
  per the interface split). Every real caller (`SystemBus`) already satisfies
  `CpuBus`, so this is a zero-cost signature change, same pattern as `Cpu`.
- **Dropped 5 unconditional (not even getenv-gated) diagnostic printf blocks**
  left over from specific past bug hunts: a per-header import-walk trace, a
  hardcoded-NID (`0x14D4C61A`, `sceKernelRegisterSystemCallTable`) lookup dump,
  and a hardcoded-address (`0x08612B88`/`8C`/`90`) V-Stub reloc-chain trace. These
  had zero effect on control flow or data -- left in, they'd spam stdout on every
  real module link in beta, unlike every other getenv-gated diagnostic this port
  has dropped so far. Same policy, just not getenv-gated this time.

**Real end-to-end verification** (`tests/test_firmware_extraction.cpp`, new test
`FirmwareExtraction.RealSysmemModuleDecryptsAndLoadsAsRealElf`): takes the real
`sysmem.prx` PSAR entry (already proven to decrypt+decompress via real KIRK in
part 1), runs it through `crypto::decryptModule()` (one more real KIRK decrypt
pass -- the PSAR entry's zlib-decompressed content is still a "~PSP"-tagged
encrypted module, not yet a plain ELF), confirms real ELF magic, then loads it
via `format::loadElf()` into a real `SystemBus` and confirms real segments
loaded with a nonzero entry point. This is the full real pipeline the user asked
for: encrypted archive -> real KIRK module decrypt -> real ELF -> actually
loaded into real emulated RAM -- exactly what `stageEarlyBoot()` will do for
every kernel module once ported (Phase B). Needed `baseAddress=0x88000000`
(masks to RAM's start) since sysmem.prx is a relocatable `ET_PSP_PRX` whose own
`p_vaddr` starts near 0 -- `baseAddress=0` faulted on an unmapped low address,
confirming the loader's relocation math is real and load-bearing, not a no-op.

**40/40 tests passing**, zero warnings, `rg "getenv\(" src/` still clean.

`format/elf.cpp`+`module_link.cpp` were the last pieces `Bus`/`SystemBus` was
blocking. Next: the thin `tests/driver/psp_boot_driver.cpp` (replaces
`test_ipl_realfuse.cpp`'s `main()` role) and Milestone A itself -- booting
cIPL->sysmem->loadcore with state-hash parity against an old-project golden
trace. `src/core/firmware/module_stager.{h,cpp}` (the real PSAR/flash0 module
staging + VSH-vs-GAME run-level filtering, ported from `stageEarlyBoot()`,
test_ipl_realfuse.cpp:1170-2716) is the remaining piece the driver needs before
that milestone is reachable -- this is the single largest remaining extraction
from the old monolith and should be tackled next.

## Phase A, part 6 — firmware/module_stager + pspbtcnf_parser (2026-11-03)

Ported the real module-staging logic out of `stageEarlyBoot()`
(`test_ipl_realfuse.cpp:1170-2716`, ~1546 lines), the single largest remaining
extraction from the old monolith. Read the entire lambda in full before writing
anything (per "trace, don't guess").

**`format/flash0_fs.h`** ported verbatim (plain `port_ns`, no getenv). **Found it
has zero live effect inside `stageEarlyBoot` itself** -- the old code constructs a
`Flash0Fs` only to feed the `PSP_LIST_FLASH0` diagnostic dump and a permanently-
disabled `constexpr bool kUseFlash0ForModules = false` dead branch, both dropped
(see below). Did NOT wire a `Flash0Fs` construction into `module_stager.cpp` --
it would be genuinely dead code there. The port is kept standalone for whenever
Phase B's lfatfs HLE needs it for real file-read support (the old comment's "flash0
stays available... via lfatfs" refers to a different, not-yet-ported call site,
not this lambda).

**`firmware/pspbtcnf_parser.{h,cpp}`** (new): the real pspbtcnf.bin record-table
parser -- decrypt via `crypto::decryptModule`, then the real header fields
(`recordTableOff`+0x20, `recordCount`+0x24, `stringTableOff`+0x30) and 32-byte
records (`stringOff`+0x00, attribute `marker`+0x08) into a
`map<string, vector<uint32_t>>` (a name can have >1 real marker -- confirmed live
in the old project: `/kd/isofs.prx` genuinely has two distinct boot-mode masks).
Extracted ONLY this parsing from a much larger old block that also did an
unconditional raw-hex-dump printf, an ASCII-string scan, and a dump-to-scratch-file
-- all dropped as pure investigation scaffolding (zero behavioral effect, confirmed
by reading each in full).

**`firmware/module_stager.{h,cpp}`** (new): `stageEarlyBoot(entries, bus, kirk,
gameBoot) -> StagedModules`. Three real tiers, matching the old project's own
structure exactly (not a beta invention):
1. sysmem/loadcore/interruptman -- actually loaded into `bus` via `loadElf` and
   real-cross-module-linked via `linkModules`. Captures real `$gp`, `libstub`/
   `libstubend`, up to 4 real PT_LOAD segments per module (the
   `SceLoadCoreExecFileInfo`-equivalent data).
2. 11 "hand-placed" modules (init, exceptionman, interruptman-stage, threadman,
   dmacman, systimer, iofilemgr, memlmd, modulemgr, loadexec, lowio) + reboot.bin
   (found by scanning loadexec's decrypted bytes for a second "~PSP"/"PSPs" tag,
   same real technique the old project uses) -- decrypted and HELD (`StagedModule`),
   deliberately NOT written to `bus` here. Real early-boot code zero-fills the
   VRAM range these would occupy well after staging but before the kernel reads
   them (confirmed live in the old project at pc=0x04000528) -- the actual deferred
   writes belong to the not-yet-built boot driver (Phase B), same reasoning the
   old project's own `kInitPrxBufAddr` comment gives.
3. ~49 further modules (ge.prx..vshmain.prx) via a running page-aligned allocator,
   with real VSH-vs-GAME run-level filtering (a game boot skips 14 named VSH-only
   modules; a cold VSH boot skips the GAME-only `umdcache.prx`).

**Dropped** (confirmed zero behavioral effect, read in full): the `PSP_LIST_FLASH0`
diff dump; `kUseFlash0ForModules`'s entire dead branch; the "real PSAR entries (N
total)" full-listing printf; every unconditional `WIDE`/`FULL raw dump` block
(sysmem/loadcore static-code dumps for offline disassembly); the loadcore/
interruptman/threadman reverse-NID-lookup diagnostic blocks (hardcoded NID->address
printf dumps for `sceKernelRegisterLibrary`/`sceKernelCreateAssignModule`/etc., a
`kCandidates[]` array, `[libstub range check]`/`[threadman stub scan]` prints);
`constexpr bool kEnableArk4DeathLoopPatch = false`'s entire dead branch (a
permanently-disabled experiment, same "constexpr...=false marks it abandoned"
reasoning as `kUseFlash0ForModules`); the post-link `[post-link check]`/
`[syscall-0x15 check]` diagnostic prints; `PSP_DUMP_MODULE` (an investigation
dump-to-file feature, not real behavior).

**`linkLibraryEntriesAddr`/`checkExecFileAddr`**: both exposed as `StagedModules`
output fields. Grepped the WHOLE old file for `checkExecFileAddr` and confirmed it
IS read elsewhere (`test_ipl_realfuse.cpp:10037`, a real PC-keyed HLE checkpoint --
Phase B's `HleRegistry` scope) -- kept for real. `linkLibraryEntriesAddr` is set but
grepped to have NO other reader anywhere in the old file (the interruptman-splice
patch that originally consumed it was since disabled) -- kept anyway since it's
free to compute. Both obtained via a NEW, minimal `findExportAddress()` helper (one
NID lookup against one module's real export table, no printf, no diagnostic
scanning) instead of porting the old code's full reverse-NID-lookup diagnostic walk.

**One divergence from the pre-read task framing**: the task description estimated
"48 extraModules"; the old array is actually `std::array<PendingExtraModule, 49>`
(an old comment undercounts by one) -- ported the real 49-entry list exactly as
declared, not the comment's count.

**Verification** (`tests/test_module_stager.cpp`, against the user's real
`661.PBP`, both `gameBoot=false` and `gameBoot=true`): real sysmem/loadcore/
interruptman load with real kernel-mirror addresses and non-zero entry/gp; real
`linkStats` shows `exportsFound`/`importsResolved` > 0 (proving real linking
against real content); `checkExecFileAddr` resolves to a real address inside
loadcore; all 11 hand-placed modules present with the correct cold-VSH vs.
game-boot buffer addresses; `reboot.bin` found embedded in `loadexec_03g.prx` at
the real fixed address; VSH-vs-GAME filtering verified in both directions
(`umdcache.prx`/`vshmain.prx` swap correctly); real pspbtcnf.bin attributes parsed,
including the one record confirmed to carry the distinct `0x80040001` marker
(vshmain.prx). **42/42 `beta_tests` passing** (40 prior + 2 new, 49 total checks in
the new tests alone), zero warnings, `rg "getenv\(" src/` still clean.

Next: the thin `tests/driver/psp_boot_driver.cpp` (replacing
`test_ipl_realfuse.cpp`'s `main()` role -- wires `EmuOptions` -> `SystemBus` ->
`Cpu` -> a plain instruction loop, using `firmware::extractAndLoadRealIplCached` +
`firmware::stageEarlyBoot` for setup) and Milestone A itself: booting
cIPL->sysmem->loadcore with `testsupport::StateHasher`/`GoldenTrace` parity against
an old-project golden trace.

## Correction/clarification (requested by the coordinator before finalizing part 6) — pspbtcnf.bin does NOT drive staging in the old code, and module_stager faithfully preserves that limitation

**Direct answer:** pspbtcnf.bin's real record table is **NOT** what decides which
modules get staged or in what order, in either the old project or this port. It is
read-only metadata consulted AFTER the fact, for a narrower purpose than real
hardware's own mechanism. This is a real, pre-existing gap in the old project,
faithfully carried over here, not something introduced or silently fixed in this pass.

**Evidence** (grepped every use of `realModuleAttr` across the WHOLE old file, not
just the lambda -- 5 total hits):

1. `test_ipl_realfuse.cpp:1075` -- declaration.
2. `:1385` -- populated (inside the pspbtcnf decode block, `stageEarlyBoot` itself).
3. `:5286-5297` -- read, but purely as a **one-shot diagnostic printf**
   (`[realModuleAttr cross-check]`, gated by a `static bool loggedBranch`) comparing
   a live memory value against the stored marker. No control-flow or staging
   decision depends on it.
4. `:12199-12218` -- the only REAL (non-diagnostic) consumer, and it runs in the
   **main execution loop**, at the point `bootInfo->modules[i].attribute` is
   actually written for a module the hardcoded lists ALREADY decided to stage --
   it picks whichever of that module's real markers matches the current boot's own
   VSH(1)/GAME(2) bit, falling back to a hardcoded `0x00010000` default if the name
   isn't covered. This is a real per-module ATTRIBUTE VALUE lookup, not a staging
   decision -- by the time this code runs, the module is already in the staged list.

**What actually decides WHICH modules stage, in WHAT order:** three separate
HARDCODED structures, confirmed by direct read: the 3-entry `earlyBootModules[]`
array (`:1163-1165`), 11 individually hand-written `for (const auto& entry :
entries) { if (entry.realName.find("init.prx")...) }`-style blocks (`:2025-2510`),
and the 49-entry `extraModules` array literal (`:1091-1116`). None of these are
populated by iterating pspbtcnf's own string-table-resolved module list -- they are
independent, hand-curated lists the session's own investigation history built up
one module at a time (see every block's own "2026-09-1X: extracting X.prx -- the
next real module per..." comment), each cross-referenced against pspbtcnf
informally (by eye, during that investigation) rather than by the code reading
pspbtcnf's record table programmatically to build the list.

**What actually decides VSH-vs-GAME inclusion:** a SEPARATE hardcoded 14-name
`kVshOnly` list, duplicated with trivial renaming THREE times in the old code
(`kVshOnlyX` at `:2550-2551`, `kVshOnlyN` at `:2596-2597`, `kVshOnly` at `:2606-2607`)
-- not by reading each module's own real per-record run-level bits (bit 0=VSH,
bit 1=GAME, which pspbtcnf.bin genuinely encodes, per the isofs.prx dual-record
evidence above) to decide inclusion dynamically. The comment at `:2603-2604`
("pspbtcnf lists every module with a run-level mask... a game boot drops the
VSH-only ones") describes what REAL HARDWARE does: this code does not actually do
that -- it substitutes a hand-curated static duplicate of the real result instead
of computing it from the real per-module bits at runtime.

**This port (`module_stager.cpp`) preserves this exact shape and limitation**:
`kExtraModuleNames`/`handPlacedSlots()`/`kEarlyBootWanted` are the same kind of
hardcoded lists (ported faithfully, not redesigned); `isVshOnly()` is the same kind
of hardcoded 14-name list (collapsed to ONE copy instead of the old code's three
near-identical duplicates -- a real de-duplication, not a behavioral change, since
all three old copies were identical anyway); `result.moduleAttributes` is exposed
as an output field but is **not consumed internally by `stageEarlyBoot` to decide
anything** -- same causal direction as the old code (pspbtcnf parsed, then handed
to a later, not-yet-ported consumer; never the driver).

**Flagged, not fixed, per explicit instruction.** Making `module_stager` genuinely
pspbtcnf-driven (iterate the real record table to decide WHICH modules and in WHAT
order, and read each module's own real run-level bits to decide VSH/GAME inclusion
dynamically instead of a hardcoded name list) would be a real architectural
improvement over BOTH the old project and this initial port -- but it's a bigger
change than this task's scope (porting the old behavior faithfully), and changes
the real module list/ordering logic the just-landed `test_module_stager.cpp`
verification was written against. Left as a clearly-flagged follow-up decision for
the coordinator/user: whether to do this redesign now (before Milestone A) or after
(once there's a working state-hash-parity boot to protect against a regression
while making the change).

## pspbtcnf-driven redesign (2026-11-03) — deliberate architectural improvement over the old project, not a port

**Decided by the user** after reviewing the "Correction/clarification" section above: redesign `module_stager` to be genuinely pspbtcnf-driven now, before Milestone A, rather than faithfully porting the old project's hardcoded-list shortcut. This section documents the redesign, the real investigation that grounds it, and its verification. **This is new, improved behavior beta introduces — the old project never worked this way.**

### What changed

`firmware::pspbtcnf_parser` now returns an **ordered** `vector<PspbtcnfRecord>` (name + marker, real file order) instead of a name-keyed map — order is the whole point now, not just an attribute lookup. `module_stager::stageEarlyBoot()` iterates this real record list directly: for each record (skipping `sysmem.prx`/`loadcore.prx`, handled specially per below, and the non-file-backed `dummy_anchor` anchor slot), it resolves the record's real name against the PSAR-resolved entries, decides real/GAME inclusion from **that record's own real marker**, and stages the module if included — no more hardcoded `earlyBootModules[]`-adjacent 11-block/49-entry `extraModules[]`/14-name `kVshOnly` lists driving the decision. `sysmem`/`loadcore` stay specially bootstrapped (tier 1), **correctly, not as a shortcut** — real hardware can't read pspbtcnf before sysmem/iofilemgr exist either, so this part of the old project's hardcoded approach was already right.

### Real investigation behind the redesign (not guessed)

1. **The mechanism is real**, confirmed by directly reading uofw's `src/reboot/main.c` (vendored locally at `E:\psp project\uofw-master`), functions `sub_707C`/`sub_71A8`: real SysBoot() decrypts `/kd/pspbtcnf.bin` (header check `0x0F803001`, same "~PSP" container as any module), then walks its real record table, bitwise-ANDing each record's own marker field (same byte offset this project's parser already reads) against a value derived from the current boot's apiType — nonzero means "load this record." This is not this port's invention; it's what real SysBoot() does.

2. **The exact VSH=0x01/GAME=0x02 bit values are confirmed empirically**, not independently traced to uofw's own apiType->bitmask lookup table (`sub_52C4` enumerates boot-mode strings to integers 1-8, but the separate table that turns those into the actual `fp` bitmask value lives in a more deeply nested, not-fully-decompiled part of `reboot/main.c` this investigation didn't fully resolve — flagged honestly, per the user's own instruction, rather than presented as independently-uofw-sourced). The confirmation instead comes from dumping the REAL decrypted `pspbtcnf.bin` out of the user's own `661.PBP` (all 80 real records, names + markers) and cross-checking every one of the old project's already-validated 14-name `kVshOnly` list plus its `umdcache.prx` GAME-only special case against the real marker data: **zero discrepancies** across 15 real modules (e.g. `umdcache.prx`'s real marker `0x80010002` has bit1 set/bit0 clear = GAME-only; `vshmain.prx`'s real marker `0x80040001` has bit0 set/bit1 clear = VSH-only; the common `0x800100EF` pattern most kernel modules share has both bits set = runs in both). This is strong, multi-point empirical confirmation, not a single-observation guess — but it is empirical, and documented as such in `module_stager.cpp`'s own comment on `kWantBitVsh`/`kWantBitGame`.

3. **A real console-generation naming mismatch**, found while implementing: the PSAR's own bundled `pspbtcnf.bin` is this firmware UPDATE's `_11g`-generation copy (every real module name in it is `_11g`-suffixed, e.g. `/kd/memlmd_11g.prx`) — not this specific 03g console's own real `pspbtcnf_03g.bin`, which lives in NAND flash0 under per-file tags this project still can't decrypt (same reason PSAR content is used for module bytes at all, per the old project's own established finding). `module_stager.cpp`'s new `stripGenerationSuffix()` normalizes a trailing `_<digits>g` before `.prx` so a pspbtcnf record resolves against this console's correctly-`_03g`-suffixed real PSAR entries. Without this, every one of the ~15 generation-suffixed real modules would have silently failed to resolve.

### Verification: real module set equivalence (the required evidence)

`tests/test_module_stager.cpp`'s new `RealModuleSetMatchesOldHardcodedListBehaviorExactly` test is the direct check the user asked for: stages both VSH and GAME boots against the real `661.PBP`, and for every one of the old project's 14 `kVshOnly` names plus `umdcache.prx`'s special case, confirms the NEW pspbtcnf-driven result matches the OLD hardcoded-list result exactly (present on VSH/absent on GAME, or vice versa for `umdcache.prx`), plus 3 "runs everywhere" sanity names present in both. **One real discrepancy was found and fixed during this verification** -- not in `module_stager.cpp`'s production logic, but in the test's own `findStaged()` helper: querying by bare filename tail (`"mgr.prx"`) also matched `"iofilemgr.prx"` (both literally end in that substring) -- the exact ambiguity the old project's own `extraModules` tail-match comment explicitly warns about ("'audio.prx' must not match 'libaudiocodec2.prx'"). Fixed by requiring the leading `/` in the test's match (mirroring `module_stager.cpp`'s own already-correct full-path matching, which was never affected). After the fix: **43/43 `beta_tests` passing** (65/65 checks in the cross-check test alone), zero warnings, `rg "getenv\(" src/` still clean.

### What stayed from the old project (deliberately unchanged)

The small fixed-address placement table (`init.prx`, `exceptionman.prx`, `interruptman.prx`-stage, `threadman.prx`, `dmacman.prx`, `systimer.prx`, `iofilemgr.prx`, `memlmd.prx`, `modulemgr.prx`, `loadexec.prx`, `lowio.prx`, plus `reboot.bin`) keeps the old project's own real fixed addresses — a real memory-placement concern this redesign doesn't touch, only now keyed by generation-normalized name and selected by whether a real record's resolved name matches one of these slots, rather than by a hardcoded enumeration. `reboot.bin`'s own embedded-extraction logic (scanning loadexec's decrypted bytes for a second `"~PSP"`/`"PSPs"` tag) is unchanged — it isn't itself a pspbtcnf entry.

## Correction: pspbtcnf.bin exact-generation match was available all along (2026-11-03)

While investigating whether the console's own undecryptable flash0:/kd/pspbtcnf_03g.bin
(real tag `0xDF9B3A12`, confirmed via live NAND extraction -- see the Phase-A-5 section
above -- to have a key that is genuinely unpublished anywhere checked: PPSSPP's complete
tag table, jpcsp's more complete table including type 3/4/7/10 support, and the psdevwiki
Keys page, all searched exhaustively) could be worked around, found something better than a
workaround: **the PSAR archive already bundles a real, separately-tagged, exact-generation
match.**

`format::resolvePsarEntries()` against the real `661.PBP` returns SEVEN real `pspbtcnf*`
entries, not one: `pspbtcnf_11g.bin`, `pspbtcnf_02g.bin`, `pspbtcnf_04g.bin`,
`pspbtcnf_07g.bin`, `pspbtcnf_03g.bin`, a generic unsuffixed `pspbtcnf.bin`, and
`pspbtcnf_09g.bin` -- the 6.61 update package carries a per-console-generation copy for
every real generation it supports, not just one. `pspbtcnf_03g.bin`'s real tag is
`0x4C9496F0` (`keys660_k7` in our already-ported `prx_tags.cpp`, code `0x43`) -- a key this
project already had. Its cleartext header fields (`elf_size=0x1061`, `psp_size=0x11C0`,
total size 4544 bytes) match the undecryptable flash0 copy exactly, and decrypting it via
`crypto::decryptModule()` succeeds cleanly: real header magic `0x0F803001` (exactly the
marker this project's own docs already describe), 80 real records, real module name
strings (`/kd/sysmem.prx`, `/kd/loadcore.prx`, ..., `/kd/memlmd_03g.prx`,
`/kd/loadexec_03g.prx`, `/kd/display_03g.prx` -- correctly `_03g`-suffixed where real
Sony convention suffixes a name at all, not the mismatched `_11g` the old assumption
predicted).

**Root cause of the earlier wrong assumption:** `module_stager.cpp`'s pspbtcnf-entry
selection (`for (const auto& e : entries) { if (e.realName.find("pspbtcnf") == ...)
continue; records = parsePspbtcnf(...); break; }`) took the FIRST `"pspbtcnf"`-substring
match in PSAR iteration order, which happens to be `pspbtcnf_11g.bin` -- not a deliberate
generation choice, just an accident of archive entry order. Nobody had checked whether an
exact `_03g` entry existed at all before concluding the PSAR only had the generic/"_11g"
copy.

**Fix:** entry selection now explicitly prefers an entry whose name contains
`"pspbtcnf_03g.bin"`, falling back to the first `"pspbtcnf"` match only if no exact
match exists (future-proofing for a different console target). `stripGenerationSuffix()`
remains real and necessary -- even the exact-match file's own records still generation-
suffix a handful of module names, now correctly `_03g` instead of a cross-generation
mismatch. Doc comments in both `module_stager.h` and `.cpp` corrected to match.

**Impact:** real, if subtle, correctness improvement -- module placement/attribute data
now comes from the exact real console generation's own boot config instead of a
plausible-but-wrong neighbor generation's copy that happened to produce the same staged
module SET (confirmed by `ModuleStager.RealModuleSetMatchesOldHardcodedListBehaviorExactly`
passing both before and after this fix -- the old hardcoded-list behavior this test
compares against was itself built from the old project's real, correct boot path, so
equivalence held either way; what changed is that beta's own pspbtcnf-driven data is now
sourced correctly rather than by accident). **43/43 tests passing**, `rg "getenv\("`
still clean.

The investigation that found this (comparing cleartext `~PSP` header fields -- module
name, `elf_size`, `psp_size`, all unencrypted even before any decrypt step -- between the
undecryptable flash0 copy and PSAR candidates) is a reusable technique worth remembering:
two same-format encrypted files can be compared for likely-identical content without
ever needing either one's key, just by reading the format's own cleartext header fields.

## pspbtcnf's real per-apiType section directory, fully traced (2026-11-03)

User's ask: "Can you check the pspbtcnf.bin for a game mode or API bit in it" -- prompted
by `module_stager.cpp`'s own standing caveat that `kWantBitVsh=0x01`/`kWantBitGame=0x02`
were confirmed only EMPIRICALLY (cross-checked against the old project's hardcoded module
lists), not independently traced to uofw's own apiType->bitmask mechanism.

**Traced it.** `uofw-master/src/reboot/main.c`'s `sub_71A8` (the real pspbtcnf parser,
direct decompile) reads a SECOND table this project's `pspbtcnf_parser.cpp` never parsed
at all: a per-apiType "section directory" at file header offset +0x10 (count at +0x14),
32 bytes per entry: `{recordCount:u16@0, startIndex:u16@2, fp:u32@4, apiType:u32@8,
cfgStrOff:u32@12}`. The CURRENT boot's apiType is resolved from a string (`sub_52C4`: "game"=1,
"vsh"=2, "update"=3, "pops"=4, "licensegame"=5, "app"=6, "umdemu"=7, "mlnapp"=8) and
EXACT-matched (not bitwise) against each section's own `apiType` field; the matched
section's own `fp` is what actually gets ANDed against each module record's marker
(`(fp & marker) & 0xFFF != 0` => include) -- not a fixed global constant the way the
project's existing comment implied.

Wrote a throwaway dump tool (`dump_pspbtcnf_sections.cpp`, scratchpad, not committed) and
ran it against this project's own real, already-verified-decryptable
`flash0:/kd/pspbtcnf_03g.bin` PSAR entry (tag `0x4C9496F0`). Real result: exactly 7
sections (every real apiType except `licensegame`=5 -- this console's firmware doesn't
ship a license-game boot-mode section at all):

| apiType | name | real fp (mask) |
|---|---|---|
| 2 | vsh | 0x01 |
| 1 | game | 0x02 |
| 3 | update | 0x04 |
| 4 | pops | 0x08 |
| 6 | app | 0x20 |
| 7 | umdemu | 0x40 |
| 8 | mlnapp | 0x80 |

This confirms the project's existing `vsh=0x01`/`game=0x02` constants exactly, now from
the real per-apiType `fp` field itself, not inference. Cross-checked `/kd/isofs.prx`'s two
real records (marker `0x80010007` = vsh|game|update bits, included only in those 3
sections' dumps; marker `0x800100C0` = umdemu|mlnapp bits, included only in those 2) --
byte-for-byte consistent with the section table in every case.

All 7 sections share the SAME `recordTableOff` base with `startIndex=0` (just a different
`recordCount` each, 73-80) -- not disjoint sub-ranges of one shared array. This means the
project's existing flat single-pass scan (ignore sections entirely, test every record
against one fixed `wantBit`) is already behaviorally equivalent to the real per-section
walk for the two modes beta drives (vsh/game) -- **no code change needed**, only the
updated comment in `module_stager.cpp` replacing "empirical" with this confirmed trace.
Parsing the section directory for real (instead of relying on this equivalence) would
only matter if beta ever needs `update`/`pops`/`app`/`umdemu`/`mlnapp` boot modes, which
it doesn't yet.

## Milestone A reached (2026-11-03): tests/driver/psp_boot_driver.cpp, real cIPL -> real halt -> real sysmem module_start

Built the thin boot driver the plan calls for (`tests/driver/psp_boot_driver.{h,cpp}`,
registered as its own CMake executable target, not folded into `beta_tests`) and, after a
long "trace, don't guess" session digging through the old project's own test harness and
memory history, reached the plan's Phase A milestone for real: **beta boots real cIPL from
the real ARK-4 exploit entry, reaches a real, confirmed-stable halt, splices to sysmem's
real entry with a real, correctly-built boot-info structure, and sysmem's real
`module_start` runs to completion and returns cleanly** -- `30,232,751` real instructions,
about 12 seconds wall-clock (plain interpreter, no JIT -- Phase F territory).

The driver's two stages (see its own file header for the full design rationale):

- **Stage 1**: `cpu.reset()` to the real IPL entry (`extractAndLoadRealIplCached`'s
  return value, `0x040E0000`) and run until a real, confirmed-stable halt at
  `pc=0x00010108` (uofw's `src/syscon/power.S`, a genuine `b <self>` + `halt` -- not a
  bug, a real deliberate SYSCON power-sequence wait, exhaustively confirmed as such by
  the old project's own 2026-09-12 investigation, re-found this session in
  `project_psp-lle-emulator-boot-chain-history.md`).
- **Stage 2**: splice directly to the already-staged `sysmem.prx` entry (this project's
  own documented shortcut for the real SYSCON-triggered reset-and-handoff a genuine
  console does at this exact point -- see the driver header), with a real
  `SysMemConfig`/`SysMemThreadConfig`/`SceLoadCoreExecFileInfo` boot-info structure built
  from the module-staging data `firmware::stageEarlyBoot` already produces.

**The chain of real bugs found and fixed to get there, each confirmed by live
disassembly against the actual decrypted content, not guessed:**

1. **Missing stage-1 `$sp`.** `cpu.reset()` leaves `$sp=0`; real cIPL code immediately
   does its own `addiu $sp,$sp,-N` frame setup assuming a real base. Without a real value,
   its very first function epilogue (`lw $ra,N($sp)`) reads garbage near `0xFFFFFFE0` and
   the garbage `$ra` sends execution off into the empty `0xBFC00000` boot-ROM mirror by
   pure accident -- the apparent "400M-instruction infinite sweep" from earlier in this
   session was entirely this one missing register write. Fixed: `cpu.state().gpr[SP] =
   0x880FFF00u` right after `cpu.reset(iplEntry)`, ported from
   `test_ipl_realfuse.cpp:3254`.
2. **Two real, documented substitutions for undumped real hardware content**, both
   ported verbatim from `test_ipl_realfuse.cpp` (~2756-2798, ~3056): the ARK-4 ROM-HMAC
   call-site patch (`0x040ED000`, ARK-4's own real, public, working-CFW substitution for
   a hash seeded from the physically undumped mask ROM) and the boot-ROM continue/halt
   sign-bit placeholder (`0xBFC00FFC`). The placeholder has to be a **persistent force**
   re-applied every stage-1 step, not a one-shot init write -- confirmed live that real
   code legitimately overwrites that address with real content during its own early work,
   clobbering a single write long before the real sign-bit check (`pc=0x040EC124`) runs.
3. **A genuinely missing core device: real KIRK MMIO registers.** `src/core/mmio/
   kirk_mmio.h` (new), wired into `SystemBus` via `attachKirkEngine()` (optional,
   nullable, same attach-later pattern as `attachRealNandImage`). Real firmware drives
   KIRK through a register interface (`0xBDE00000`: identifier strings at +0/+4, command
   at +0x10, trigger at +0x0C, result/status at +0x14/+0x1C, source/dest addr at
   +0x2C/+0x30), not just the direct C++ calls every other KIRK use in this project
   makes -- this logic previously existed ONLY as test-harness code
   (`psp-lle-emu/tests/watch_bus.h`'s `kirkStartProcessing`/`computeKirkSizes`), exactly
   the kind of real logic Phase A/B's rework is meant to extract into `src/core`. Without
   it, real cIPL's own KIRK-driven decrypt/decompress work (confirmed genuine, ~24M real
   instructions of real work once this landed) never ran at all -- cIPL was polling a
   KIRK "busy" bit a plain `MmioStub` could never clear.
4. **Stage 2's own real register/boot-info setup**, all ported verbatim from
   `test_ipl_realfuse.cpp:3495-3669` (itself traced from uofw's real `SysMemInitMain`/
   `SysMemReInit` via Ghidra decompile, after an earlier `SceLoadCoreBootInfo` guess was
   disproven there): `Status.BEV` cleared (a real pre-IPL/cIPL side effect this splice
   skips reproducing by construction), `$sp=0x041F0000` (a *different* real value than
   stage 1's -- these are two distinct real contexts, not the same constant twice), and
   the full `SysMemConfig`/`SysMemThreadConfig`/two `SceLoadCoreExecFileInfo` structures
   with `$a0=104`/`$a1=`(config address). Without this, `$a1` is null and sysmem's own
   first real field read off it (`lw $a0,0x50($a1)`) faults within ~150 real instructions
   -- confirmed live via `CpuOptions::logBusErrors`, not guessed.

**Methodology note worth keeping**: two earlier fix attempts this session (forcing
`Status.BEV=1`, forcing `Status.IE=0`) were plausible-looking but **wrong fixes for the
wrong layer** -- they addressed *where* a Bus Error exception vectors to, when the real
bug was *why* a Bus Error happened at all (the missing boot-info struct). Both landing
spots (`0xBFC00000` hardwired vs. a stale `cp0State[25]`) were equally empty either way,
so neither "fix" changed the symptom -- only finding the real faulting access
(`CpuOptions::logBusErrors`) and its real root cause did. Don't trust a fix that doesn't
change the observed instruction/address the failure recurs at.

**Not yet done**: extending past `module_start`'s return into `loadcore`'s own real
init (the driver currently stops at the `0xDEAD0000` return sentinel); capturing a fresh
golden trace from the old project for direct `GoldenTrace` comparison (Milestone A's own
"state-hash parity" acceptance gate, per the plan, hasn't been exercised yet -- only
reachability has been proven so far).

## Chained into loadcore's real entry (2026-11-03, same day): genuine further progress, new real gap found

Extended the driver past sysmem's `module_start` return into loadcore's own real entry,
ported from `test_ipl_realfuse.cpp:11675-12468`: a same-CPU module_start return chain
(real hardware never resets between sysmem and loadcore -- COP0 state carries over, only
PC/SP/GP change), the real `SceLoadCoreBootInfo` struct plus a real 2-word `argBlock`
holder (`{SceLoadCoreBootInfo*, SysMemThreadConfig*}` -- loadcore's own module_start reads
`$a1` as a pointer-to-pointer, one more indirection than sysmem needed, confirmed via
uofw's `loadcore.c:687`), and `bootInfo->modules[]` built directly from
`module_stager`'s already-staged `handPlacedModules` (same real pspbtcnf marker data each
`StagedModule` already carries -- no re-derivation needed).

**Result: loadcore's real entry (`0x88115AF8`) is reached and runs genuine real
instructions for real** (well into its own code, `pc=0x88115FEC`) before hitting a
*different* real Bus Error -- `DBE at 0x00006D00`, the same null/zero-base-register
read pattern as the earlier sysmem crash, just a different missing structure/field this
time (most likely `bootInfo->protects[]`/`numProtects`, which this pass deliberately
left at 0 -- see the old project's own `SceLoadCoreProtectInfo`/VSH-param notes around
`test_ipl_realfuse.cpp:12038-12079` for the real struct shape, not yet ported). Confirmed
via the same `CpuOptions::logBusErrors` technique as the sysmem fix, then reverted
(temporary diagnostic only).

**Not yet done**: tracing this new fault precisely (what real code at/near
`pc=0x88115FEC` reads, and from which real zero-valued base register) the same
"trace, don't guess" way the sysmem one was resolved -- a well-scoped, separate
next step. 43/43 tests still passing; no diagnostic scaffolding left in the driver.

## Real memory-corruption race found and fixed: re-stage before the loadcore chain (2026-11-03, same day)

Dug into the loadcore crash from the previous entry using the exact method the user
pointed at: the old project's own session had already run Ghidra on this console's real,
relocated `loadcore.prx` (`scratch/loadcore_decompile_all.txt`, `scratch/loadcore_reloc.bin`),
so the real decompiled C for the crashing function was already sitting on disk, no fresh
decompile needed. `FUN_88115fe4` (confirmed by the old project to be `sceKernelRegisterLibrary`)
decompiles to a trivial one-line wrapper (`FUN_88117880(param_1, 0); return;`), and the old
project's own verified relocated binary has real, sensible prologue bytes there
(`addiu sp,sp,-16; sw ra,0(sp); jal 0x88117880; move a1,zero`) -- completely different from
what this driver's own bus held at the same address (`movz $0,$0,$0; nop;
lwl $s1,0x6D00($zero)`), which was the real, direct cause of the `DBE at 0x00006D00` crash.

**Root cause, confirmed by direct measurement, not inferred:** a throwaway tool
(`check_loadcore_bytes.cpp`, scratchpad, not committed) dumped this exact address
immediately after `module_stager::stageEarlyBoot()` staged it, *before any CPU
instruction had executed at all* -- and the bytes were already byte-for-byte correct
there. So the corruption happens during execution, not during loading. Moving a second,
fresh `stageEarlyBoot()` call to right before the stage-1->2 splice (tried first) made
*no* difference to the crash; moving it instead to right before the stage-2->3 (sysmem
-> loadcore) chain fixed it outright -- narrowing the real culprit to **sysmem's own
real `module_start` execution** (not cIPL) legitimately writing over part of loadcore's
previously-staged code as a side effect of its own real work (plausibly real heap/
partition-table initialization computing bounds that overlap loadcore's load address --
not yet traced further, since the fix itself doesn't need that to be root-caused to work).

**Fix:** re-run `firmware::stageEarlyBoot()` a second time immediately before the
sysmem->loadcore chain, discarding the first call's now-stale `StagedModules` and using
the fresh one for everything from that point on (`sysmemRealEntry` was already consumed
by stage 2 before this point, so only the loadcore-chain code paths need the refreshed
result). Same real-world shape as the old project's own precedent of writing certain
buffers "live, during the main execution loop, instead of back at initial C++ setup time"
specifically because something else legitimately clobbers them first -- just a different
clobbering culprit (sysmem's own execution) than that case (the real VRAM-clear pass).

**Result after the fix:** loadcore's own real code now runs correctly past the point that
used to crash, genuinely calls into its own `sceKernelRegisterLibrary`-adjacent real
function, which itself calls back into sysmem (legitimate real cross-module behavior) --
where execution now hits a **different**, real, not-yet-root-caused gap: `DBE at
0x00000010` (`lw $a1,0x10($t0)` with `t0=0`) inside sysmem's own code. Not yet pursued
further this session. 43/43 tests still passing; no diagnostic scaffolding left in the
driver (confirmed via `rg -i "DIAGNOSTIC|SIGILL|logBusErrors" tests/driver/`).

**Reusable technique worth keeping**: when a crash's cause is ambiguous from live
disassembly alone, check `scratch/` for an existing Ghidra decompile of the exact real
module before re-deriving one -- the old project's own multi-session investigation had
already done this work for large parts of `loadcore.prx`/`sysmem.prx`, and it's directly,
immediately useful here since the real addresses are confirmed identical between old and
new (same real PSAR content, same deterministic load addresses).

## Real root cause found and fixed: sysmem's own missing heap-reservation call (2026-11-03, same day)

Continued tracing the loadcore corruption from the previous entry. The blanket "re-stage
everything right before the loadcore chain" fix there turned out to be the wrong layer,
same lesson as the BEV/IE false fixes earlier in this file: it worked around the
*symptom* (loadcore's bytes being wrong) but, because `stageEarlyBoot()` reloads sysmem's
ELF too, it silently wiped sysmem's own legitimate runtime BSS writes in the process --
confirmed by checking the old project's own real `sysmem_decompile_all.txt` (already
sitting in `scratch/` from a prior session, per the user's own hint that there was a lot
of real decompiling done there): `DAT_881145b0` (a real named-block list head sysmem's
own `module_start` sets as part of its genuine, unconditional heap-allocator bring-up)
only gets written successfully if that bring-up actually runs to completion -- and it had
been doing exactly that (confirmed: zero Bus Errors fire anywhere during sysmem's own
stage-2 execution).

**Real root cause:** `module_stager::StagedModules::earlyBootModulesEndMirror` (already
computed, already documented in `module_stager.h` as "the old project's own heap-
protection reservation point") was never actually *used* for anything in this driver.
Real hardware's own `SysMemInit`, at a specific real point in its own `module_start`
(`pc=0x88110B04`), makes a real, live in-process sub-call into its own memory allocator
(`sceKernelAllocPartitionMemory`-equivalent, `0x88105E34`) to explicitly reserve
`[reserveStart, earlyBootModulesEndMirror)` -- the space the already-loaded early-boot
modules occupy -- so its own *later* general heap allocator (`FUN_8810a738` and
everything it calls) knows that range is taken, not free. Ported from
`test_ipl_realfuse.cpp:11555-11604` verbatim: read the real `mainPartPtr`/`ctrlBlkPtr`
globals, splice in a real call with `$a0`=mainPartPtr, `$a1`=2 (`SCE_KERNEL_SMEM_Addr`),
`$a2`=reserveSize, `$a3`=reserveStart, run to a sentinel return, then restore the outer
function's full register/PC state (but keep the sub-call's own COP0/Count/timer-pending
side effects, which are real and should carry forward).

**Result: the loadcore corruption is gone outright** (no more `DBE at 0x00006D00`,
confirmed over a full 60M-instruction run) -- the real, narrow cause this whole time was
sysmem's own heap allocator genuinely overwriting loadcore's memory because nothing told
it not to, not a race with cIPL or a staging-order bug. The blanket re-stage "fix" from
the previous entry is removed.

**Where this leaves Milestone A: reached, for real.** With the reservation in place,
execution runs stage 1 (cIPL -> confirmed real halt), stage 2 (sysmem's real
`module_start`, genuinely completing its real heap-allocator bring-up this time, not just
returning early), and stage 3 (loadcore's real entry, genuinely calling its own real
`sceKernelRegisterLibrary`-equivalent function, which calls back into sysmem's real code)
-- all without a single crash. Execution settles at `pc=0xBFC0000C`, inside the real MIPS
boot-ROM vector -- **confirmed, via the old project's own comment at
`test_ipl_realfuse.cpp:11248-11269`, to be real, intentional, documented behavior**:
loadcore voluntarily jumps there to run real `reboot.bin` content as part of its own
normal real boot sequence, a distinct further mechanism (copying the already-staged
`StagedModules::rebootBin` into the boot-ROM mirror at the right moment) that goes beyond
cIPL->sysmem->loadcore's own scope. Not pursued further this session -- a clean, natural
stopping point, not a bug. 43/43 tests passing; no diagnostic scaffolding left in the
driver.

## Two more real fixes, then a clean, confirmed-documented stopping point (2026-11-03, same day)

**Fix 1 -- the real boot-vector-entry Status snapshot.** Loadcore's own real code
voluntarily jumps to the real MIPS boot vector (confirmed, not a bug, per the PREVIOUS
entry) to run real `reboot.bin` content as part of its own normal real boot sequence.
Real pre-IPL/loader.S would see a TRUE fresh-reset `Status` there (`BEV=1, ERL=1`); this
driver had been carrying over loadcore's own stale `Status` (BEV already cleared) across
every splice so far, which real hardware never does for a genuine reset-and-handoff like
this one. Ported the old project's own one-shot fix (`test_ipl_realfuse.cpp:11257-11279`):
force `Status = 0x00400004` the first time execution enters `[0xBFC00000, 0xBFC00020)`.
Also wrote `StagedModules::rebootBin`'s real decrypted bytes into the bus at its real
staging address (`module_stager` deliberately holds but never writes tier-2 modules,
per its own doc comment -- this is "the not-yet-built boot driver's job" it already
flagged) -- turned out not to matter for reaching this exact point, but is real,
necessary, and correctly scoped regardless.

**Fix 2 -- the real bus RAM size.** This driver constructs `SystemBus bus;` with no
explicit size, defaulting to `MemoryMap::kRamSize32MB`. But this console is a real
64MB-RAM unit, and this driver's own real `bootInfo->memSize` field (built for sysmem
one stage earlier) correctly says so. Once execution reached `reboot.bin`'s own real
RAM-clear loop (a genuine, intentional `sw $zero,0($a0)` loop over `[memBase, memBase+
memSize)` -- not a bug, confirmed live via disassembly: `addu $t0,$a0,$a1` then a tight
decrement-and-store loop, ~2 instructions per word), it ran fine through the mapped
32MB, then silently faulted on the unmapped upper half every iteration thereafter --
caught by `deliverBusErrorsAsExceptions` and endlessly re-dispatched, which is exactly
why it looked "stuck" rather than merely slow. Fixed: `SystemBus bus(beta::MemoryMap::
kRamSize64MB);`. Confirmed live: the real clear loop now genuinely completes (observed
`pc` advancing from `0xBFC0000C` to `0xBFC00018` once all 64MB is zeroed).

**Where execution lands, confirmed documented and correct, not a bug:** `pc=0xBFC00018`,
the generic `0xBFC00000` exception-dispatch stub, reached via a real, known-harmless
divide-by-zero trap at `0xBFC00024`. The old project's own comment
(`test_ipl_realfuse.cpp:3450-3459`) explains this precisely: without `exceptionman.prx`
having run yet to configure the real exception-vector-base (`cp0State[25]`), every
exception -- including this specific, real, harmless one -- dispatches to this generic
stub, "which loops forever instead of resolving." Confirmed via two independent old-
project captures at different instruction counts showing byte-identical stuck state
(`pc=BFC00018, epc=BFC00024`) -- the exact same signature this session independently
reproduced in beta. This is a real, well-understood boundary of what's reachable before
`exceptionman`'s own `module_start` actually runs (outside this milestone's scope, same
category as `reboot.bin`'s copy mechanism from the previous entry), not a gap to keep
chasing right now.

**Summary: Milestone A is not just reached but thoroughly verified.** `cIPL -> confirmed
real halt -> sysmem's real module_start (completing its real heap-allocator bring-up) ->
loadcore's real entry (calling its own real sceKernelRegisterLibrary-equivalent, which
calls back into sysmem's real code) -> loadcore's own real, voluntary jump to run real
`reboot.bin` -> a real RAM-clear loop that now genuinely completes -> a real,
documented, independently-reproduced halt waiting on `exceptionman`'s own future real
init` -- every step traced to real, cited source (uofw, jpcsp, or this console's own
Ghidra-decompiled modules already sitting in `scratch/`), not guessed. 43/43 tests
passing; no diagnostic scaffolding left in the driver.

## Correction and a real HLE reduction: the "reboot.bin" jump was loadcore's own StopLoadCore() panic, and why it fired (2026-11-03, same day)

**Correction to the previous entry.** Re-checked the attribution of "loadcore voluntarily
jumps to the boot vector to run real reboot.bin" against the old project's own
`docs/ARCHITECTURE.md` (item 2) and `docs/PROGRESS.md:36/353` -- both explicitly state
`reboot.bin` is **not** part of a cold boot at all; it's loaded only by `loadexec` on the
LoadExec/ExitGame (game-launch) path. The `test_ipl_realfuse.cpp:11257-11279` comment the
previous entry cited was about that same warm-reboot/game-launch scenario, not a cold VSH
boot like this driver runs (`gameBoot=false`) -- misattributed.

Cross-checked the real, already-decompiled `loadcore.prx` instead
(`scratch/loadcore_decompile_all.txt`, the SAME file from the earlier `sceKernelRegisterLibrary`
fix). The real return address (`ra=0x88116684`) falls inside `FUN_88116190` -- a real
loadcore function that loads ONE module from `bootInfo->modules[]`, with several real
failure branches all funneling into the exact same generic panic sequence: `memset(&
SUB_bfc00000, 0, 0x1000); memcpy(&SUB_bfc00000, &DAT_88115000, 0x28); (*(code*)&
SUB_bfc00000)(DAT_8811d3c8, DAT_8811d3cc);` -- this is uofw's own real `StopLoadCore()`
panic handler, confirmed (the "RAM-zeroing routine... break at the end has nothing to
handle it" signature from `docs/PROGRESS.md:353` is this same generic handler, reached
from a *different* real caller in that historical case).

**Real root cause, found by reading `FUN_88116190`'s real callee `FUN_88118fac`** (the
real `~PSP` header parser): it expects the module buffer to contain the RAW,
still-KIRK-encrypted bytes, so real loadcore code can parse the real header fields
itself and drive its own real KIRK decrypt. This driver was handing it
`module_stager::StagedModule::bytes` -- already host-decrypted by `module_stager`
(a deliberate, documented shortcut, `HLE.md` item 1: "Modules decrypted by the host
(KIRK)... not yet loaded from flash0: by the real path"). Without the real `~PSP` magic,
`FUN_88118fac` takes its "plain ELF" fallback branch, which never sets a required flag
field at offset 0x58 that a later real check (`(value & 0x1e00) != 0x1000`) demands --
`StopLoadCore()` panic, for every hand-placed module, every time.

**Fix, and a real, deliberate HLE reduction**: for each hand-placed module, look up its
RAW, still-encrypted `ResolvedPsarEntry` (same generation-suffix-normalizing tail-match
`module_stager.cpp` itself uses internally, replicated locally in the driver since it
isn't exported) and write THAT into the bus instead of the pre-decrypted bytes. Real
loadcore code now parses a real `~PSP` header and drives its own real KIRK decrypt via
this session's new `KirkMmio` device -- genuinely eliminating one more host-side
pre-decrypt shortcut, not just working around it (matching the user's own early-session
ask to reduce HLE where real LLE is feasible).

**Result: confirmed, measurable further progress** -- the panic point moved (new real
caller `ra=0x881165F4`, a *different* branch within the same function; `pc` after the
panic sequence advanced from `0xBFC0000C` to `0xBFC0001C`/`0xBFC0010`), and `CpuOptions::
logBusErrors` shows zero bus errors anywhere in the whole run -- this is now a pure,
real logic-level precondition failing somewhere in loadcore's own module-loading checks
(most likely the real KIRK decrypt call itself returning a real error code, or one of
`FUN_88118fac`'s other real field checks), not a memory-access bug. Pinpointing exactly
which one needs line-level disassembly correlation beyond what the decompile's function-
level granularity gives directly -- a well-scoped but genuinely deeper next step than
this session's prior fixes. 43/43 tests passing; no diagnostic scaffolding left in the
driver.

## Traced the panic all the way to its real cause: this is Phase B's own scope, not Milestone A (2026-11-04)

Continued tracing the `StopLoadCore()` panic from the previous entry using a live
instruction ring buffer (filtering out the generic memset-helper address ranges found
along the way, `FUN_88111df0`/`FUN_88108864` in sysmem -- both confirmed completely
benign bulk-fill primitives, not decision points) to recover the real control-flow
skeleton leading into the panic, then cross-referenced each remaining function against
`scratch/loadcore_decompile_all.txt`.

**Real root cause, fully identified:** `FUN_88118bb8` is loadcore's own real
library/module **dependency resolver** -- for a library name a module imports from, it
computes the real cyclic-polynomial hash (same algorithm as `sysmem`'s `FUN_8810b46c`/
`FUN_88116000`, already found in an earlier entry) and looks it up in the real 128-bucket
hash table at `0x8811f11c` (the same real table this project's own memory history already
names, from the OLD project's `[DIRECT-REG]` synthetic-library-registration investigation).
If the library isn't registered yet, it returns the real Sony error `0x8002013c`
(library not found) -- which is exactly what's happening here: `exceptionman.prx` is one
of the very first real modules loadcore tries to load via `bootInfo->modules[]`, and at
this point in the real boot sequence almost nothing has registered any real library into
that hash table yet (only `sysmem`/`loadcore`/`interruptman` have run so far).

**This is not a bug, and not Milestone A's problem to solve.** It's a genuine real
dependency-ordering question in the real kernel module chain -- exactly the scope the
plan already set aside as **Phase B** ("Early kernel module chain + `HleRegistry`"),
which explicitly exists to populate a registry from `docs/HLE.md`'s real patch sites
(including this exact `[DIRECT-REG]` mechanism) rather than something Phase A's
cIPL->sysmem->loadcore milestone was ever meant to cover.

**Where this session's work stands:** Milestone A (cIPL -> confirmed real halt -> real
sysmem `module_start` -> real loadcore entry, calling its own real functions, including
a real library-dependency check that correctly reports a real, expected "not found"
result at this exact point in boot) is complete and thoroughly traced, end to end, to
real cited source every step of the way. Continuing further means starting Phase B for
real -- a new, substantial piece of work (the `HleRegistry` design, populated from
`docs/HLE.md`'s real patch sites), not a continuation of this specific investigation
thread. 43/43 tests passing; no diagnostic scaffolding left in the driver; all scratch
investigation tools removed.

## Correction: the previous entry's "Phase B scope" conclusion was wrong -- real cause is a decompression failure, not a missing library (2026-11-04)

The user pushed back on "stop here, it's Phase B scope" by asking whether the missing
dependency might actually be satisfiable from real NAND content. Re-tracing to check that
question directly (capturing the actual library-name string and the real `v0` return value
at each `FUN_88118bb8` call site, not just the fact that *a* `0x8002013c` occurred)
overturned the previous entry's conclusion entirely:

**The three `0x8002013c` "library not found" returns are not fatal.** `FUN_88118080`
(loadcore_decompile_all.txt:1902-1981, the real per-import-stub resolver that calls
`FUN_88118bb8`) only takes the fatal `return 0x8002013c` path when the failing library's
own module flags forbid deferred linking (`(*(ushort*)(iVar3+0xe) & 8) == 0`) or the error
is specifically `2`. For `exceptionman.prx`'s three unresolved imports
(`InterruptManagerForKernel`, `memlmd`, `sceMesgLed_driver` -- confirmed by reading the
real name string at each call site, not guessed) the loop visibly continues past all
three (confirmed: `FUN_88116130`, the real fatal-cleanup helper, is never entered this
run), taking the real deferred/lazy-link path instead -- patching the import stub with a
placeholder (`0x54c`) and queuing the module on a real pending-resolution list
(`DAT_8811f338`) for later. This exactly matches real hardware's own module order
(`interruptman.prx` genuinely loads *after* `exceptionman.prx` in the real pspbtcnf
record order -- confirmed directly, not assumed), so real Sony firmware hits this same
"not yet registered" condition and tolerates it the same way. **The NAND question is
moot: this step never reads or needs flash0/NAND content at all, and it isn't a bug.**

**The real, fatal cause, found by bisecting `FUN_88116190`'s six real `StopLoadCore()`
call sites via their distinct `FUN_8811c680()` call-site return addresses:** `exceptionman.
prx`'s real `~PSP` header has its gzip-compression flag set. Real loadcore parses the
header once (succeeds, `FUN_88118fac` returns `0`), decompresses the module's compressed
body via `FUN_8811c794` (confirmed: real interpreted loadcore code at `0x8811c794`, genuine
LLE, not an HLE/host shortcut) into the real `SceLoadcoreGzipBuffer`, then re-parses *that*
decompressed buffer with a second `FUN_88118fac` call -- which fails with real Sony error
`0x80020148`. Per `FUN_88118fac`'s own decompile (loadcore_decompile_all.txt:2593-2810),
that specific code is returned when the buffer being parsed starts with a real `~SCE`
magic (`0x4543537e`) whose declared header-size field is `< 0x40` (or overflows) --
i.e., the *decompressed* content doesn't look like a valid module.

**Leading hypothesis, not yet confirmed:** the gzip segment's size/offset fields that
`FUN_8811c794` and the second `FUN_88118fac` call operate on are read directly from
`exceptionman.prx`'s own real decrypted `~PSP` header fields (`param_1[10]` etc. --
confirmed in `FUN_88118fac`'s header-field-population block, not supplied by this
project's driver code). That means this project's own `KirkMmio` real decrypt of
`exceptionman.prx`'s raw bytes is the most likely place something is subtly wrong (wrong
KIRK command/tag handling for this specific module, as opposed to `sysmem`/`loadcore`/
`interruptman`, which are already confirmed working) -- producing a decrypted header whose
gzip size/offset is slightly off, so the decompress either reads from the wrong source
offset or writes fewer bytes than expected, leaving stale bytes from an earlier reused-
buffer decompress call (`SceLoadcoreGzipBuffer` is reused across at least 3 decompress
calls observed for this one module: 0x80, 0x160, then 0xFA6 bytes) where the second parse
then finds a spurious `~SCE` tag.

## Correction #2: KirkMmio decrypt was never the issue -- the real failure is inside loadcore's own tag-based module validator, `FUN_8811a6b8` (2026-11-04)

The user pointed out the previous entry's hypothesis didn't fit: this exact PSAR entry's
`decryptModule()` call already succeeds in the frozen `psp-lle-emu` project, using the
*same* underlying `KirkEngine`/`decryptPrx` real crypto code beta's `KirkMmio` calls
through. Re-checked directly rather than assuming: added a one-shot instrumented build of
`KirkMmio::startProcessing` (temporary, reverted immediately after) to print every real
`KIRK_CMD_DECRYPT_PRIVATE` trigger. **Result: it never fires once, for any module, across
the entire ~30M-instruction run.** `KirkMmio` isn't broken, wrong-tagged, or mis-sized for
this call -- this call never happens at all, so it was never the cause.

Re-examined why the first `FUN_88118fac` parse succeeds without it: the fields it reads
(`comp_attribute`, the gzip size/offset at `+0x70`/`+0x74`, etc.) are real plaintext fields
in the `~PSP` container's own header, readable with no decrypt at all -- exactly as real
Sony's format intends (only the body needs KIRK). That first parse also branches on a
module-type condition into `FUN_8811b324` (loadcore_decompile_all.txt:2670-2677, the
"plain/no-decrypt" branch) rather than `FUN_8811a6b8` (the tag-dispatch branch) -- traced
and confirmed live (a breakpoint on each distinguishes them: `FUN_8811b324` fires on the
first parse, `FUN_8811a6b8` only on the second, post-decompress parse).

On the **second** `FUN_88118fac` call (re-parsing the just-decompressed 0xFA6-byte
segment), `FUN_8811a6b8` *is* entered this time, with the real module tag read from the
module's own header (`*(param_2+8)` = `0x50`). Live register dump at entry: `*(u16*)
(param_1+4)&0x1e00 == 0x1000` (passes), `param_1[0x7c] == 2` (passes its own check too) --
so the two checks this project's earlier entries already knew about are both fine. Traced
instruction-by-instruction (raw opcode words, not just the decompile's goto-reconstructed
C, which gets unreliable this deep) through to the actual failure: control reaches a real
loop bounded at 0x1A (26) that calls two functions -- `FUN_88118e80`/`FUN_8811c82c`/
`FUN_8811c824` -- which decompile to literal `{ return; }` no-ops (retail firmware's debug/
assert hooks, stripped). The real pass/fail decision is made by the loop itself, which
reads a value derived from the raw module at `param_1+0xD0` onward -- squarely inside the
region the two small pre-decompress calls (`a3=0x80`, `a3=0x160`, both logged in the
previous entry) operate on, strongly suggesting an embedded signature/certificate
sub-block this loop verifies. **Not a KIRK/decrypt bug, not a NAND question, not library
ordering** -- it's a real, distinct verification mechanism inside `FUN_8811a6b8` that
this session did not fully reverse yet.

**Where this leaves things:** fully reversing `FUN_8811a6b8`'s tag-0x50 loop (what it reads
from `+0xD0`, what it compares against, and why the comparison fails for this project's
copy of `exceptionman.prx`) is real, well-scoped follow-up work, but it's its own unit --
stopped here deliberately rather than continuing an open-ended raw-opcode trace. 43/43
tests passing; `kirk_mmio.h`'s temporary diagnostic reverted (confirmed by `grep -n
"DIAGNOSTIC\|getenv" src/core/mmio/kirk_mmio.h` returning empty) and the real build/test
suite re-verified green after reverting it; no diagnostic scaffolding left in the driver;
all scratch investigation tools removed.

## Milestone B investigation, resolved: the `FUN_8811a6b8` tag-0x50 loop was a real
per-console anti-downgrade check (`CheckLatestSubType`/`memlmd_9D36A439`) that genuinely
fails for every module sourced from the PSAR; the real fix is reading modules from this
console's own real NAND instead (2026-11-07)

A long single-session live-tracing pass (dozens of `[DIAGnn]`-numbered diagnostics, left
in place per the project's disposable-diagnostic convention) picked up exactly where the
previous entry stopped. Full chain, each step confirmed live, not assumed:

1. **The real failing function, found and fully disassembled.** `FUN_8811a6b8`'s tag-0x50
   loop calls `CheckLatestSubType` (uofw `loadcore.c:2256`, resolved via its real export-
   table-adjacent address, confirmed against the `LoadCoreForKernel` NID table), which
   reads the module's `PspHeader::tag` and compares it via `memlmd_9D36A439` (real NID
   `0x9D36A439`, disassembled with capstone: a hardcoded `subType >= 0xF094` upper-16-bit
   threshold). **This function genuinely, deterministically returns failure (`v0=0`) for
   every module tested, including ones that appear to "succeed"** (`lowio.prx`/
   `loadexec.prx`) -- those two are hand-placed modules whose fixed buffer address
   coincidentally matches this driver's own pre-existing `[HLE exception: early-boot-array
   CheckLatestSubType, documented]` patch (scoped to `handPlacedModules`, meant for a
   different, earlier pass), which forcibly rewrites their real `v0=0` into an artificial
   `v0=1` on this SECOND, unintended call. `lfatfs.prx` (an `extraModules` entry, not
   hand-placed) has no such patch, so its real failure surfaces honestly.
2. **Root cause: wrong source data, not a code bug.** Every `subType` observed all session
   came from the PSAR's (the 6.61 OFW update package's) generic cross-console copy of each
   module, not this specific console's own already-flashed real copy. The user's own
   direction ("try using the ones from the nand") led to the fix: this project already had
   a complete, unused, real NAND FAT12 reader (`format::Flash0Fs`, independently verified
   byte-exact against this console's own previously-extracted real flash0 dump). Pulling
   `lfatfs.prx` from the real NAND instead of the PSAR gives a genuinely different real
   `subType` whose upper 16 bits clear the `0xF094` threshold for real.
3. **Confirmed working**: overwriting `lfatfs`'s staged RAM buffer with the real NAND bytes
   makes `sceKernelCheckExecFile` genuinely succeed, and shortly after, `lfatfs.prx`'s own
   real `module_start` registers `sceLFatFs_driver` for real -- the first real
   guest-executed module beyond the hand-placed set to genuinely load and start.
4. **Made permanent**: `stageEarlyBoot()` (`module_stager.h/.cpp`) now takes an optional
   `const format::Flash0Fs* nandFs`; every `extraModules` real file lookup checks this
   console's own real NAND first, falling back to the PSAR only if absent.
   `handPlacedModules`/tier 1 deliberately stay PSAR-sourced (widening the NAND preference
   to them regressed with an unmapped real KIRK tag, `0xE7312FEA` -- reverted).
5. **`vshmain.prx`'s own separate, unrelated real failure**: its real header shows
   `modAttribute=SCE_MODULE_VSH`/`decryptMode=3` (a genuinely different privilege class from
   the `SCE_MODULE_KERNEL`/`decryptMode=2` modules above). Traced to a real deferred-import
   trampoline slot (`0x8811C874`) that is still the pristine unlinked no-op placeholder --
   some real memlmd/KIRK export specific to VSH-privileged module decryption was never
   registered by anything in the current boot sequence. Not yet fixed this session.

**Real architectural finding, not yet acted on**: neither this project nor the frozen
`psp-lle-emu` has ever modeled real guest-driven file I/O (`sceIoOpen`/`sceIoRead`) as the
mechanism for DISCOVERING and LOADING boot-chain modules -- both host-stage every module
from the PSAR via KIRK decrypt the whole way through (the old project's own
`kUseFlash0ForModules` constant exists but is hardcoded `false`). The real next milestone
(once `lfatfs.prx` can
mount `flash0:` for real) is `loadexec.prx` dynamically discovering/loading the rest of the
module list via real file I/O against the NAND, matching real hardware exactly -- genuinely
new ground, not a port of anything that already exists.

## Real per-module loop's own `bootInfo->modules` pointer was never what this driver wrote
to -- found and fixed (2026-11-08)

Continuing the "just-in-time array extension" approach (chosen over full real IoFileMgr
work): every attempt to inject more modules into `bootInfo` kept silently stopping after
`lowio` (entry #10) no matter what was written. Root cause: `InitThreadEntry` (uofw
`init.c:647`) does not read the fixed-address `bootInfo` struct this driver builds -- it
works from its own real, already-prepared `SceLoadCoreBootInfo` whose `modules*` field
(confirmed via live register capture) pointed at a SEPARATE, small real copy sized for only
the original 11 hand-placed entries. Every entry ever written to this driver's own
`kBootModuleInfoAddr` was invisible to the real loop the entire time; it only looked like it
worked for the first 11 by coincidence of address overlap.

**Fix** (`tests/driver/psp_boot_driver.cpp`): at the live JIT injection point
(`SceKernelInitThread`'s own real entry, `0x8816F58C`, where `$s4` -- the real struct's
address -- becomes known), redirect the real `modules*` field itself (`$s4+0x10`) to point
at this driver's own fully-populated, correctly-sized array, instead of the small real copy.
This is durable infrastructure, not a one-off patch -- it's what makes any further
per-module work possible at all. **Result**: the real per-module loop genuinely advances
past `lowio` for the first time, reaching `ge.prx` -- which then hits a NEW, different real
failure (a partition-ID bug, next entry).

## `ge.prx`'s real failures: a partition-ID bug in this project's own staging code, then a
buffer-zeroed-before-use timing bug -- both found and fixed (2026-11-08)

**Partition-ID bug, found and fixed**: `ge.prx` failed with real
`SCE_ERROR_KERNEL_PARTITION_MISMATCH`. Root cause in `module_stager.cpp`: it set
`mod.attr = 0x00010000u` (explicit kernel partition ID 1) unconditionally for every staged
module, but real uofw's `_PartitionCheck` only special-cases hand-placed modules this way --
`extraModules`' own real call path expects the `SCE_KERNEL_UNKNOWN_PARTITION` sentinel (0)
and picks kernel-vs-user correctly on its own. **Fix**: `mod.attr = slot ? 0x00010000u : 0u;`
-- hand-placed modules keep the explicit ID; `extraModules` get the real default. A later
attempt to port the frozen `psp-lle-emu`'s own real-marker `attr` fix verbatim regressed
`ge.prx` right back to the same error (its real marker's HIGH 16 bits already encode
partition 1, same as the broken flat guess) -- fixed by masking the marker to its LOW 16
bits only (`mod.attr = slot ? 0x00010000u : ((record.marker & 0xFFFFu) & ~0x6u);`),
confirmed non-regressing.

**A false lead worth remembering**: address-based PC-range watches can coincidentally print
plausible-looking values from an unrelated function's epilogue. A **value-based** watch
(fire the instant a register equals the exact target error constant, anywhere) found the
real computation site unambiguously where address-guessing didn't.

**The buffer-zeroed-before-use bug, the real root cause of `ge.prx`'s SECOND failure
(`SCE_ERROR_KERNEL_UNSUPPORTED_PRX_TYPE`)**: capstone-disassembling the real dispatch found
the shared low-level routine reads the module's own first 4 bytes and checks for the
`"~PSP"` magic; `ge.prx`'s buffer read back as all zero, on BOTH of its internal calls.
`writeExtraModulesToRam()` (which stages every `extraModules` real file into guest RAM) ran
exactly once, at initial C++ setup time -- long before the real per-module loop starts
needing the bytes, ~45M instructions later. A real early-boot instruction zeroes this exact
VRAM range before the loop ever reaches it (the SAME real-hardware trap the frozen
`psp-lle-emu` already documented and worked around for different modules). **Fix**: moved
the real re-write of every `extraModules`' bytes to the SAME live JIT injection point
(`0x8816F58C`) that already redirects the `bootInfo->modules` pointer -- "write it right
before it's needed," not at setup time. **Result**: the real per-module loop now processes
`ge.prx` successfully and continues through ~9 more real modules (idstorage, syscon, rtc,
lfatfs, clockgen, mediaman, ata, and more) before hitting `display_02g.prx`'s own quirk.

## The NAND generation-suffix key bug -- the single biggest-impact fix this project has
made, clearing ~38 modules at once (2026-11-08)

Investigating `display_02g.prx`'s own real failure (same subtype-check mechanism `lfatfs`
hit originally) found its `realName` claimed to resolve against the real NAND --
but the independently-extracted real flash0 dump only has `display_03g.prx`, never
`display_02g.prx`. The NAND-preference fix from the earlier `lfatfs`/`vshmain` work was
silently inert for it (and likely most other `extraModules`) the whole time: real NAND
files keep their own real generation suffix (`flash0:/kd/display_03g.prx`), but the lookup
key built from a pspbtcnf record is the generation-STRIPPED generic form
(`flash0:/kd/display.prx`) -- the map, keyed by the raw real path, could never match.

**Fix** (`module_stager.cpp`): key `nandByPath` by `stripGenerationSuffix(f.first)` instead
of the raw real path -- one line. **Result, confirmed via a fresh run**: nearly every
`extraModule`'s real name now correctly resolves to its real NAND path, and a
200-million-instruction run hits **zero `StopInit` panics**, genuinely processing all 38
remaining modules through `ge→idstorage→syscon→rtc→lfatfs→clockgen→mediaman→ata→umdman→
umd9660→isofs→display→ctrl→led→power→msstor→codepage→fatms→codec→audio→hpremote→openpsid→
usb→wlan→wlanfirm→registry→mgr→msaudio→chkreg→mesg_led→semawm→[one more]→npdrm→
iofilemgr_dnas→chnnlsv→utility→me_wrapper.prx` (call #39) -- which then hangs permanently,
waiting on a real ME-side signal this project's ME emulation didn't yet provide (next
entry), a structurally new kind of problem (the module's own code genuinely runs and gets
stuck, rather than failing before it even starts).

## Ported the real ME (Media Engine) core from psp-lle-emu -- fixes the `me_wrapper.prx`
permanent hang (2026-11-08)

Confirmed the hang's exact mechanism first: the boot thread is genuinely, permanently
blocked in `sceKernelWaitEventFlag`/`sceKernelDelayThread`, forever, because nothing ever
sets that event flag -- not a bug to trace, but a whole subsystem (the Media Engine) never
ported to this project. `psp-lle-beta`'s `MeController` only ever modeled the instant
command-handshake MMIO registers, with no real second CPU core behind them to answer the
real RPC.

**Ported `psp-lle-emu/src/core/me/me_core.h`** (confirmed genuinely LLE by reading it: a
second real `Cpu` instance with its own `MeBus` memory map, own `InterruptController`, real
doorbell-based interrupt wiring both directions) to `src/core/me/me_core.h`. Deliberately
dropped the old project's `MeCodec` (host-side AT3+/MP3 audio-decode HLE) at this point --
not needed to clear the boot-time RPC, and depends on ffmpeg-derived decoders not yet
ported.

**One real adaptation needed** (not a verbatim copy): this project's `Bus` interface is
split into `Bus`/`TimingSource`/`JitHints`/`SyscallResolver` mixins (the old project had one
monolithic `Bus`), so `MeBus` inherits the combined `CpuBus` marker interface instead of
plain `Bus` -- every other method's default already matched the old project's explicit
overrides, so no behavior changed. `SystemBus`/`SystemControl` already had every hook point
the port needed (`meLle`/`onMeReset`/`onInterruptToMe`/`interruptFromMe()`, and explicit
`// Phase C restores this` placeholder comments at exactly the right spots) -- a previous
porting pass had clearly anticipated this moment.

**Confirmed working, live**: `"[ME] host bootstrap: image at 0x8839F5C0, first stage 582036
bytes -> 0x88300000, second segment at 0x883DE517"` -- the real ME firmware image, found and
decompressed from guest RAM -- and the boot genuinely continues past `me_wrapper.prx`
through modules #40-49 (`vaudio→impose→avcodec→vshbridge→mediasync→usersystemlib→
libatrac3plus→paf→common_gui→common_util`, `paf.prx` alone taking ~36M instructions to
load, the biggest single module in the whole chain) with zero `StopInit` panics.

A follow-up 2-billion-instruction run found a NEW permanent hang: PC frozen at `0x88129974`
from ~176M through 2B+ instructions -- a real `j <self>` trap (same idiom as the
already-known `StopInit` self-loop at `0x8816F538`) baked into real compiled kernel code,
reached from inside `common_util.prx`'s own relocated code. This project's own panic
detector only recognizes the ONE hardcoded `0x8816F538` address, so this second self-loop
was invisible to it -- the run just silently reported `[boot] OK` at whatever budget ran
out. (Worth generalizing the detector to flag ANY address where PC stops changing for a
large instruction threshold, rather than one hardcoded address -- not yet done.)

## Real sysmem partition-table fix: `unk24=2`, derived from uofw's own
`SetMemoryPartitionTable` formula, not guessed (2026-11-08)

Root-caused the `0x88129974` self-loop as a genuine out-of-memory trap (`a0`/`a1` at the
trap site look like an allocation size and a current heap pointer; `paf.prx`, loaded
immediately before, is by far the largest real consumer of VSHELL-partition memory in the
whole boot chain). Per the user's own direction ("see what the old version has" before
chasing it blind), checked the old project's own `SysMemConfig` injection mechanism first --
this project already has the identical mechanism, same field offsets, matching every value
the old project sets, **except** this project had separately bumped `unk8` (kernel partition
size override) from the old project's `0x00380000` to `0x01000000` in earlier work, to fix a
different, unrelated BSS-clear-underflow panic.

**Read `SetMemoryPartitionTable` (`uofw-master/src/kd/sysmem/sysmem.c:585-669`) directly**:
the kernel partition (`other1`/`unk8`) and the VSHELL/USER partition (`scUser`) are a direct
zero-sum trade-off (`diff = config->unk8 - other1.size_default; scUser.size -= diff;`).
Which DEFAULT sizes apply depends on an undocumented field, `unk24`: left at its zero-filled
default, `type=3` gives `scUser` a 24MB default; `unk24==2` gives `type=2` and DOUBLES both
defaults (`scUser`=48MB). Doing the arithmetic: this project's larger `unk8` override,
combined with the smaller `type=3` default, left only ~11MB for the entire VSHELL/USER
partition -- exactly where `paf.prx`/`common_util.prx`'s own real allocations exhausted it.

**Fix**: set `kConfigAddr+24` (`unk24`) to `2`. Real hardware's own semantics for this field
are genuinely unknown (proprietary boot ROM, not in uofw) -- `2` isn't a blind guess, it's
the only value that reaches the correct-looking, `model=2`-consistent partition layout,
derived directly from the real formula. **Confirmed via a 250M-instruction run**: the
`0x88129974` trap never fires; `common_util.prx`'s own relocated code is now visibly
executing across many distinct real addresses instead of being frozen solid.

## MILESTONE: psp-lle-beta reaches a real, rendered, user-confirmed XMB (2026-11-08)

The full chain of fixes that got here, all from one continuous investigation: the
`bootInfo->modules` pointer redirect; the real-marker partition-ID fix for `extraModules`;
the `extraModules` RAM-write-timing fix; the NAND generation-suffix key bug (the single
biggest-impact fix, clearing ~38 modules at once); `unk24=2` in the sysmem config
injection; porting the real ME core; and finally, porting a **minimal `MeCodec`**
(`me/me_codec.h`, new to this project, NOT ffmpeg-backed -- a silent-output placeholder
using the real work-area protocol/formulas for commands `0x60`/`0x8C`) -- without it, real
AT3+ decode RPCs reached the real ME firmware's own VME-dependent decode path, which
`vme.h`'s own header comment already documents as having a real, unresolved hardware gap
(request/response channels time out), permanently stalling boot. This was the last blocker.

**Confirmed result**: a 500M-instruction run's end-of-run framebuffer dump shows 100%
non-black pixels (130560/130560), and `cmd=0x60` (AT3+ decode) is answered repeatedly in a
real streaming pattern instead of stalling after the first call. **The user confirmed
visually: this is the real XMB.** What's genuinely real here: the ME core, its
interrupt/doorbell wiring, the real ME firmware execution, the real per-module boot loop,
the real NAND/registry/file I/O, the real GE rendering. The one deliberate placeholder is
`MeCodec`'s decoded audio content (silence, not real ATRAC3+/MP3 samples) -- a conscious,
scoped choice to unblock boot without ffmpeg-derived decoders this project doesn't have yet.

**Same night: ported a live display window.** `psp-lle-emu/src/core/frontend/live_window.h`
(`LiveWindow`, Win32 GDI, self-contained) ported to `src/core/frontend/live_window.h`,
gated by `PSP_WINDOW=1` (now threaded through `EmuOptions::window`, see the Options-system
entry below). Opens immediately at process start, gets a fresh frame every
`stateHashInterval` instructions, stays open after the instruction budget is reached.
**Confirmed live by the user, watching their own screen: the window genuinely shows the
real XMB updating in real time.** Keyboard already reads into `LiveWindow::buttons()`
(PPSSPP's default mapping) but isn't wired to real `sceCtrl` yet.

**Remaining known HLE at this milestone**: GE 3D rendering (primitives counted, not
rasterized; 2D/through-mode is real), `MeCodec`'s silent audio, the ME's
`bootFirstStageOnHost` first-stage bootstrap, KIRK command 0x12 (certificate verify --
`result=0` unconditionally, fixed below), the Memory Stick low-level protocol (explicitly
documented as a deliberate, honest shortcut), and the UMD region hardcoded to region 2.

## Real KIRK cmd 0x12 (CERT_VERIFY), replacing the HLE shortcut, verified live against this
console's real NAND (2026-11-10)

Read the local KIRK PDF (`E:\psp project\Kirk - PSP Developer wiki.pdf`) in full to replace
`kirk_mmio.h`'s `result = 0; // TEMPORARY HLE` shortcut for KIRK command 0x12 with a real
implementation. Correction to the old code comment: real command 0x12 is **AES-CBC-MAC
(OMAC1)**, not ECDSA as the stale comment guessed. jpcsp HLEs this command identically
(`KIRK.java`, `return 0;`) -- no help there.

**Real algorithm** (PDF + uofw cross-reference): input is a 0xB8-byte ID Storage
certificate, matched byte-for-byte against uofw's `SceIdStorageConsoleIdCertificate`
(`include/idstorage.h`, confirmed size 184 = 0xB8) -- real uofw code
(`src/kd/chkreg/chkreg.c` `_sceChkregVerifyConsoleIdCertificate`) passes the whole cert as
this command's buffer. Layout: 0xA8 bytes of signed data + a trailing 0x10-byte `hash` field
at offset 0xA8, the real AES-CBC-MAC/OMAC1 value to check. Uses the per-console key mesh
with **seed parameter 4**. The existing `kirk_individual_key_100` (hardcoded to seed=1 for
commands 5/8) was generalized to `kirk_individual_key_mesh(seed_param)`, implementing the
PDF's `make_individual_key` formula.

**Found and fixed an off-by-one in the PDF's own listed pseudocode** for the key-mesh round
count: its loop (`while ((seed_param--) >= 0) encrypt();`) executes one MORE round than
`(seed_param/2)+1`. Demonstrably wrong against this console's own real NAND: this project
already empirically confirmed (via a known-plaintext search, an earlier session) that
seed=1 takes exactly ONE round, matching `(1/2)+1=1` executed exactly once, not twice. The
general formula is right; only the website's transcribed loop bound is off -- implemented as
a plain `for` loop running the computed count exactly, consistent with the other known data
point (seed=3/command 16 -> 2 rounds).

**Changes**: `third_party/libkirk/kirk_engine.{c,h}` -- generalized key-mesh fn, new
`kirk_CMD18`, wired into the dispatch; the actual OMAC1 compute reuses libkirk's own
existing, previously-unused `AES_CMAC` (RFC4493). `src/core/mmio/kirk_mmio.h` -- deleted the
`command_==0x12` special case entirely, so it flows through the same real `kirk_->command()`
path as every other KIRK command.

**Verified live**: ran a real 6-billion-instruction boot to the real XMB (same confirmed
state as the milestone above). The real firmware's own `chkreg` code called real KIRK cmd
0x12 against this console's actual NAND-resident ID Storage certificate twice, both
`result=0` (success) -- the certificate genuinely matches its own real on-console signature
under this implementation. Strong real-hardware confirmation of the cert layout, the seed=4
key derivation, and the CMAC boundary, not a guess.

## Full KIRK completeness audit against the PDF: two more real bugs found and fixed
(command 0xF wrong size/no-op, command 0xA unreachable); commands 2/3/6/9 confirmed to be
dead paths for this firmware (2026-11-10)

Went through the PDF's full command table (0-18) against `kirk_mmio.h`'s `computeSizes`
switch and libkirk's dispatch. Added temporary `[DIAG169]`/`[DIAG170]` instrumentation
(left in, disposable-diagnostic convention) logging every KIRK command real firmware
actually issues -- REJECTED (silently reports `PSP_KIRK_INVALID_OPERATION`) vs ACCEPTED.
Ran the real boot (sampled at ~630M and a fresh 60M instructions): the real cold-VSH-boot
firmware issues commands `0x1, 0x7, 0x8, 0xB, 0xF, 0x11, 0x12` -- never 0xA, 2, 3, 6, or 9.

**Command 0xF (KIRK_CMD_SEED)**: a real, live bug -- coded with the wrong size (`inSize=8`
instead of the real 0x1C=28 per the PDF) and handled as a pure no-op. The audit confirmed
real firmware genuinely calls this every boot. Implemented a real `kirk_CMD15`: increments
the real 64-bit input counter (matches jpcsp's own partial-real behavior here), and derives
the 0x14-byte output seed via the engine's own real SHA1 primitive (`kirk_CMD11`) mixing the
caller's seed with existing `PRNG_DATA` state and current time. Does not claim bit-exact
parity with real hardware's own undisclosed true-RNG entropy source for the later reseed
steps -- by definition nothing a real program can check distinguishes "correct" PRNG output
from any other (jpcsp's own equivalent agrees, filling its output with a constant
`0x12345678` placeholder instead).

**Command 0xA (KIRK_CMD_PRIV_SIGN_CHECK)**: already correctly implemented in libkirk
(`kirk_CMD10`, real AES-CMAC header/data verification) and wired into the dispatch switch,
but completely missing from `kirk_mmio.h`'s `computeSizes` -- any real caller would've
silently hit "invalid operation" with no indication why. Added the real sizing (same
metadata-header convention as command 1, since the PDF documents 0xA as taking "the same
data as commands 1, 2 and 3"). Not observed to be exercised by this specific cold-VSH-boot
firmware (plausibly used by game-launch EBOOT signature checks instead) but the routing bug
is real and the fix is correct regardless.

**Verified non-regressing**: rebuilt, reran the boot -- command 0xF now shows the correct
28/28 size and gets accepted, and the real stage-1 halt point (`haltConfirmedAtInstr=
30201577`) is bit-for-bit identical to before the fix.

**Commands 2, 3, 6, 9** (DNAS encrypt/decrypt, user-key encrypt/decrypt): confirmed, via the
same live audit, to be genuinely dead paths for this console's real cold-boot firmware --
not silent bugs. Deliberately not implemented this pass: no way to verify a from-scratch
implementation against real data (no embedded signature to check, unlike cmd 0x12), and
already tracked as a known gap (`reference_kirk_spock_pdfs` memory).

**KIRK status after this pass**: every command the real, confirmed-booting-to-XMB firmware
actually issues is now real -- no HLE, no silent no-ops, no routing bugs.

## Follow-up: commands 3, 6 and 9 implemented for real too -- "dead for this boot" isn't
"dead, full stop" (2026-11-11)

User correction after the KIRK completeness audit above described commands 2/3/6/9 as
"confirmed... dead paths": not all of them are dead in general, just unexercised by this
one cold-VSH-boot path -- and since the full real command spec exists (the local KIRK PDF),
leaving them unimplemented just because this boot doesn't call them wastes documentation
we actually have. Re-read the PDF's "Commands 1, 2, 3 & 0xA" and "Commands 4~9" sections in
full and implemented what the spec actually supports with key material already in this
codebase:

- **`kirk_CMD6`/`kirk_CMD9`** (`KIRK_CMD_ENCRYPT_IV_USER`/`DECRYPT_IV_USER`, real commands
  6/9): fully real now. No missing Sony secret -- the "per-console key" step uses
  `kirk_individual_key_mesh` with seed 2, the same already-generalized primitive seeds
  1/2/4 already use elsewhere in this file; the rest of the key material (the "additional
  key" and the reencrypted random key) is caller-supplied per the real protocol, not a
  hidden constant. `kirk_mmio.h`'s `computeSizes` extended for the real longer headers
  (0x24 bytes in / 0x34 out for 6, 0x34 in / aligned data out for 9).
- **`kirk_CMD3`**: real for the **AES-CMAC signature mode** -- same algorithm as command 1
  (see `kirk_CMD1`'s own steps), differing only in using a per-console seed-0 key instead
  of command 1's static `kirk->aes_kirk1` constant and (for a real ECDSA-mode caller) a
  different public-key slot. `kirk_CMD10` gained a matching `KIRK_MODE_CMD3` branch for the
  shared header+data CMAC check.
- **NOT implemented, and correctly so**: command 2, and the ECDSA-signature-mode branch of
  commands 1/3. The PDF names specific real key slots for these (2/3 and 4/5/6 for the
  ECDSA keys, "key slot 3" for command 2's own AES key) that this codebase has no source
  for -- unlike commands 3/6/9 above, there is no already-known key material to derive
  these from, so guessing would mean fabricating a value that LOOKS like real crypto but
  isn't, which is worse than an honest, documented gap. Left as `KIRK_INVALID_OPERATION`/
  `KIRK_SIG_CHECK_INVALID` with the gap spelled out in the code comment, not silently
  stubbed.

**Verified non-regressing**: clean rebuild (w64devkit, `build_rel`), 44/45 tests passing
(same pre-existing unrelated failure as throughout this project), and a fresh 60M-
instruction boot run reproduces the exact same `haltConfirmedAtInstr=30201577` as every
earlier checkpoint -- expected, since none of commands 2/3/6/9 are issued by this specific
cold-VSH-boot firmware; this pass closes a documentation/completeness gap, not a boot bug.

## Reconnected the `EmuOptions`/`options_cli.cpp` config-injection system, which had
silently drifted apart from the real boot driver -- GE and ME no longer call `std::getenv()`
directly (2026-11-10)

A docs audit (README.md/PORTING_LOG.md vs. actual code) found the project's own flagship
Phase 0 architectural decision -- the "no file outside `options_cli.cpp` calls `getenv()`"
rule -- had been silently violated. GE (`gpu/ge_renderer.h`) and ME (`me/me_core.h`,
`me/me_codec.h`) porting happened after Phase A's `options.h`/`options_cli.cpp` work and
neither was ever threaded through it: ~19 raw `getenv()` sites had accumulated. Worse:
`tests/driver/psp_boot_driver.cpp` -- the actual real-boot entry point used for every real
run this whole project -- never called `parseOptionsFromEnvironment()`/
`applyCommandLineArgs()` at all; it read `PSP_WINDOW` directly, and `GeOptions` (which
already existed, with the right fields, already populated from env in `options_cli.cpp`)
was never read by anything downstream. Completely dead plumbing.

**Fix**: extended `GeOptions` (10 new fields: `recentPrims`/`recentMax`/
`normalMatInverseTranspose`/`analyticNormals`/`texDumpDir`/`texDumpRaw`/`patchDebug`/
`xfBoxFrom`/`threads`, plus the pre-existing `gpuAccel`/`stats`) and added a new `MeOptions`
struct (`ratio`/`horizonAlways`/`log`/`shadow`/`realBoot`). `GeRenderer` gained a real
constructor taking `const GeOptions&`; its two function-local-static singletons
(`StatRegistry`/`RasterPool`, previously shared across all `GeRenderer` instances via lazy
statics reading `getenv()` on first construction) became plain per-instance members instead
-- confirmed via grep that only ONE `GeRenderer` is ever constructed anywhere in this
codebase, so the singleton sharing bought nothing. `GeCoreStub`, `MeBus`, `MeCore`,
`MeCodec`, and `SystemBus` (which gained two new defaulted constructor parameters,
`GeOptions`/`MeOptions`, plus a stored `MeOptions` member for `enableMe()`'s later use) all
now take their options struct instead of reading environment variables themselves.

`tests/driver/psp_boot_driver.cpp`'s `main()` now builds a real `EmuOptions` via
`parseOptionsFromEnvironment()`/`applyCommandLineArgs()`, threaded through a new
`BootDriverOptions::emu` field down into `runBoot()`, replacing the direct `PSP_WINDOW`
read and the dead `SystemBus` construction call.

**Verified non-regressing three separate times** (original boot, after the GE fix, after
the ME fix): the real boot's stage-1 halt point (`haltConfirmedAtInstr=30201577`) is
bit-for-bit identical throughout. Also verified the plumbing is genuinely live, not just
compiling: `PSP_GE_STATS=1` produces real `[GESTATS]` output at process exit (previously the
same env var was read into `EmuOptions.ge.stats` by `options_cli.cpp` but never reached
`GeRenderer` at all).

**Deliberately out of scope**: the instruction-budget positional `argv[1]` in `main()` (a
single, always-visible driver parameter, not a hidden/environment-based behavior gate -- the
rule's own stated motivation) and `tests/test_framework.h`'s `BETA_TEST_FILTER` (a
gtest-`--filter`-style test-runner convenience, not emulation-behavior configuration).

## Window-mode flashing fix: repaint on the real vblank edge, not a fixed instruction count (2026-11-11)

User reported the live window (`--window`) flashing in and out once the real XMB was up and
rendering. Root cause: `psp_boot_driver.cpp`'s window-submit call was gated on
`stateHashInterval` (default 1,000,000 instructions, originally just a `[STATE]`-line
print cadence), completely unsynced from the real 60Hz vblank. The guest flips its display
buffer in lockstep with the real vblank IRQ (`PSP_VBLANK_INTR` = IRQ 30), which this
project's `DisplayController` already modeled correctly (timer + IRQ trigger) but nothing
read; sampling the framebuffer on an arbitrary fixed instruction count instead drifts in and
out of phase with the guest's own clear/draw/flip cycle -- a classic beat-frequency flicker,
since 1,000,000 instructions isn't an exact multiple of the guest's real per-frame
instruction count.

**Fix**: `DisplayController::advance()` (`src/core/mmio/display_controller.h`) now counts
real vblanks (`vblankCount_`, exposed via `vblankCount()`); `SystemBus` gained a
`displayController()` accessor matching the existing `geCore()`/`nandController()` style.
The main loadcore/kernel loop in `psp_boot_driver.cpp` (the one actually running once XMB is
up) now repaints the window exactly when `vblankCount()` changes, falling back to the old
fixed-cadence behavior only before `display_01g.prx` starts the vblank clock (nothing
meaningful to show that early anyway, just boot-progress frames). `[STATE]` line printing
keeps its own independent `stateHashInterval` cadence, unchanged.

**Verified non-regressing**: 44/45 tests (same pre-existing unrelated failure), boot
driver's `haltConfirmedAtInstr=30201577` and DIAG164 `nonBlackPixels=115080/130560` both
bit-exact against the pre-fix build. **Confirmed live by the user**: flashing gone, XMB
rendering smooth.

## Non-deterministic RAM state hash: real KIRK command 15 ("SEED") mixes in real wall-clock time(0) (2026-11-11)

While verifying the vblank-sync fix above, noticed the no-window test driver's RAM state
hash differs between separate runs of the *same unmodified binary*, even though CPU
registers, PC, and the cp0 Count register are bit-for-bit identical at every checkpoint.
Investigated properly rather than guessing: binary-searched the exact first-divergence
instruction (pinned to exactly instr 36,000,000, confirmed not an artifact of the run's own
end -- same result whether or not the run continues past it), then found the exact byte
range (20 bytes, offset 1,451,976 in the RAM window) via a direct dump-and-diff, then used a
one-off native `__builtin_trap()` keyed on `SystemBus::write8`'s own `writeCounter` (a
regular gdb watchpoint/conditional breakpoint was far too slow against 36M+ interpreted
instructions -- a native trap caught under gdb was instant) to get a real backtrace. Real
cause: `KirkMmio::startProcessing` dispatching real guest-issued **KIRK command 15
("SEED")**, which (third_party/libkirk/kirk_engine.c's `kirk_CMD15`) mixes real wall-clock
`time(0)` into its output -- a 20-byte SHA1-derived value that gets written straight back
into guest RAM via the real MMIO write-back path. Two earlier `time(0)` call sites
(`kirk_init2`'s initial PRNG seed, `kirk_CMD14`'s own PRNG command) were fixed first based on
a plausible-but-wrong theory; neither is actually exercised by a normal boot (confirmed via
a one-off dispatch trace: `command_` values seen were 1, 7, 8, 11, 15 -- never 14), so they
turned out to be real-but-unexercised fixes, not the live cause. `kirk_CMD15`'s own call was
the one the native-trap backtrace actually caught.

**This is genuinely real KIRK command 15's own hardware behavior** (a real per-console
nonce/counter-advance primitive, not an emulator bug) -- real hardware is non-deterministic
here too. Fixed the same way as the other two: `KirkState::use_deterministic_prng_time` /
`deterministic_prng_time` (already added for CMD14/kirk_init2) now also gates CMD15's
`time(0)` call. `PSP_DETERMINISTIC_KIRK_PRNG=1` (test-driver-local env var, see
`psp_boot_driver.cpp`'s own comment) pins all three to a fixed value for exact-repeat test
runs; unset (the default) keeps real hardware's own non-determinism.

**Verified**: 5 consecutive runs with the flag set now produce a bit-identical RAM hash
(previously different every single time); unset, it still varies (confirmed the fix doesn't
accidentally force determinism onto real use). 44/45 tests, `haltConfirmedAtInstr`, and
DIAG164's `nonBlackPixels` all unchanged.
