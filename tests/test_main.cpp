#include "test_framework.h"

#include <cstdio>

void runAllStateHashTests(TestStats& stats);
void runAllGoldenTraceTests(TestStats& stats);
void runAllFirmwareExtractionTests(TestStats& stats);
void runAllSystemBusTests(TestStats& stats);
void runAllCpuTests(TestStats& stats);
void runAllVfpuTests(TestStats& stats);
void runAllModuleStagerTests(TestStats& stats); // unchanged declaration

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    TestStats stats;
    runAllStateHashTests(stats);
    runAllGoldenTraceTests(stats);
    runAllFirmwareExtractionTests(stats);
    runAllSystemBusTests(stats);
    runAllCpuTests(stats);
    runAllVfpuTests(stats);
    runAllModuleStagerTests(stats);

    std::printf("\n%d/%d tests passed\n", stats.totalTests - stats.totalFailures, stats.totalTests);
    return stats.totalFailures == 0 ? 0 : 1;
}
