/**
 * @file PerfMonitor.h
 * @brief Instance-based performance monitoring with RAII recording
 *
 * Provides encapsulated performance statistics collection to support
 * multi-instance debugging scenarios. Replaces the global g_perfStats
 * pattern with dependency injection while maintaining backward compatibility.
 *
 * Key features:
 * - RAII-based automatic timing via PerfRecorder
 * - Cache hit tracking per event
 * - Statistical aggregation (count, min, max, avg, cache hit ratio)
 * - Optional global default for transition period
 *
 * @see Performance.h for PerfEvent enumeration and PerfStats implementation
 */

#ifndef GLEAM_PERF_MONITOR_H
#define GLEAM_PERF_MONITOR_H

#include "Performance.h"

namespace Gleam {

/**
 * @brief Performance statistics collector
 *
 * Encapsulates performance measurement state to support multi-instance
 * scenarios where each GleamDebugger has its own independent metrics.
 *
 * **Thread safety:** Not thread-safe. Caller must synchronize if recording
 * from multiple threads.
 *
 * @example Basic usage:
 * @code
 * PerfMonitor monitor;
 * monitor.enable();
 *
 * {
 *     PerfRecorder rec(&monitor, PerfEvent::SymbolResolve, "kernel32!CreateFileW");
 *     // ... perform work ...
 *     rec.setCacheHit(true);  // Optional: mark as cache hit
 * }  // Auto-records on destruction
 *
 * auto stats = monitor.getStats(PerfEvent::SymbolResolve);
 * printf("Avg: %.2f us, Cache hit ratio: %.1f%%\n",
 *        stats.averageMicros, stats.cacheHitRatio * 100.0);
 * @endcode
 *
 * @example Integration with GleamDebugger:
 * @code
 * class GleamDebugger {
 *     PerfMonitor* mPerfMonitor;
 * public:
 *     GleamDebugger() : mPerfMonitor(new PerfMonitor()) {
 *         mPerfMonitor->enable();
 *     }
 *     PerfMonitor& perfMonitor() { return *mPerfMonitor; }
 * };
 * @endcode
 */
class PerfMonitor {
private:
    PerfStats mStats;  ///< Underlying statistics storage

public:
    PerfMonitor() = default;

    /**
     * @brief Access the raw statistics object
     * @return Reference to the internal PerfStats
     * @note Advanced usage only; prefer the higher-level methods
     */
    PerfStats& stats() { return mStats; }

    /** @brief Const access to statistics */
    const PerfStats& stats() const { return mStats; }

    /**
     * @brief Enable performance monitoring
     *
     * When disabled, PerfRecorder calls have minimal overhead
     * (no timing, no recording).
     */
    void enable() { mStats.enable(); }

    /**
     * @brief Disable performance monitoring
     *
     * Disabling reduces overhead to near-zero. Recorded data is preserved
     * until clear() is called.
     */
    void disable() { mStats.disable(); }

    /**
     * @brief Check if monitoring is enabled
     * @return true if performance recording is active
     */
    bool isEnabled() const { return mStats.isEnabled(); }

    /**
     * @brief Record a performance event
     *
     * Typically called automatically by PerfRecorder destructor.
     * Manual calls are supported for non-RAII scenarios.
     *
     * @param event The event type being measured
     * @param durationMicros Elapsed time in microseconds
     * @param cacheHit Whether this operation hit a cache
     * @param details Optional context (e.g., symbol name, address)
     */
    void record(PerfEvent event, int64_t durationMicros, bool cacheHit = false,
                const std::string& details = "") {
        mStats.record(event, durationMicros, cacheHit, details);
    }

    /**
     * @brief Clear all collected measurements
     *
     * Resets counts and statistics for all event types.
     * Does not change enabled/disabled state.
     */
    void clear() { mStats.clear(); }

    /**
     * @brief Get aggregated statistics for an event type
     *
     * @param event The event type to query
     * @return EventStats with count, min, max, average, cache hit ratio
     *
     * @example
     * @code
     * auto stats = monitor.getStats(PerfEvent::SymbolResolve);
     * if (stats.count > 0) {
     *     printf("Symbol resolution: %d calls, avg %.2f us, %.1f%% cached\n",
     *            stats.count, stats.averageMicros, stats.cacheHitRatio * 100.0);
     * }
     * @endcode
     */
    PerfStats::EventStats getStats(PerfEvent event) const {
        return mStats.getStats(event);
    }

    /**
     * @brief Print a formatted report to stdout
     *
     * Outputs a table with all event types, counts, timing statistics,
     * and cache hit ratios.
     */
    void printReport() const {
        mStats.printReport();
    }

