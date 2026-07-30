// Logging utility for unified output management
// DEPRECATED: Use Logger.h instead. This file is kept for backward compatibility.
// It now forwards to the new Logger class.

#ifndef GLEAM_LOG_H
#define GLEAM_LOG_H

#include "Logger.h"

// Re-export for backward compatibility
namespace Gleam {
    using ::Gleam::LogLevel;
    using ::Gleam::logDebug;
    using ::Gleam::logInfo;
    using ::Gleam::logWarn;
    using ::Gleam::logError;
    using ::Gleam::logEvent;
    using ::Gleam::logStop;
}

// Convenience macros (unchanged)
#define LOG_DEBUG(...) Gleam::logDebug(__VA_ARGS__)
#define LOG_INFO(...)  Gleam::logInfo(__VA_ARGS__)
#define LOG_WARN(...)  Gleam::logWarn(__VA_ARGS__)
#define LOG_ERROR(...) Gleam::logError(__VA_ARGS__)
#define LOG_EVENT(...) Gleam::logEvent(__VA_ARGS__)

#endif // GLEAM_LOG_H
