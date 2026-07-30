// Performance profiling utilities for GleamDebugger

#ifndef GLEAM_PERFORMANCE_H
#define GLEAM_PERFORMANCE_H

#include <cstdint>
#include <string>
#include <vector>
#include <Windows.h>

namespace Gleam {

// High-precision timer using QueryPerformanceCounter
class PerformanceTimer {
private:
    LARGE_INTEGER mStart;
    LARGE_INTEGER mFrequency;
    bool mRunning;

public:
    PerformanceTimer();

    // Start timing
    void start();

    // Stop timing and return elapsed microseconds
    int64_t stopMicros();

    // Stop timing and return elapsed milliseconds
    double stopMillis();

    // Get elapsed time without stopping (microseconds)
    int64_t elapsedMicros() const;

    // Get elapsed time without stopping (milliseconds)
    double elapsedMillis() const;

    // Check if timer is running
    bool isRunning() const { return mRunning; }
};

// RAII-style scoped timer that logs duration on destruction
class ScopedTimer {
private:
    PerformanceTimer mTimer;
    const char* mName;
    bool mLogOnDestroy;

public:
    ScopedTimer(const char* name, bool logOnDestroy = true);
    ~ScopedTimer();

    // Get elapsed time (microseconds)
    int64_t elapsedMicros() const { return mTimer.elapsedMicros(); }

    // Get elapsed time (milliseconds)
    double elapsedMillis() const { return mTimer.elapsedMillis(); }

    // Disable auto-logging on destruction
    void disableLog() { mLogOnDestroy = false; }
};

// Performance event tracking
enum class PerfEvent {
    SymbolResolve,        // Symbol resolution via dbghelp
    IltScan,              // ILT thunk target scan
    BreakpointBind,       // Logical breakpoint binding
    BreakpointSet,        // Physical breakpoint write
    MemoryRead,           // Remote process memory read
    MemoryWrite,          // Remote process memory write
    ModuleLoad,           // Module load event processing
    ExceptionDispatch,    // Exception event dispatch
};

// Single performance measurement
struct PerfMeasurement {
    PerfEvent event;
    int64_t durationMicros;
    bool cacheHit;  // For cache-related events
    std::string details;  // Optional context (e.g., symbol name)
};

// Performance statistics collector
class PerfStats {
private:
    std::vector<PerfMeasurement> mMeasurements;
    bool mEnabled;

public:
    PerfStats();

    // Enable/disable collection
    void enable() { mEnabled = true; }
    void disable() { mEnabled = false; }
    bool isEnabled() const { return mEnabled; }

    // Record a measurement
    void record(PerfEvent event, int64_t durationMicros,
                bool cacheHit = false, const std::string& details = "");

    // Get all measurements
    const std::vector<PerfMeasurement>& measurements() const { return mMeasurements; }

    // Clear collected data
    void clear() { mMeasurements.clear(); }

    // Get statistics for a specific event type
    struct EventStats {
        int64_t count;
        int64_t totalMicros;
        int64_t minMicros;
        int64_t maxMicros;
        double avgMicros;
        int64_t cacheHits;
        int64_t cacheMisses;
    };
    EventStats getStats(PerfEvent event) const;

    // Print summary report
    void printReport() const;
};

// Global performance stats instance
extern PerfStats g_perfStats;

// Helper macros for easy profiling
#define PERF_TIMER(name) Gleam::ScopedTimer _perf_timer_##__LINE__(name)
#define PERF_TIMER_NO_LOG(name) Gleam::ScopedTimer _perf_timer_##__LINE__(name, false)

// Helper to record events with automatic timing
class PerfRecorder {
private:
    PerformanceTimer mTimer;
    PerfEvent mEvent;
    bool mCacheHit;
    std::string mDetails;

public:
    PerfRecorder(PerfEvent event, const std::string& details = "")
        : mEvent(event), mCacheHit(false), mDetails(details)
    {
        if (g_perfStats.isEnabled())
            mTimer.start();
    }

    void setCacheHit(bool hit) { mCacheHit = hit; }

    ~PerfRecorder() {
        if (g_perfStats.isEnabled()) {
            int64_t duration = mTimer.elapsedMicros();
            g_perfStats.record(mEvent, duration, mCacheHit, mDetails);
        }
    }
};

#define PERF_RECORD(event, details) Gleam::PerfRecorder _perf_recorder_##__LINE__(event, details)
#define PERF_CACHE_HIT() _perf_recorder_##__LINE__.setCacheHit(true)

} // namespace Gleam

#endif // GLEAM_PERFORMANCE_H