    /**
     * @brief Get all raw measurements
     *
     * @return Vector of individual PerfMeasurement records
     * @note For detailed analysis or export; prefer getStats() for aggregates
     */
    const std::vector<PerfMeasurement>& measurements() const {
        return mStats.measurements();
    }
};

/**
 * @brief Global default performance monitor (transition period)
 *
 * Provides backward compatibility for code that hasn't been refactored
 * to use dependency injection. New code should pass PerfMonitor* explicitly.
 *
 * @deprecated Use GleamDebugger::perfMonitor() instead
 * @warning In multi-instance scenarios, this points to only one instance
 */
extern PerfMonitor* g_defaultPerfMonitor;

/**
 * @brief RAII performance recorder
 *
 * Automatically starts a timer on construction and records the elapsed
 * time to a PerfMonitor on destruction. This ensures measurements are
 * captured even if the scope exits early (return, exception, etc.).
 *
 * **Usage pattern:**
 * @code
 * void expensiveOperation(PerfMonitor* monitor) {
 *     PerfRecorder perf(monitor, PerfEvent::SymbolResolve, "myFunction");
 *
 *     // ... do work ...
 *
 *     if (foundInCache) {
 *         perf.setCacheHit(true);
 *     }
 * }  // Auto-records here
 * @endcode
 *
 * **Macro convenience:**
 * @code
 * void foo() {
 *     PERF_RECORD(PerfEvent::SymbolResolve, "foo");
 *     // ... work ...
 *     if (cached) PERF_CACHE_HIT();
 * }
 * @endcode
 *
 * @note If the monitor is disabled, the recorder has minimal overhead
 *       (no heap allocation, no timer start).
 */
class PerfRecorder {
private:
    PerformanceTimer mTimer;   ///< High-resolution timer
    PerfEvent mEvent;          ///< Event type being measured
    bool mCacheHit;            ///< Whether this operation hit a cache
    std::string mDetails;      ///< Context string (e.g., symbol name)
    PerfMonitor* mMonitor;     ///< Target monitor (null = no-op)

public:
    /**
     * @brief Construct and start timing
     *
     * @param monitor Target PerfMonitor (nullptr = no-op recorder)
     * @param event Event type to measure
     * @param details Optional context string for debugging
     *
     * @note Timer starts immediately if monitor is enabled
     */
    PerfRecorder(PerfMonitor* monitor, PerfEvent event, const std::string& details = "")
        : mEvent(event), mCacheHit(false), mDetails(details), mMonitor(monitor)
    {
        if (mMonitor && mMonitor->isEnabled()) {
            mTimer.start();
        }
    }

    /**
     * @brief Construct using global default monitor
     *
     * @param event Event type to measure
     * @param details Optional context string
     *
     * @deprecated Use the monitor-accepting constructor with GleamDebugger::perfMonitor()
     */
    PerfRecorder(PerfEvent event, const std::string& details = "")
        : PerfRecorder(g_defaultPerfMonitor, event, details) {}

    /**
     * @brief Mark this operation as a cache hit
     *
     * Call this before the recorder destructs if the operation
     * successfully used a cache.
     *
     * @param hit true if cache was hit
     */
    void setCacheHit(bool hit) { mCacheHit = hit; }

    /**
     * @brief Destructor: stop timer and record measurement
     *
     * Automatically captures elapsed time and records to the monitor.
     * Safe to call even if monitor is null or disabled.
     */
    ~PerfRecorder() {
        if (mMonitor && mMonitor->isEnabled()) {
            int64_t duration = mTimer.elapsedMicros();
            mMonitor->record(mEvent, duration, mCacheHit, mDetails);
        }
    }
};

} // namespace Gleam

/**
 * @brief Convenience macro for creating a PerfRecorder
 *
 * Creates a uniquely-named PerfRecorder instance that lives until
 * end of scope. Uses the global default monitor.
 *
 * @param event PerfEvent enumeration value
 * @param details String literal or expression
 *
 * @example
 * @code
 * void processCommand() {
 *     PERF_RECORD(PerfEvent::CommandExecution, "attach");
 *     // ... work ...
 *     if (cached) PERF_CACHE_HIT();
 * }
 * @endcode
 *
 * @deprecated Prefer explicit PerfRecorder with passed monitor
 */
#define PERF_RECORD(event, details) Gleam::PerfRecorder _perf_recorder_##__LINE__(event, details)

/**
 * @brief Mark the current PERF_RECORD scope as a cache hit
 *
 * Must be called after PERF_RECORD in the same scope.
 *
 * @example
 * @code
 * PERF_RECORD(PerfEvent::SymbolResolve, "kernel32!CreateFileW");
 * uint64_t addr = lookupCache(spec);
 * if (addr) {
 *     PERF_CACHE_HIT();
 *     return addr;
 * }
 * @endcode
 */
#define PERF_CACHE_HIT() _perf_recorder_##__LINE__.setCacheHit(true)

#endif // GLEAM_PERF_MONITOR_H
