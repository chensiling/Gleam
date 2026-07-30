#include "Logger.h"
#include <cstring>

namespace Gleam {

// Global default logger pointer (transition period)
Logger* g_defaultLogger = nullptr;

// Implementation of Logger methods

void Logger::logImpl(LogLevel level, const char* prefix, const char* fmt, va_list args) {
    if (level < mLevel) {
        return; // Filtered out
    }

    if (prefix) {
        printf("%s", prefix);
    }
    vprintf(fmt, args);
    printf("\n");
    fflush(stdout);
}

void Logger::debug(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    logImpl(LogLevel::Debug, "[DEBUG] ", fmt, args);
    va_end(args);
}

void Logger::info(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    logImpl(LogLevel::Info, nullptr, fmt, args);
    va_end(args);
}

void Logger::warn(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    logImpl(LogLevel::Warn, "[WARN] ", fmt, args);
    va_end(args);
}

void Logger::error(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    logImpl(LogLevel::Error, "error: ", fmt, args);
    va_end(args);
}

void Logger::event(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    logImpl(LogLevel::Info, "event ", fmt, args);
    va_end(args);
}

void Logger::stop(const char* reason, const char* details, uint64_t rip, uint32_t tid) {
    if (LogLevel::Info < mLevel) {
        return;
    }

    printf("stop reason=%s%s%s rip=0x%llX tid=%u\n",
           reason,
           details ? " " : "",
           details ? details : "",
           (unsigned long long)rip,
           tid);
    fflush(stdout);
}

} // namespace Gleam
