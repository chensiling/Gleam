// Logger - Encapsulated logging with configurable level
// Replaces global g_logLevel with instance-based state

#ifndef GLEAM_LOGGER_H
#define GLEAM_LOGGER_H

#include <cstdio>
#include <cstdarg>
#include <cstdint>

namespace Gleam {

// Log levels for filtering output
enum class LogLevel {
    Debug,   // Verbose debugging information
    Info,    // General informational messages
    Warn,    // Warning messages
    Error    // Error messages
};

// Logger class - encapsulates log level state and output methods
class Logger {
private:
    LogLevel mLevel;

public:
    // Constructor with default level
    explicit Logger(LogLevel level = LogLevel::Info)
        : mLevel(level) {}

    // Accessors
    void setLevel(LogLevel level) { mLevel = level; }
    LogLevel getLevel() const { return mLevel; }

    // Core logging methods
    void debug(const char* fmt, ...);
    void info(const char* fmt, ...);
    void warn(const char* fmt, ...);
    void error(const char* fmt, ...);
    void event(const char* fmt, ...);

    // Special stop event format
    void stop(const char* reason, const char* details, uint64_t rip, uint32_t tid);

    // Check if a level would be logged (for avoiding expensive formatting)
    bool wouldLog(LogLevel level) const { return level >= mLevel; }

private:
    // Internal implementation
    void logImpl(LogLevel level, const char* prefix, const char* fmt, va_list args);
};

// Global default logger instance (for transition period)
// Will be removed once all code is refactored to use dependency injection
extern Logger* g_defaultLogger;

// Convenience functions using default logger (backward compatibility)
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

inline void logStop(const char* reason, const char* details, uint64_t rip, uint32_t tid) {
    if (g_defaultLogger) {
        g_defaultLogger->stop(reason, details, rip, tid);
    }
}

} // namespace Gleam

// Convenience macros (using default logger)
#define LOG_DEBUG(...) Gleam::logDebug(__VA_ARGS__)
#define LOG_INFO(...)  Gleam::logInfo(__VA_ARGS__)
#define LOG_WARN(...)  Gleam::logWarn(__VA_ARGS__)
#define LOG_ERROR(...) Gleam::logError(__VA_ARGS__)
#define LOG_EVENT(...) Gleam::logEvent(__VA_ARGS__)

#endif // GLEAM_LOGGER_H
