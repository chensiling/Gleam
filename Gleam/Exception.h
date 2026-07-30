/**
 * @file Exception.h
 * @brief Exception handling strategy and recovery utilities
 *
 * **Design Philosophy:**
 * GleamDebugger uses Result<T> for expected errors and reserves exceptions
 * for truly exceptional situations. This file provides:
 * - Typed exception hierarchy (GleamException, MemoryException, etc.)
 * - Exception-to-Result conversion utilities (tryCatch, tryCatchVoid)
 * - Recovery strategies (retry with backoff, fallback chains)
 * - Contract checking macros (GLEAM_REQUIRE, GLEAM_ENSURE, GLEAM_ASSERT)
 *
 * **When to use exceptions vs Result<T>:**
 * - Result<T>: Expected errors (file not found, symbol not resolved, parsing failed)
 * - Exceptions: Unexpected errors (out of memory, internal invariant violated)
 *
 * @see Error.h for the Result<T> monad and error representation
 */

#ifndef GLEAM_EXCEPTION_H
#define GLEAM_EXCEPTION_H

#include "Error.h"
#include "Log.h"
#include <string>
#include <exception>
#include <functional>

namespace Gleam {

/**
 * @brief Base exception class for all Gleam-specific exceptions
 *
 * Wraps an Error object to provide rich error context (category, message,
 * system error code, context string) while conforming to std::exception.
 *
 * @example
 * @code
 * throw GleamException(ErrorCategory::Memory,
 *                      "Failed to read process memory",
 *                      ERROR_PARTIAL_COPY,
 *                      "address=0x12345678");
 * @endcode
 */
class GleamException : public std::exception {
private:
    Error mError;          ///< Wrapped error details
    std::string mWhat;     ///< Cached what() string

public:
    /**
     * @brief Construct from an existing Error
     * @param error Error object to wrap
     */
    explicit GleamException(const Error& error)
        : mError(error) {
        mWhat = error.format();
    }

    /**
     * @brief Construct from error components
     * @param category Error category
     * @param message Human-readable error message
     * @param systemCode OS error code (0 = not applicable)
     * @param context Additional context string
     */
    GleamException(ErrorCategory category, const std::string& message,
                   int systemCode = 0, const std::string& context = "")
        : mError(category, message, systemCode, context) {
        mWhat = mError.format();
    }

    /**
     * @brief Get the formatted error message
     * @return C-string describing the error
     */
    const char* what() const noexcept override {
        return mWhat.c_str();
    }

    /**
     * @brief Access the wrapped Error object
     * @return Reference to the internal Error
     */
    const Error& error() const noexcept {
        return mError;
    }

