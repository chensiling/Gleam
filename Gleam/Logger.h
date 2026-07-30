/**
 * @file Logger.h
 * @brief Encapsulated logging with configurable level
 *
 * Replaces global g_logLevel with instance-based state, enabling:
 * - Multiple independent Logger instances
 * - Per-instance log level configuration
 * - Better testability and isolation
 *
 * @see Log.h for backward-compatible wrapper
 */

#ifndef GLEAM_LOGGER_H
#define GLEAM_LOGGER_H

#include <cstdio>
#include <cstdarg>
#include <cstdint>

namespace Gleam {

/**
 * @brief Log levels for filtering output
 *
 * Messages are filtered based on the Logger's configured level.
 * Only messages at or above the configured level are output.
 */
enum class LogLevel {
    Debug,   ///< Verbose debugging information
    Info,    ///< General informational messages (default)
    Warn,    ///< Warning messages
    Error    ///< Error messages only
};

/**
 * @brief Logger class - encapsulates log level state and output methods
 *
 * Each Logger instance maintains its own log level filter and output state.
 * This enables multiple debugger instances to have independent logging
 * configurations without global state conflicts.
 *
 * @example Basic usage:
 * @code
 * Logger logger(LogLevel::Debug);
 * logger.debug("Connection established");
 * logger.info("Processing command: %s", cmd.c_str());
 * logger.warn("Symbol ambiguous: %d candidates", count);
 * logger.error("Failed to read memory at 0x%llX", address);
 * @endcode
 *
 * @example Conditional logging to avoid expensive formatting:
 * @code
 * if (logger.wouldLog(LogLevel::Debug)) {
 *     std::string details = buildExpensiveDebugInfo();
 *     logger.debug("Details: %s", details.c_str());
 * }
 * @endcode
 */
class Logger {
private:
    LogLevel mLevel;  ///< Current log level filter

public:
    /**
     * @brief Constructor with default level
     * @param level Initial log level (default: Info)
     */
    explicit Logger(LogLevel level = LogLevel::Info)
        : mLevel(level) {}

    /**
     * @brief Set the log level filter
     * @param level New log level (messages below this level are filtered out)
     */
    void setLevel(LogLevel level) { mLevel = level; }

    /**
     * @brief Get the current log level
     * @return Current log level filter
     */
    LogLevel getLevel() const { return mLevel; }

    /**
     * @brief Log a debug message (prefix: "[DEBUG] ")
     * @param fmt Printf-style format string
     * @param ... Format arguments
     */
    void debug(const char* fmt, ...);

    /**
     * @brief Log an info message (no prefix)
     * @param fmt Printf-style format string
     * @param ... Format arguments
     */
    void info(const char* fmt, ...);

    /**
     * @brief Log a warning message (prefix: "[WARN] ")
     * @param fmt Printf-style format string
     * @param ... Format arguments
     */
    void warn(const char* fmt, ...);

    /**
     * @brief Log an error message (prefix: "error: ")
     * @param fmt Printf-style format string
     * @param ... Format arguments
     */
    void error(const char* fmt, ...);

    /**
     * @brief Log a machine-readable event (prefix: "event ")
     * @param fmt Printf-style format string
     * @param ... Format arguments
     */
    void event(const char* fmt, ...);

    /**
     * @brief Log a debugger stop event in machine-readable format
     *
     * Format: "stop reason=<r> [details] rip=0x<addr> tid=<id>"
     *
     * @param reason Stop reason (e.g., "breakpoint", "step", "exception")
     * @param details Optional additional details (can be nullptr)
     * @param rip Instruction pointer at stop
     * @param tid Thread ID that stopped
     */
    void stop(const char* reason, const char* details, uint64_t rip, uint32_t tid);

