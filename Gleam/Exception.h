// Exception handling strategy and utilities

#ifndef GLEAM_EXCEPTION_H
#define GLEAM_EXCEPTION_H

#include "Error.h"
#include "Log.h"
#include <string>
#include <exception>
#include <functional>

namespace Gleam {

// Exception policy for GleamDebugger
// By default, we use Result<T> for expected errors and avoid throwing exceptions
// This header provides utilities for the rare cases where exceptions are needed

// Base exception class for Gleam-specific exceptions
class GleamException : public std::exception {
private:
    Error mError;
    std::string mWhat;

public:
    explicit GleamException(const Error& error)
        : mError(error) {
        mWhat = error.format();
    }

    GleamException(ErrorCategory category, const std::string& message,
                   int systemCode = 0, const std::string& context = "")
        : mError(category, message, systemCode, context) {
        mWhat = mError.format();
    }

    const char* what() const noexcept override {
        return mWhat.c_str();
    }

    const Error& error() const noexcept {
        return mError;
    }

    ErrorCategory category() const noexcept {
        return mError.category;
    }
};

// Specific exception types for different error categories
class MemoryException : public GleamException {
public:
    explicit MemoryException(const std::string& message, const std::string& context = "")
        : GleamException(ErrorCategory::Memory, message, 0, context) {}
};

class SymbolException : public GleamException {
public:
    explicit SymbolException(const std::string& message, const std::string& context = "")
        : GleamException(ErrorCategory::Symbol, message, 0, context) {}
};

class ProcessException : public GleamException {
public:
    explicit ProcessException(const std::string& message, int systemCode = 0)
        : GleamException(ErrorCategory::Process, message, systemCode) {}
};

class BreakpointException : public GleamException {
public:
    explicit BreakpointException(const std::string& message, const std::string& context = "")
        : GleamException(ErrorCategory::Breakpoint, message, 0, context) {}
};

// Exception-safe wrappers

// Try-catch wrapper that converts exceptions to Result<T>
template<typename T, typename Func>
Result<T> tryCatch(Func func, const char* operation = "operation") {
    try {
        return Result<T>(func());
    }
    catch (const GleamException& e) {
        logError("Exception in %s: %s", operation, e.what());
        return Result<T>(e.error());
    }
    catch (const std::exception& e) {
        logError("Unexpected exception in %s: %s", operation, e.what());
        return Result<T>(Error(ErrorCategory::Internal, e.what()));
    }
    catch (...) {
        logError("Unknown exception in %s", operation);
        return Result<T>(Error(ErrorCategory::Internal, "Unknown exception"));
    }
}

// Try-catch wrapper for void functions
template<typename Func>
Result<bool> tryCatchVoid(Func func, const char* operation = "operation") {
    try {
        func();
        return Result<bool>(true);
    }
    catch (const GleamException& e) {
        logError("Exception in %s: %s", operation, e.what());
        return Result<bool>(e.error());
    }
    catch (const std::exception& e) {
        logError("Unexpected exception in %s: %s", operation, e.what());
        return Result<bool>(Error(ErrorCategory::Internal, e.what()));
    }
    catch (...) {
        logError("Unknown exception in %s", operation);
        return Result<bool>(Error(ErrorCategory::Internal, "Unknown exception"));
    }
}

// Exception guard - catches and logs exceptions at scope boundaries
class ExceptionGuard {
private:
    const char* mOperation;
    bool mThrow;

public:
    explicit ExceptionGuard(const char* operation, bool rethrow = false)
        : mOperation(operation), mThrow(rethrow) {}

    ~ExceptionGuard() noexcept(!mThrow) {
        // Check if we're unwinding due to an exception
        if (std::uncaught_exception()) {
            try {
                // Try to log the current exception
                std::exception_ptr p = std::current_exception();
                if (p) {
                    std::rethrow_exception(p);
                }
            }
            catch (const GleamException& e) {
                logError("Exception in %s: %s", mOperation, e.what());
                if (mThrow) throw;
            }
            catch (const std::exception& e) {
                logError("Unexpected exception in %s: %s", mOperation, e.what());
                if (mThrow) throw;
            }
            catch (...) {
                logError("Unknown exception in %s", mOperation);
                if (mThrow) throw;
            }
        }
    }
};

// Fatal error handler - for unrecoverable errors
[[noreturn]] inline void fatalError(const char* message) {
    logError("FATAL: %s", message);
    std::abort();
}

[[noreturn]] inline void fatalError(const std::string& message) {
    fatalError(message.c_str());
}

// Assertion with error message
#ifdef NDEBUG
    #define GLEAM_ASSERT(condition, message) ((void)0)
#else
    #define GLEAM_ASSERT(condition, message) \
        do { \
            if (!(condition)) { \
                Gleam::logError("Assertion failed: %s (%s:%d)", message, __FILE__, __LINE__); \
                std::abort(); \
            } \
        } while (0)
#endif

// Contract checking
#define GLEAM_REQUIRE(condition, message) \
    do { \
        if (!(condition)) { \
            Gleam::logError("Precondition failed: %s (%s:%d)", message, __FILE__, __LINE__); \
            throw Gleam::GleamException(Gleam::ErrorCategory::Internal, message); \
        } \
    } while (0)

#define GLEAM_ENSURE(condition, message) \
    do { \
        if (!(condition)) { \
            Gleam::logError("Postcondition failed: %s (%s:%d)", message, __FILE__, __LINE__); \
            throw Gleam::GleamException(Gleam::ErrorCategory::Internal, message); \
        } \
    } while (0)

// Panic macro for unrecoverable errors
#define GLEAM_PANIC(message) \
    do { \
        Gleam::logError("PANIC: %s (%s:%d)", message, __FILE__, __LINE__); \
        std::abort(); \
    } while (0)

// Unreachable code marker
#define GLEAM_UNREACHABLE() \
    GLEAM_PANIC("Unreachable code reached")

// Recovery strategies

// Retry with exponential backoff
template<typename T, typename Func>
Result<T> retryWithBackoff(Func func, int maxRetries = 3, int initialDelayMs = 10) {
    int delay = initialDelayMs;

    for (int attempt = 0; attempt <= maxRetries; attempt++) {
        auto result = tryCatch<T>(func, "retry operation");

        if (result.isOk()) {
            return result;
        }

        if (attempt < maxRetries) {
            logWarn("Retry attempt %d/%d after %dms", attempt + 1, maxRetries, delay);
            Sleep(delay);
            delay *= 2; // Exponential backoff
        }
    }

    return Result<T>(Error(ErrorCategory::Internal, "Max retries exceeded"));
}

// Fallback chain - try multiple alternatives
template<typename T>
Result<T> tryFallbacks(std::initializer_list<std::function<Result<T>()>> alternatives) {
    Error lastError(ErrorCategory::Internal, "No alternatives succeeded");

    for (const auto& alt : alternatives) {
        auto result = alt();
        if (result.isOk()) {
            return result;
        }
        lastError = result.error();
    }

    return Result<T>(lastError);
}

} // namespace Gleam

#endif // GLEAM_EXCEPTION_H
