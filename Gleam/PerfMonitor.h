// PerfMonitor - Encapsulated performance monitoring
// Replaces global g_perfStats with instance-based state

#ifndef GLEAM_PERF_MONITOR_H
#define GLEAM_PERF_MONITOR_H

#include "Performance.h"

namespace Gleam {

// PerfMonitor - encapsulates performance statistics collection
class PerfMonitor {
private:
    PerfStats mStats;

public:
    PerfMonitor() = default;

    // Accessors
    PerfStats& stats() { return mStats; }
    const PerfStats& stats() const { return mStats; }

    // Enable/disable monitoring
    void enable() { mStats.enable(); }
    void disable() { mStats.disable(); }
    bool isEnabled() const { return mStats.isEnabled(); }

    // Record an event
    void record(PerfEvent event, int64_t durationMicros, bool cacheHit = false,
                const std::string& details = "") {
        mStats.record(event, durationMicros, cacheHit, details);
    }

    // Clear collected data
    void clear() { mStats.clear(); }

    // Get statistics
    PerfStats::EventStats getStats(PerfEvent event) const {
        return mStats.getStats(event);
    }

    // Print report
    void printReport() const {
        mStats.printReport();
    }

    // Get all measurements
    const std::vector<PerfMeasurement>& measurements() const {
        return mStats.measurements();
    }
};

// Global default perf monitor (for transition period)
// Will be removed once all code is refactored to use dependency injection
extern PerfMonitor* g_defaultPerfMonitor;

// Helper class for RAII-style performance recording
class PerfRecorder {
private:
    PerformanceTimer mTimer;
    PerfEvent mEvent;
    bool mCacheHit;
    std::string mDetails;
    PerfMonitor* mMonitor;

public:
    PerfRecorder(PerfMonitor* monitor, PerfEvent event, const std::string& details = "")
        : mEvent(event), mCacheHit(false), mDetails(details), mMonitor(monitor)
    {
        if (mMonitor && mMonitor->isEnabled()) {
            mTimer.start();
        }
    }

    // For backward compatibility with global default
    PerfRecorder(PerfEvent event, const std::string& details = "")
        : PerfRecorder(g_defaultPerfMonitor, event, details) {}

    void setCacheHit(bool hit) { mCacheHit = hit; }

    ~PerfRecorder() {
        if (mMonitor && mMonitor->isEnabled()) {
            int64_t duration = mTimer.elapsedMicros();
            mMonitor->record(mEvent, duration, mCacheHit, mDetails);
        }
    }
};

} // namespace Gleam

// Convenience macros (using default monitor)
#define PERF_RECORD(event, details) Gleam::PerfRecorder _perf_recorder_##__LINE__(event, details)
#define PERF_CACHE_HIT() _perf_recorder_##__LINE__.setCacheHit(true)

#endif // GLEAM_PERF_MONITOR_H
