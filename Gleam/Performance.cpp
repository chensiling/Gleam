// Performance profiling implementation

#include "Performance.h"
#include "Log.h"
#include <algorithm>
#include <cstdio>

namespace Gleam {

// Global performance stats
PerfStats g_perfStats;

// PerformanceTimer implementation

PerformanceTimer::PerformanceTimer() : mRunning(false) {
    QueryPerformanceFrequency(&mFrequency);
}

void PerformanceTimer::start() {
    QueryPerformanceCounter(&mStart);
    mRunning = true;
}

int64_t PerformanceTimer::stopMicros() {
    int64_t elapsed = elapsedMicros();
    mRunning = false;
    return elapsed;
}

double PerformanceTimer::stopMillis() {
    return stopMicros() / 1000.0;
}

int64_t PerformanceTimer::elapsedMicros() const {
    if (!mRunning)
        return 0;

    LARGE_INTEGER end;
    QueryPerformanceCounter(&end);

    int64_t elapsed = end.QuadPart - mStart.QuadPart;
    return (elapsed * 1000000LL) / mFrequency.QuadPart;
}

double PerformanceTimer::elapsedMillis() const {
    return elapsedMicros() / 1000.0;
}

// ScopedTimer implementation

ScopedTimer::ScopedTimer(const char* name, bool logOnDestroy)
    : mName(name), mLogOnDestroy(logOnDestroy) {
    mTimer.start();
}

ScopedTimer::~ScopedTimer() {
    if (mLogOnDestroy) {
        double ms = mTimer.stopMillis();
        logDebug("[perf] %s: %.3f ms", mName, ms);
    }
}

// PerfStats implementation

PerfStats::PerfStats() : mEnabled(false) {
    mMeasurements.reserve(1000);  // Pre-allocate for common case
}

void PerfStats::record(PerfEvent event, int64_t durationMicros,
                       bool cacheHit, const std::string& details) {
    if (!mEnabled)
        return;

    PerfMeasurement m;
    m.event = event;
    m.durationMicros = durationMicros;
    m.cacheHit = cacheHit;
    m.details = details;

    mMeasurements.push_back(m);
}

PerfStats::EventStats PerfStats::getStats(PerfEvent event) const {
    EventStats stats = {};

    for (const auto& m : mMeasurements) {
        if (m.event != event)
            continue;

        stats.count++;
        stats.totalMicros += m.durationMicros;

        if (stats.count == 1) {
            stats.minMicros = m.durationMicros;
            stats.maxMicros = m.durationMicros;
        } else {
            if (m.durationMicros < stats.minMicros)
                stats.minMicros = m.durationMicros;
            if (m.durationMicros > stats.maxMicros)
                stats.maxMicros = m.durationMicros;
        }

        if (m.cacheHit)
            stats.cacheHits++;
        else
            stats.cacheMisses++;
    }

    if (stats.count > 0)
        stats.avgMicros = (double)stats.totalMicros / stats.count;

    return stats;
}

static const char* eventName(PerfEvent event) {
    switch (event) {
        case PerfEvent::SymbolResolve: return "SymbolResolve";
        case PerfEvent::IltScan: return "IltScan";
        case PerfEvent::BreakpointBind: return "BreakpointBind";
        case PerfEvent::BreakpointSet: return "BreakpointSet";
        case PerfEvent::MemoryRead: return "MemoryRead";
        case PerfEvent::MemoryWrite: return "MemoryWrite";
        case PerfEvent::ModuleLoad: return "ModuleLoad";
        case PerfEvent::ExceptionDispatch: return "ExceptionDispatch";
        default: return "Unknown";
    }
}

void PerfStats::printReport() const {
    if (mMeasurements.empty()) {
        printf("No performance data collected.\n");
        return;
    }

    printf("\n=== Performance Report ===\n");
    printf("Total measurements: %zu\n\n", mMeasurements.size());

    // Report for each event type
    PerfEvent allEvents[] = {
        PerfEvent::SymbolResolve,
        PerfEvent::IltScan,
        PerfEvent::BreakpointBind,
        PerfEvent::BreakpointSet,
        PerfEvent::MemoryRead,
        PerfEvent::MemoryWrite,
        PerfEvent::ModuleLoad,
        PerfEvent::ExceptionDispatch,
    };

    for (auto event : allEvents) {
        auto stats = getStats(event);
        if (stats.count == 0)
            continue;

        printf("%s:\n", eventName(event));
        printf("  Count:       %lld\n", stats.count);
        printf("  Total:       %.3f ms\n", stats.totalMicros / 1000.0);
        printf("  Average:     %.3f µs\n", stats.avgMicros);
        printf("  Min:         %lld µs\n", stats.minMicros);
        printf("  Max:         %lld µs\n", stats.maxMicros);

        if (stats.cacheHits + stats.cacheMisses > 0) {
            double hitRate = (double)stats.cacheHits / (stats.cacheHits + stats.cacheMisses) * 100.0;
            printf("  Cache hits:  %lld (%.1f%%)\n", stats.cacheHits, hitRate);
            printf("  Cache miss:  %lld\n", stats.cacheMisses);
        }

        printf("\n");
    }

    fflush(stdout);
}

} // namespace Gleam