    /**
     * @brief Get the error category
     * @return ErrorCategory value
     */
    ErrorCategory category() const noexcept {
        return mError.category;
    }
};

/**
 * @brief Exception for memory access errors
 *
 * @example
 * @code
 * throw MemoryException("Failed to read 8 bytes", "address=0x7FFF12345678");
 * @endcode
 */
class MemoryException : public GleamException {
public:
    explicit MemoryException(const std::string& message, const std::string& context = "")
        : GleamException(ErrorCategory::Memory, message, 0, context) {}
};

/**
 * @brief Exception for symbol resolution errors
 *
 * @example
 * @code
 * throw SymbolException("Symbol not found", "kernel32!NonExistentFunction");
 * @endcode
 */
class SymbolException : public GleamException {
public:
    explicit SymbolException(const std::string& message, const std::string& context = "")
        : GleamException(ErrorCategory::Symbol, message, 0, context) {}
};

/**
 * @brief Exception for process-level errors
 *
 * @example
 * @code
 * throw ProcessException("Failed to attach to process", GetLastError());
 * @endcode
 */
class ProcessException : public GleamException {
public:
    explicit ProcessException(const std::string& message, int systemCode = 0)
        : GleamException(ErrorCategory::Process, message, systemCode) {}
};

/**
 * @brief Exception for breakpoint errors
 *
 * @example
 * @code
 * throw BreakpointException("Failed to set hardware breakpoint", "DR0 already in use");
 * @endcode
 */
class BreakpointException : public GleamException {
public:
    explicit BreakpointException(const std::string& message, const std::string& context = "")
        : GleamException(ErrorCategory::Breakpoint, message, 0, context) {}
};

// -----------------------------------------------------------------------
// Exception-safe wrappers
// -----------------------------------------------------------------------

/**
 * @brief Convert exception-throwing code to Result<T>
 *
 * Wraps a function that may throw exceptions and converts them to
 * Result<T> for uniform error handling.
 *
 * @tparam T Return type of the function
 * @tparam Func Callable type
 * @param func Function to execute (may throw)
 * @param operation Name for logging (shown in error messages)
 * @return Result<T> containing the value or converted exception
 *
 * @example
 * @code
 * Result<uint64_t> addr = tryCatch<uint64_t>([&]() {
 *     return resolveSymbolMayThrow("kernel32!CreateFileW");
 * }, "symbol resolution");
 *
 * if (addr.isOk()) {
 *     printf("Resolved: 0x%llX\n", addr.value());
 * } else {
 *     printf("Error: %s\n", addr.error().message.c_str());
 * }
 * @endcode
 */
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

/**
 * @brief Convert exception-throwing void function to Result<bool>
 *
 * Variant of tryCatch for void functions. Returns Result<bool> where
 * true indicates success.
 *
 * @tparam Func Callable type
 * @param func Function to execute (may throw)
 * @param operation Name for logging
 * @return Result<bool> - Ok(true) on success, Error on exception
 *
 * @example
 * @code
 * Result<bool> result = tryCatchVoid([&]() {
 *     performRiskyOperation();
 * }, "risky operation");
 *
 * if (!result.isOk()) {
 *     printf("Failed: %s\n", result.error().message.c_str());
 * }
 * @endcode
 */
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

/**
 * @brief RAII exception guard for scope boundaries
 *
 * Catches and logs any exception that unwinds through the guarded scope.
 * Useful for destructors and callback boundaries where exceptions cannot
 * propagate.
 *
 * @example
 * @code
 * ~MyClass() {
 *     ExceptionGuard guard("MyClass destructor");
 *     cleanupResources();  // May throw, but guard will log it
 * }
 * @endcode
 *
 * @warning Never rethrows from destructor (always noexcept)
 */
class ExceptionGuard {
private:
    const char* mOperation;  ///< Operation name for logging
    bool mThrow;             ///< Unused (kept for ABI compatibility)

public:
    /**
     * @brief Construct an exception guard
     * @param operation Name of the operation being guarded
     * @param rethrow Unused parameter (kept for backward compatibility)
     */
    explicit ExceptionGuard(const char* operation, bool rethrow = false)
        : mOperation(operation), mThrow(rethrow) {}

