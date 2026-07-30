// Logging utility for unified output management
// Replaces scattered printf + fflush calls throughout the codebase

#ifndef GLEAM_LOG_H
#define GLEAM_LOG_H

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

// Global log level filter (can be set at runtime)
extern LogLevel g_logLevel;

// Core logging function
inline void logImpl(LogLevel level, const char* prefix, const char* fmt, va_list args) {
    if (level < g_logLevel) {
        return; // Filtered out
    }

    if (prefix) {
        printf("%s", prefix);
    }
    vprintf(fmt, args);
    printf("\n");
    fflush(stdout);
}

// Convenience functions for each log level

inline void logDebug(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    logImpl(LogLevel::Debug, "[DEBUG] ", fmt, args);
    va_end(args);
}

inline void logInfo(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    logImpl(LogLevel::Info, nullptr, fmt, args);
    va_end(args);
}

inline void logWarn(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    logImpl(LogLevel::Warn, "[WARN] ", fmt, args);
    va_end(args);
}

inline void logError(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    logImpl(LogLevel::Error, "error: ", fmt, args);
    va_end(args);
}

// Event logging (machine-readable format)
inline void logEvent(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    logImpl(LogLevel::Info, "event ", fmt, args);
    va_end(args);
}

// Stop event (special format for debugger stops)
inline void logStop(const char* reason, const char* details, uint64_t rip, uint32_t tid) {
    printf("stop reason=%s%s%s rip=0x%llX tid=%u\n",
           reason,
           details ? " " : "",
           details ? details : "",
           (unsigned long long)rip,
           tid);
    fflush(stdout);
}

} // namespace Gleam

// Convenience macros (optional, can use functions directly)
#define LOG_DEBUG(...) Gleam::logDebug(__VA_ARGS__)
#define LOG_INFO(...)  Gleam::logInfo(__VA_ARGS__)
#define LOG_WARN(...)  Gleam::logWarn(__VA_ARGS__)
#define LOG_ERROR(...) Gleam::logError(__VA_ARGS__)
#define LOG_EVENT(...) Gleam::logEvent(__VA_ARGS__)

#endif // GLEAM_LOG_H
