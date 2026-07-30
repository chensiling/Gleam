// Test suite for Logger and PerfMonitor
// Verifies that global state has been properly encapsulated

#include <gtest/gtest.h>
#include "../Gleam/Logger.h"
#include "../Gleam/PerfMonitor.h"
#include <sstream>

using namespace Gleam;

// ============================================================================
// Logger Tests
// ============================================================================

TEST(LoggerTest, DefaultLevel) {
    Logger logger;
    EXPECT_EQ(logger.getLevel(), LogLevel::Info);
}

TEST(LoggerTest, SetLevel) {
    Logger logger;
    logger.setLevel(LogLevel::Debug);
    EXPECT_EQ(logger.getLevel(), LogLevel::Debug);

    logger.setLevel(LogLevel::Error);
    EXPECT_EQ(logger.getLevel(), LogLevel::Error);
}

TEST(LoggerTest, WouldLog) {
    Logger logger(LogLevel::Warn);

    EXPECT_FALSE(logger.wouldLog(LogLevel::Debug));
    EXPECT_FALSE(logger.wouldLog(LogLevel::Info));
    EXPECT_TRUE(logger.wouldLog(LogLevel::Warn));
    EXPECT_TRUE(logger.wouldLog(LogLevel::Error));
}

TEST(LoggerTest, MultipleInstances) {
    Logger logger1(LogLevel::Debug);
    Logger logger2(LogLevel::Error);

    EXPECT_EQ(logger1.getLevel(), LogLevel::Debug);
    EXPECT_EQ(logger2.getLevel(), LogLevel::Error);

    // Changing one should not affect the other
    logger1.setLevel(LogLevel::Warn);
    EXPECT_EQ(logger1.getLevel(), LogLevel::Warn);
    EXPECT_EQ(logger2.getLevel(), LogLevel::Error);
}

TEST(LoggerTest, GlobalDefaultPointer) {
    // Save current global
    Logger* savedGlobal = g_defaultLogger;

    // Create a logger and set it as default
    Logger testLogger(LogLevel::Debug);
    g_defaultLogger = &testLogger;

    EXPECT_EQ(g_defaultLogger, &testLogger);
    EXPECT_EQ(g_defaultLogger->getLevel(), LogLevel::Debug);

    // Restore
    g_defaultLogger = savedGlobal;
}

// ============================================================================
// PerfMonitor Tests
// ============================================================================

TEST(PerfMonitorTest, InitiallyDisabled) {
    PerfMonitor monitor;
    EXPECT_FALSE(monitor.isEnabled());
}

TEST(PerfMonitorTest, EnableDisable) {
    PerfMonitor monitor;

    monitor.enable();
    EXPECT_TRUE(monitor.isEnabled());

    monitor.disable();
    EXPECT_FALSE(monitor.isEnabled());
}

TEST(PerfMonitorTest, RecordEvent) {
    PerfMonitor monitor;
    monitor.enable();

    monitor.record(PerfEvent::SymbolResolve, 1000, false, "test");

    const auto& measurements = monitor.measurements();
    EXPECT_EQ(measurements.size(), 1u);
    EXPECT_EQ(measurements[0].event, PerfEvent::SymbolResolve);
    EXPECT_EQ(measurements[0].durationMicros, 1000);
    EXPECT_FALSE(measurements[0].cacheHit);
}

TEST(PerfMonitorTest, Clear) {
    PerfMonitor monitor;
    monitor.enable();

    monitor.record(PerfEvent::SymbolResolve, 1000);
    monitor.record(PerfEvent::SymbolResolve, 2000);

    EXPECT_EQ(monitor.measurements().size(), 2u);

    monitor.clear();
    EXPECT_EQ(monitor.measurements().size(), 0u);
}

TEST(PerfMonitorTest, MultipleInstances) {
    PerfMonitor monitor1;
    PerfMonitor monitor2;

    monitor1.enable();
    monitor2.enable();

    monitor1.record(PerfEvent::SymbolResolve, 1000);
    monitor2.record(PerfEvent::BreakpointSet, 500);

    // Each monitor has independent measurements
    EXPECT_EQ(monitor1.measurements().size(), 1u);
    EXPECT_EQ(monitor2.measurements().size(), 1u);

    EXPECT_EQ(monitor1.measurements()[0].event, PerfEvent::SymbolResolve);
    EXPECT_EQ(monitor2.measurements()[0].event, PerfEvent::BreakpointSet);
}

