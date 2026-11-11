# psp-lle-beta

A ground-up architectural rework of [`../psp-lle-emu`](../psp-lle-emu) — same goal (real,
unmodified Sony PSP firmware on a from-scratch Allegrex LLE core, no HLE reimplementation
of the OS), cleaner module boundaries. Started 2026-11-03; `psp-lle-emu` is frozen as a
read-only reference as of this project starting, and all new work happens here. It is a
personal reverse-engineering and emulator-development project, not affiliated with Sony.

**This is a proof of concept, not a usable emulator.** It boots one specific console's own
real 6.61 firmware to a genuinely live, interactive XMB — that part is real, not staged or
scripted — but there is no JIT (pure interpreter), no GPU-accelerated rendering path, no way
to launch a game or piece of homebrew from the command line yet, and several subsystems
(audio decode, the ME's first boot stage, Memory Stick's low-level protocol) are host stand-
ins rather than real hardware behavior — see "Remaining known HLE" below. Treat it as a
from-scratch architecture and a milestone demo, not something to install and play on.

This source tree does **not** bundle any Sony firmware or NAND dump — those are supplied
locally (see "Quick start" below) and never committed here. If you're pulling in `661.PBP`
or a console's NAND dump to run it, that's Sony's own copyrighted firmware and your own
console's data respectively: keep them out of git (`.gitignore` already covers the usual
filenames) and don't redistribute them.

## Status

**MILESTONE reached (2026-11-08):** real, unmodified 6.61 firmware (this console's own 03g
build) boots all the way to a rendered, live, interactive XMB — confirmed visually
(framebuffer dump + a live Win32 window), with a real ported ME (Media Engine) core and real
audio RPC traffic. Since then:

- A real EBOOT (FasterARK) decrypts, loads, runs to completion from the XMB, and performs
  its own real apiType 0x210 (Exit Game / warm VSH) reboot back out.
