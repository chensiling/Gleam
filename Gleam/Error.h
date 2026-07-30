// Error handling utilities for GleamDebugger
//
// Provides structured error reporting to replace inconsistent bool/exception
// return patterns throughout the codebase.

#ifndef GLEAM_ERROR_H
#define GLEAM_ERROR_H

#include <string>
#include <variant>
#include <optional>
#include <stdexcept>

namespace Gleam {

// Error categories
enum class ErrorCategory {
    None,
    Memory,           // Memory read/write failures
    Symbol,           // Symbol resolution failures
    Process,          // Process/thread management failures
    Breakpoint,       // Breakpoint set/remove failures
    Exception,        // Exception handling failures
    Internal,         // Internal logic errors
    System,           // System API failures
};

// Detailed error information
struct Error {
    ErrorCategory category;
    std::string message;
    int systemCode;  // GetLastError() or similar
    std::string context;  // Additional context (e.g., address, symbol name)

    Error() : category(ErrorCategory::None), systemCode(0) {}

    Error(ErrorCategory cat, const std::string& msg, int code = 0, const std::string& ctx = "")
        : category(cat), message(msg), systemCode(code), context(ctx) {}

    // Check if this represents an error
    bool hasError() const { return category != ErrorCategory::None; }

    // Format error for display
    std::string format() const;
};

// Result<T> - either a value or an error
template<typename T>
class Result {
private:
    std::variant<T, Error> mData;

public:
    // Construct with success value
    Result(const T& value) : mData(value) {}
    Result(T&& value) : mData(std::move(value)) {}

    // Construct with error
    Result(const Error& error) : mData(error) {}
    Result(Error&& error) : mData(std::move(error)) {}

    // Check if result is successful
    bool isOk() const { return std::holds_alternative<T>(mData); }
    bool isError() const { return std::holds_alternative<Error>(mData); }

    // Get value (only valid if isOk())
    const T& value() const { return std::get<T>(mData); }
    T& value() { return std::get<T>(mData); }

    // Get error (only valid if isError())
    const Error& error() const { return std::get<Error>(mData); }
    Error& error() { return std::get<Error>(mData); }

    // Get value or default
    T valueOr(const T& defaultValue) const {
        return isOk() ? value() : defaultValue;
    }

    // Unwrap value (throws if error)
    T unwrap() const {
        if (isError()) {
            throw std::runtime_error("Result::unwrap() called on error: " + error().format());
        }
        return value();
    }
};

// Result<void> specialization for operations that don't return a value
template<>
class Result<void> {
private:
    std::optional<Error> mError;

public:
    // Construct with success
    Result() : mError(std::nullopt) {}

    // Construct with error
    Result(const Error& error) : mError(error) {}
    Result(Error&& error) : mError(std::move(error)) {}

    // Check if result is successful
    bool isOk() const { return !mError.has_value(); }
    bool isError() const { return mError.has_value(); }

    // Get error (only valid if isError())
    const Error& error() const { return *mError; }
    Error& error() { return *mError; }

    // Unwrap (throws if error)
    void unwrap() const {
        if (isError()) {
            throw std::runtime_error("Result<void>::unwrap() called on error: " + error().format());
        }
    }
};

// Helper macros for creating errors
#define GLEAM_ERROR(category, message) \
    Gleam::Error(Gleam::ErrorCategory::category, message, 0, "")

#define GLEAM_ERROR_CTX(category, message, context) \
    Gleam::Error(Gleam::ErrorCategory::category, message, 0, context)

#define GLEAM_ERROR_SYS(category, message, code) \
    Gleam::Error(Gleam::ErrorCategory::category, message, code, "")

#define GLEAM_ERROR_FULL(category, message, code, context) \
    Gleam::Error(Gleam::ErrorCategory::category, message, code, context)

// Helper to create success Result<void>
inline Result<void> Ok() { return Result<void>(); }

// Helper to create error Result<void>
inline Result<void> Err(const Error& error) { return Result<void>(error); }

// Convert Windows error code to string
std::string formatWindowsError(int errorCode);

} // namespace Gleam

#endif // GLEAM_ERROR_H