TEST(PerfMonitorTest, GetStats) {
    PerfMonitor monitor;
    monitor.enable();

    monitor.record(PerfEvent::SymbolResolve, 1000, true);
    monitor.record(PerfEvent::SymbolResolve, 2000, false);
    monitor.record(PerfEvent::SymbolResolve, 3000, true);

    auto stats = monitor.getStats(PerfEvent::SymbolResolve);
    EXPECT_EQ(stats.count, 3);
    EXPECT_EQ(stats.totalMicros, 6000);
    EXPECT_EQ(stats.minMicros, 1000);
    EXPECT_EQ(stats.maxMicros, 3000);
    EXPECT_DOUBLE_EQ(stats.avgMicros, 2000.0);
    EXPECT_EQ(stats.cacheHits, 2);
    EXPECT_EQ(stats.cacheMisses, 1);
}

TEST(PerfMonitorTest, GlobalDefaultPointer) {
    // Save current global
    PerfMonitor* savedGlobal = g_defaultPerfMonitor;

    // Create a monitor and set it as default
    PerfMonitor testMonitor;
    g_defaultPerfMonitor = &testMonitor;

    EXPECT_EQ(g_defaultPerfMonitor, &testMonitor);

    // Restore
    g_defaultPerfMonitor = savedGlobal;
}

// ============================================================================
// PerfRecorder Tests (RAII)
// ============================================================================

TEST(PerfRecorderTest, RecordsOnDestruction) {
    PerfMonitor monitor;
    monitor.enable();

    {
        PerfRecorder recorder(&monitor, PerfEvent::SymbolResolve, "test");
        // Recorder automatically times and records on destruction
    }

    const auto& measurements = monitor.measurements();
    EXPECT_EQ(measurements.size(), 1u);
    EXPECT_EQ(measurements[0].event, PerfEvent::SymbolResolve);
    EXPECT_EQ(measurements[0].details, "test");
}

TEST(PerfRecorderTest, CacheHitFlag) {
    PerfMonitor monitor;
    monitor.enable();

    {
        PerfRecorder recorder(&monitor, PerfEvent::SymbolResolve);
        recorder.setCacheHit(true);
    }

    const auto& measurements = monitor.measurements();
    EXPECT_EQ(measurements.size(), 1u);
    EXPECT_TRUE(measurements[0].cacheHit);
}

TEST(PerfRecorderTest, DisabledMonitor) {
    PerfMonitor monitor;
    // Monitor is disabled

    {
        PerfRecorder recorder(&monitor, PerfEvent::SymbolResolve);
    }

    // Should not record anything
    EXPECT_EQ(monitor.measurements().size(), 0u);
}

// ============================================================================
// Integration Tests
// ============================================================================

TEST(GlobalStateTest, NoGlobalVariables) {
    // This test verifies that we can create multiple independent instances
    // without global state pollution

    Logger log1(LogLevel::Debug);
    Logger log2(LogLevel::Error);
    PerfMonitor perf1;
    PerfMonitor perf2;

    log1.setLevel(LogLevel::Warn);
    perf1.enable();
    perf1.record(PerfEvent::SymbolResolve, 100);

    // log2 and perf2 should be completely independent
    EXPECT_EQ(log2.getLevel(), LogLevel::Error);
    EXPECT_FALSE(perf2.isEnabled());
    EXPECT_EQ(perf2.measurements().size(), 0u);
}

TEST(GlobalStateTest, TransitionPeriodCompatibility) {
    // Verify that the global pointers work for backward compatibility

    Logger logger;
    PerfMonitor monitor;

    // Set as globals
    Logger* oldLogger = g_defaultLogger;
    PerfMonitor* oldMonitor = g_defaultPerfMonitor;

    g_defaultLogger = &logger;
    g_defaultPerfMonitor = &monitor;

    // Old code using globals should still work
    EXPECT_EQ(g_defaultLogger, &logger);
    EXPECT_EQ(g_defaultPerfMonitor, &monitor);

    // Restore
    g_defaultLogger = oldLogger;
    g_defaultPerfMonitor = oldMonitor;
}
