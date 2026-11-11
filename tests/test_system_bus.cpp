#include "test_framework.h"
#include "bus/system_bus.h"

using beta::SystemBus;

namespace {

void testConstructsAndRoundTripsRam(TestStats& stats) {
    SystemBus bus;
    const uint32_t ramAddr = 0x08000010u; // kRamStart + 0x10
    bus.write32(ramAddr, 0xDEADBEEFu);
    CHECK_EQ(stats, bus.read32(ramAddr), 0xDEADBEEFu);
    bus.write8(ramAddr + 4, 0x7Au);
    CHECK_EQ(stats, bus.read8(ramAddr + 4), 0x7Au);
}

void testIplImageLoadsAndReadsBack(TestStats& stats) {
    SystemBus bus;
    const uint8_t image[8] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    bus.loadIplImage(image, sizeof(image));
    CHECK_EQ(stats, bus.read32(0xBFC00000u), 0x04030201u);
    CHECK_EQ(stats, bus.read32(0xBFC00004u), 0x08070605u);
}

void testUnmappedReadThrows(TestStats& stats) {
    SystemBus bus;
    bool threw = false;
    try {
        // The real GE register block (0xBD400000) is deliberately unmapped
        // in beta through Phase C (see system_bus.h's class comment) -- a
        // read there must throw, same as any other genuinely unmapped gap,
        // not silently return 0.
        bus.read32(0xBD400000u);
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK_TRUE(stats, threw);
}

void testUnmappedWriteIsSilentlyDiscarded(TestStats& stats) {
    SystemBus bus;
    bool threw = false;
    try {
        bus.write32(0xBD400000u, 0x12345678u);
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK_TRUE(stats, !threw); // real open-bus behavior: a stray store past a region's edge lands nowhere, it doesn't trap
}

void testInterruptControllerReachableThroughLocate(TestStats& stats) {
    SystemBus bus;
    // No interrupt source has fired yet -- pendingInterrupts() should read
    // as a clean 0 through the real InterruptController, not fault.
    CHECK_EQ(stats, bus.pendingInterrupts(), 0u);
    CHECK_EQ(stats, bus.interruptController().hasPendingEnabled() ? 1u : 0u, 0u);
}

void testTimerCountsDownAfterArming(TestStats& stats) {
    SystemBus bus;
    // Arm timer 0 (reachable through SystemBus::timer(0), one of the typed
    // accessors kept per the §1b real-caller audit) with a period of 100,
    // then advance it by 30 instructions and confirm the live counter
    // actually counted down by 30 -- a real behavioral check of the
    // ported Timer/TimingSource wiring, not just "did this compile."
    auto& t0 = bus.timer(0);
    t0.write32(0x00, 0x00800000u | 100u); // COUNTER_ACTIVE, period=100, interrupt trigger off
    CHECK_EQ(stats, t0.read32(0x04), 100u); // currentCounter() before any tick
    t0.advance(30);
    CHECK_EQ(stats, t0.read32(0x04), 70u);
}

} // namespace

void runAllSystemBusTests(TestStats& stats) {
    runTest(stats, "SystemBus.ConstructsAndRoundTripsRam", testConstructsAndRoundTripsRam);
    runTest(stats, "SystemBus.IplImageLoadsAndReadsBack", testIplImageLoadsAndReadsBack);
    runTest(stats, "SystemBus.UnmappedReadThrows", testUnmappedReadThrows);
    runTest(stats, "SystemBus.UnmappedWriteIsSilentlyDiscarded", testUnmappedWriteIsSilentlyDiscarded);
    runTest(stats, "SystemBus.InterruptControllerReachableThroughLocate", testInterruptControllerReachableThroughLocate);
    runTest(stats, "SystemBus.TimerCountsDownAfterArming", testTimerCountsDownAfterArming);
}
