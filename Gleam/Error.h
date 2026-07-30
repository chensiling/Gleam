// Error handling utilities for GleamDebugger

#ifndef GLEAM_ERROR_H
#define GLEAM_ERROR_H

#include <string>

namespace Gleam {

// Error categories
enum class ErrorCategory {
    None,
    Memory,
    Symbol,
    Process,
    Breakpoint,
    Exception,
    Internal,
    WindowsSystem,  // Renamed to avoid conflict with System macro
};

// Detailed error information
struct Error {
    ErrorCategory category;
    std::string message;
    int systemCode;
    std::string context;

    Error() : category(ErrorCategory::None), systemCode(0) {}

    Error(ErrorCategory cat, const std::string& msg, int code = 0, const std::string& ctx = "")
        : category(cat), message(msg), systemCode(code), context(ctx) {}

    bool hasError() const { return category != ErrorCategory::None; }
    std::string format() const;
};

// Simple Result<T> - either has value or error
template<typename T>
class Result {
private:
    T mValue;
    Error mError;
    bool mHasValue;

public:
    Result(const T& value) : mValue(value), mHasValue(true) {}
    Result(const Error& error) : mError(error), mHasValue(false) {}

    bool isOk() const { return mHasValue; }
    bool isError() const { return !mHasValue; }

    const T& value() const { return mValue; }
    T& value() { return mValue; }

    const Error& error() const { return mError; }

    T valueOr(const T& defaultValue) const {
        return mHasValue ? mValue : defaultValue;
    }
};

// Helper macros
#define GLEAM_ERROR(category, message) \
    Gleam::Error(Gleam::ErrorCategory::category, message, 0, "")

#define GLEAM_ERROR_CTX(category, message, context) \
    Gleam::Error(Gleam::ErrorCategory::category, message, 0, context)

#define GLEAM_ERROR_SYS(category, message, code) \
    Gleam::Error(Gleam::ErrorCategory::category, message, code, "")

// Convert Windows error code to string
std::string formatWindowsError(int errorCode);

} // namespace Gleam

#endif // GLEAM_ERROR_H