    /**
     * @brief Check if a given level would be logged
     *
     * Use this to avoid expensive formatting for filtered messages.
     *
     * @param level Log level to check
     * @return true if messages at this level will be output
     *
     * @example
     * @code
     * if (logger.wouldLog(LogLevel::Debug)) {
     *     logger.debug("Expensive info: %s", buildExpensiveString().c_str());
     * }
     * @endcode
     */
    bool wouldLog(LogLevel level) const { return level >= mLevel; }

private:
    /**
     * @brief Internal logging implementation
     * @param level Message level
     * @param prefix Prefix to prepend (nullptr for none)
     * @param fmt Printf-style format string
     * @param args va_list of format arguments
     */
    void logImpl(LogLevel level, const char* prefix, const char* fmt, va_list args);
};

/**
 * @brief Global default logger instance (for transition period)
 *
 * This pointer is set by GleamDebugger's constructor to enable backward
 * compatibility with code using the old global logging functions.
 *
 * @warning Will be removed in future versions once all code is refactored
 * to use dependency injection. New code should accept a Logger& parameter.
 */
extern Logger* g_defaultLogger;

// ============================================================================
// Backward compatibility functions (use global default logger)
// ============================================================================

/**
 * @brief Log debug message using default logger
 * @param fmt Printf-style format string
 * @param ... Format arguments
 * @deprecated Use Logger instance methods instead
 */
inline void logDebug(const char* fmt, ...) {
    if (g_defaultLogger) {
        va_list args;
        va_start(args, fmt);
        char buf[4096];
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        g_defaultLogger->debug("%s", buf);
    }
}

/**
 * @brief Log info message using default logger
 * @param fmt Printf-style format string
 * @param ... Format arguments
 * @deprecated Use Logger instance methods instead
 */
inline void logInfo(const char* fmt, ...) {
    if (g_defaultLogger) {
        va_list args;
        va_start(args, fmt);
        char buf[4096];
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        g_defaultLogger->info("%s", buf);
    }
}

/**
 * @brief Log warning message using default logger
 * @param fmt Printf-style format string
 * @param ... Format arguments
 * @deprecated Use Logger instance methods instead
 */
inline void logWarn(const char* fmt, ...) {
    if (g_defaultLogger) {
        va_list args;
        va_start(args, fmt);
        char buf[4096];
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        g_defaultLogger->warn("%s", buf);
    }
}

/**
 * @brief Log error message using default logger
 * @param fmt Printf-style format string
 * @param ... Format arguments
 * @deprecated Use Logger instance methods instead
 */
inline void logError(const char* fmt, ...) {
    if (g_defaultLogger) {
        va_list args;
        va_start(args, fmt);
        char buf[4096];
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        g_defaultLogger->error("%s", buf);
    }
}

/**
 * @brief Log event message using default logger
 * @param fmt Printf-style format string
 * @param ... Format arguments
 * @deprecated Use Logger instance methods instead
 */
inline void logEvent(const char* fmt, ...) {
    if (g_defaultLogger) {
        va_list args;
        va_start(args, fmt);
        char buf[4096];
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        g_defaultLogger->event("%s", buf);
    }
}

/**
 * @brief Log stop event using default logger
 * @param reason Stop reason
 * @param details Optional details
 * @param rip Instruction pointer
 * @param tid Thread ID
 * @deprecated Use Logger instance methods instead
 */
inline void logStop(const char* reason, const char* details, uint64_t rip, uint32_t tid) {
    if (g_defaultLogger) {
        g_defaultLogger->stop(reason, details, rip, tid);
    }
}

} // namespace Gleam

// Convenience macros (using default logger)
/// @deprecated Use logger.debug() instead
#define LOG_DEBUG(...) Gleam::logDebug(__VA_ARGS__)
/// @deprecated Use logger.info() instead
#define LOG_INFO(...)  Gleam::logInfo(__VA_ARGS__)
/// @deprecated Use logger.warn() instead
#define LOG_WARN(...)  Gleam::logWarn(__VA_ARGS__)
/// @deprecated Use logger.error() instead
#define LOG_ERROR(...) Gleam::logError(__VA_ARGS__)
/// @deprecated Use logger.event() instead
#define LOG_EVENT(...) Gleam::logEvent(__VA_ARGS__)

#endif // GLEAM_LOGGER_H