- The VFPU's register addressing/prefix machinery and its non-transcendental
  arithmetic/data-movement instruction set are implemented (transcendentals — sin/cos/sqrt/
  log2/exp2/... — are not yet; real hardware's lookup tables are needed for exactness).
- KIRK (the crypto engine) was audited command-by-command against the real hardware
  reference: every command this firmware's real boot sequence actually issues is implemented
  for real, including certificate-verify (AES-CBC-MAC/OMAC1) and RNG-reseed. KIRK commands
  2/3/6/9 (DNAS/user-key encrypt/decrypt) remain unimplemented but are confirmed, via a live
  command-usage audit, to be dead paths for this firmware.
- A NOP-sled watchdog catches silent hook-list/boot-timeline mismatches (the class of bug
  behind the FasterARK launch crash) within tens of thousands of instructions instead of
  needing hours of manual tracing.
- **44/45 unit tests passing** (`build_rel/tests/beta_tests.exe`; one known failure,
  `SystemBus.UnmappedReadThrows`, is a harness expectation mismatch, not a boot-correctness
  issue).

See [docs/PORTING_LOG.md](docs/PORTING_LOG.md) for the full, dated chronology.

**Remaining known HLE** (not real hardware emulation — see [docs/HLE.md](docs/HLE.md) for
the complete, categorized inventory): GE 3D rasterization (command dispatch is real LLE;
the pixel-producing rasterizer itself is a from-scratch host implementation, since the GE is
fixed-function silicon with no kernel-module equivalent to trace), the ME's host-side audio
decode (outputs silence instead of real AT3+/MP3 — `psp-lle-emu` has real decoders for this,
not yet ported here), the ME's first boot stage, the Memory Stick low-level protocol, and the
UMD region (hardcoded to region 2).

**Not finished yet / not supported by this build's driver:** mounting a disc or launching
homebrew from a command line — `psp_boot_driver.exe` boots straight to the XMB; there is no
CLI hook for either yet (see "Quick start" below for the flags that do exist). GPU-accelerated
rendering (`GeOptions::gpuAccel` exists but every GPU code path is compiled out of every
current build). No JIT — this project is a pure interpreter so far.

## Quick start

Windows x64, no installers: a portable GCC and Ninja are fetched into the repo-root `tools/`
folder (shared with the sibling `psp-lle-emu` checkout, if present — `build.ps1` finds either
project's copy automatically, so nothing is ever fetched twice).

```powershell
powershell -ExecutionPolicy Bypass -File .\setup-toolchain.ps1   # once
powershell -ExecutionPolicy Bypass -File .\build.ps1             # Release build into build_rel
.\build_rel\tests\beta_tests.exe                 # unit tests (44/45)
.\build_rel\tests\psp_boot_driver.exe 1000000000000000 --window   # the real boot, in a window
```

The instruction-budget argument is basically unlimited by default already
(`BootDriverOptions::instructionBudget`) — the window loop has no other exit, so the number
above just matches that default explicitly; close the window (or kill the process) to end a
run rather than waiting it out.

The boot driver needs a `661.PBP`, a NAND dump, and a console fuse ID (same file/format
`psp-lle-emu`'s own `pspcfg::requireFuse()` uses — see `tests/driver/fuse_config.h`) at the
paths `BootDriverOptions` defaults to in `tests/driver/psp_boot_driver.h`; point those
defaults at your own portable folder's `nand/`, `firmware/` and `config/` subfolders if you
use one. All three have to come from your own PSP, and specifically **a PSP-3000**: this
project's memory map, MMIO models and boot sequence are traced against that model's own
hardware (Tachyon/Baryon revisions, NAND layout), so a dump from a PSP-1000, 2000 or Go is
not a drop-in substitute.

- `661.PBP` is Sony's own 6.61 system update file, from Sony's PSP system-software update
  service.
- The NAND dump and the fuse ID both require hardware you own: a NAND dumper and a tool such
  as PSPident, run against your own PSP-3000. Never use someone else's dump or fuse ID — the
  dump carries that console's own CFW install, settings and identifiers, and the fuse ID is
  paired to that exact dump.

**Driver CLI flags:** `--window`, `--window-title=<text>`, `--state-hash-every=<n>`,
`--scripted-input=<path>`, `--gpu-accel`, `--no-audio`. **A few of the env vars**
`src/config/options_cli.cpp` (the only file in this project allowed to read them) recognizes:
`PSP_NO_AUDIO`, `PSP_MS_ABSENT`, `PSP_MS_FOLDER=<path>`, `PSP_WLAN_SWITCH`,
`PSP_GPU_GE`, `PSP_RTC_SEED=<n>`; the full, current list is that file, not a doc, since it's
also this project's own enforced single source of truth (see "Rule" below).

## Documentation

| Document | Contents |
|---|---|
| [docs/HLE.md](docs/HLE.md) | every place real hardware/firmware behavior is stood in for, categorized (genuine HLE, look-alike patches that aren't, unused HLE-capable mechanisms) |
| [docs/PORTING_LOG.md](docs/PORTING_LOG.md) | the full dated history: what moved from `psp-lle-emu`, what changed, why, including dead ends |

## Rule: no silent configuration

No file outside `src/config/options_cli.cpp` calls `std::getenv()` or reads `argv` — verify
with `rg "getenv\(" src/`; any hit outside that one file is a bug. A subsystem that wants a
toggle adds a field to its own sub-struct in `src/config/options.h` and takes that struct (or
a reference to it) in its own constructor — see `GeOptions`/`MeOptions` and
`GeRenderer`/`MeCore`/`SystemBus` for the established pattern. (This was silently violated for
a while during GE/ME porting and fixed on 2026-11-10 — see `docs/PORTING_LOG.md` — which is
exactly the kind of regression this rule, and the `rg` check, exist to make structurally
impossible.)

## Layout of this directory

```
src/core/          the emulator core
  cpu.h/.cpp          Allegrex interpreter
  bus/, bus.h         PSP memory map, MMIO routing
  mmio/               device models (KIRK, GPIO, syscon, display, system control, ...)
  gpu/                GE command dispatch (real) + the host software rasterizer
  jit_hints.h         (no JIT yet; interpreter only)
  me/                 Media Engine core (second Allegrex) + the audio-decode HLE stub
  net/                Wi-Fi virtual router groundwork
  crypto/             KIRK engine
  format/, firmware/  PBP/PSAR/ELF parsing, NAND-resident IPL extraction, module staging
  util/               shared helpers
third_party/       vendored code: libkirk, puff (licences below)
tests/             the unit suite (beta_tests.exe) and tests/driver/psp_boot_driver.cpp,
                   the real boot driver -- this project's actual "run the emulator" entry point
testsupport/       shared test helpers (golden-trace diffing, etc.)
tools/             disasm, investigate -- small standalone diagnostic utilities
docs/              documentation (above)
```

## Licensing

The project's own code has **no licence file yet**. Vendored third-party code in
`third_party/`: `libkirk` (from PPSSPP) is GPL v2 or later (`AES.c` public domain), `puff`
(from zlib) is zlib-licensed. Combining the GPL part into a distributed build makes the
combined work GPL-encumbered, the same position PPSSPP, RPCS3 and jpcsp are in. No audio
decoder is vendored here yet (the ME's audio decode is currently an HLE silence stub), so
unlike `psp-lle-emu` this project does not yet carry the LGPL ATRAC3+/minimp3 code.

## Contributing / poking at it

A solo, incremental effort so far, same as `psp-lle-emu`. If you're into PSP internals, real-
hardware boot chains or emulator archaeology and want to compare notes, read
[docs/PORTING_LOG.md](docs/PORTING_LOG.md) first — it documents the reasoning (and the wrong
turns) behind every change, not just the end state.