    /**
     * @brief Destructor: catches and logs any active exception
     *
     * Uses std::uncaught_exception() to detect if we're unwinding due
     * to an exception. If so, attempts to log the exception details.
     */
    ~ExceptionGuard() noexcept {
        if (std::uncaught_exception()) {
            try {
                std::exception_ptr p = std::current_exception();
                if (p) {
                    std::rethrow_exception(p);
                }
            }
            catch (const GleamException& e) {
                logError("Exception in %s: %s", mOperation, e.what());
            }
            catch (const std::exception& e) {
                logError("Unexpected exception in %s: %s", mOperation, e.what());
            }
            catch (...) {
                logError("Unknown exception in %s", mOperation);
            }
        }
    }
};

/**
 * @brief Abort with a fatal error message
 *
 * For unrecoverable errors where continuing would be unsafe.
 * Logs the message and calls std::abort().
 *
 * @param message Error description
 *
 * @example
 * @code
 * if (criticalInvariantViolated) {
 *     fatalError("Heap corruption detected");
 * }
 * @endcode
 */
[[noreturn]] inline void fatalError(const char* message) {
    logError("FATAL: %s", message);
    std::abort();
}

/** @brief Overload accepting std::string */
[[noreturn]] inline void fatalError(const std::string& message) {
    fatalError(message.c_str());
}

/**
 * @brief Debug assertion macro
 *
 * Checks a condition in debug builds. Aborts if the condition is false.
 * Compiled out in release builds (NDEBUG defined).
 *
 * @param condition Expression that must be true
 * @param message Description of what failed
 *
 * @example
 * @code
 * GLEAM_ASSERT(ptr != nullptr, "Pointer must not be null");
 * GLEAM_ASSERT(count > 0, "Count must be positive");
 * @endcode
 */
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

/**
 * @brief Precondition check (throws on failure)
 *
 * Verifies that a function's preconditions are met. Unlike GLEAM_ASSERT,
 * this is enabled in release builds and throws GleamException on failure.
 *
 * @param condition Expression that must be true
 * @param message Description of the precondition
 *
 * @example
 * @code
 * void setBreakpoint(uint64_t address) {
 *     GLEAM_REQUIRE(address != 0, "Address must be non-zero");
 *     GLEAM_REQUIRE(isValidAddress(address), "Address must be valid");
 *     // ...
 * }
 * @endcode
 */
#define GLEAM_REQUIRE(condition, message) \
    do { \
        if (!(condition)) { \
            Gleam::logError("Precondition failed: %s (%s:%d)", message, __FILE__, __LINE__); \
            throw Gleam::GleamException(Gleam::ErrorCategory::Internal, message); \
        } \
    } while (0)

/**
 * @brief Postcondition check (throws on failure)
 *
 * Verifies that a function's postconditions hold before returning.
 * Enabled in release builds and throws GleamException on failure.
 *
 * @param condition Expression that must be true
 * @param message Description of the postcondition
 *
 * @example
 * @code
 * uint64_t allocateMemory(size_t size) {
 *     uint64_t addr = VirtualAllocEx(...);
 *     GLEAM_ENSURE(addr != 0, "Memory allocation must succeed");
 *     return addr;
 * }
 * @endcode
 */
#define GLEAM_ENSURE(condition, message) \
    do { \
        if (!(condition)) { \
            Gleam::logError("Postcondition failed: %s (%s:%d)", message, __FILE__, __LINE__); \
            throw Gleam::GleamException(Gleam::ErrorCategory::Internal, message); \
        } \
    } while (0)

/**
 * @brief Panic macro for unrecoverable errors
 *
 * Immediately aborts the program with an error message.
 * Use when continuing would cause data corruption or undefined behavior.
 *
 * @param message Error description
 *
 * @example
 * @code
 * if (heapCorrupted()) {
 *     GLEAM_PANIC("Heap metadata corruption detected");
 * }
 * @endcode
 */
#define GLEAM_PANIC(message) \
    do { \
        Gleam::logError("PANIC: %s (%s:%d)", message, __FILE__, __LINE__); \
        std::abort(); \
    } while (0)

/**
 * @brief Mark unreachable code paths
 *
 * Aborts if reached. Use in switch default cases or after exhaustive
 * if-else chains that should cover all possibilities.
 *
 * @example
 * @code
 * switch (state) {
 *     case State::Running: return "running";
 *     case State::Stopped: return "stopped";
 *     case State::Terminated: return "terminated";
 *     default: GLEAM_UNREACHABLE();
 * }
 * @endcode
 */
#define GLEAM_UNREACHABLE() \
    GLEAM_PANIC("Unreachable code reached")

// -----------------------------------------------------------------------
// Recovery strategies
// -----------------------------------------------------------------------

/**
 * @brief Retry an operation with exponential backoff
 *
 * Attempts a function multiple times with increasing delays between attempts.
 * Useful for transient failures (network timeouts, temporary locks, etc.).
 *
 * @tparam T Return type
 * @tparam Func Callable returning T
 * @param func Function to retry (may throw)
 * @param maxRetries Maximum number of retry attempts (default: 3)
 * @param initialDelayMs Initial delay in milliseconds (doubles each retry)
 * @return Result<T> containing the value or final error
 *
 * @example
 * @code
 * auto result = retryWithBackoff<uint64_t>([&]() {
 *     return readMemoryMayFail(address, size);
 * }, 5, 50);  // Up to 5 retries, starting with 50ms delay
 *
 * if (result.isOk()) {
 *     printf("Success after retries: 0x%llX\n", result.value());
 * }
 * @endcode
 */
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

/**
 * @brief Try multiple fallback strategies
 *
 * Attempts alternatives in order until one succeeds. Returns the first
 * successful result or the last error if all fail.
 *
 * @tparam T Return type
 * @param alternatives Initializer list of functions to try
 * @return Result<T> from the first successful alternative
 *
 * @example
 * @code
 * auto result = tryFallbacks<uint64_t>({
 *     [&]() { return resolveByPdb("kernel32!CreateFileW"); },
 *     [&]() { return resolveByExports("CreateFileW"); },
 *     [&]() { return resolveByPattern(createFilePattern); }
 * });
 *
 * if (result.isOk()) {
 *     printf("Resolved via fallback: 0x%llX\n", result.value());
 * }
 * @endcode
 */
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
