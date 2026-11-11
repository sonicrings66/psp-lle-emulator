#pragma once

#include "bus.h"
#include "timing_source.h"
#include "jit_hints.h"
#include "syscall_resolver.h"

// Everything Cpu needs: exact device timing, the direct-RAM fast path,
// and real inter-module syscall resolution, on top of plain read/write.
// Every concrete bus (SystemBus) implements all four via multiple
// inheritance; a bare-Bus test fixture that inherits THIS instead gets
// safe, correct defaults for the other three for free -- it only has to
// implement the 6 pure Bus methods, exactly as before this interface
// split existed (see docs/PORTING_LOG.md for the split's rationale).

namespace beta {

class CpuBus : public Bus, public TimingSource, public JitHints, public SyscallResolver {};

} // namespace beta
