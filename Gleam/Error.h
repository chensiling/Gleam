/**
 * @file Error.h
 * @brief Error handling utilities for GleamDebugger
 *
 * Provides structured error handling with Result<T> monad pattern and
 * detailed error information including category, message, and system codes.
 */

#ifndef GLEAM_ERROR_H
#define GLEAM_ERROR_H

#include <string>

namespace Gleam {

/**
 * @brief Categories of errors that can occur in GleamDebugger
 *
 * Error categories help classify failures and determine appropriate
 * recovery strategies. Each category represents a different subsystem.
 */
enum class ErrorCategory {
    None,            ///< No error (success state)
    Memory,          ///< Memory access or allocation failures
    Symbol,          ///< Symbol resolution failures
    Process,         ///< Process/thread management failures
    Breakpoint,      ///< Breakpoint operation failures
    Exception,       ///< Exception handling failures
    Internal,        ///< Internal logic errors
    WindowsSystem,   ///< Windows API failures (renamed to avoid System macro conflict)
};

/**
 * @brief Detailed error information structure
 *
 * Contains all information needed to diagnose and report an error:
 * - Category: broad classification of the error
 * - Message: human-readable description
 * - SystemCode: Windows error code (from GetLastError)
 * - Context: additional contextual information (e.g., function name, address)
 *
 * @example
 * @code
 * Error err(ErrorCategory::Memory, "Failed to read process memory",
 *           ERROR_PARTIAL_COPY, "address=0x12345678");
 * if (err.hasError()) {
 *     printf("%s\n", err.format().c_str());
 * }
 * @endcode
 */
struct Error {
    ErrorCategory category;  ///< Error category
    std::string message;     ///< Human-readable error message
    int systemCode;          ///< Windows error code (0 if not applicable)
    std::string context;     ///< Additional context (e.g., "address=0x...")

    /// Default constructor - creates a "no error" state
    Error() : category(ErrorCategory::None), systemCode(0) {}

    /**
     * @brief Construct an error with full details
     * @param cat Error category
     * @param msg Human-readable message
     * @param code Windows error code (default 0)
     * @param ctx Additional context string (default empty)
     */
    Error(ErrorCategory cat, const std::string& msg, int code = 0, const std::string& ctx = "")
        : category(cat), message(msg), systemCode(code), context(ctx) {}

    /**
     * @brief Check if this represents an actual error
     * @return true if category is not None
     */
    bool hasError() const { return category != ErrorCategory::None; }

    /**
     * @brief Format error as a human-readable string
     * @return Formatted error message including all details
     */
    std::string format() const;
};

/**
 * @brief Result monad for error propagation without exceptions
 *
 * Result<T> represents either a successful value of type T or an Error.
 * This allows functions to return typed errors without throwing exceptions,
 * making error paths explicit and testable.
 *
 * @tparam T The type of the successful result value
 *
 * @example Basic usage:
 * @code
 * Result<uint32_t> readDword(uint64_t address) {
 *     uint32_t value;
 *     if (!ReadProcessMemory(..., &value, ...)) {
 *         return Error(ErrorCategory::Memory, "Read failed", GetLastError());
 *     }
 *     return value;
 * }
 *
 * auto result = readDword(0x12345678);
 * if (result.isOk()) {
 *     printf("Value: 0x%X\n", result.value());
 * } else {
 *     printf("Error: %s\n", result.error().format().c_str());
 * }
 * @endcode
 *
 * @example Chaining with valueOr:
 * @code
 * uint32_t value = readDword(address).valueOr(0);  // Default to 0 on error
 * @endcode
 */
template<typename T>
class Result {
private:
    T mValue;           ///< The successful value (only valid if mHasValue is true)
    Error mError;       ///< The error (only valid if mHasValue is false)
    bool mHasValue;     ///< True if this Result contains a value, false if error

public:
    /**
     * @brief Construct a successful Result with a value
     * @param value The successful result value
     */
    Result(const T& value) : mValue(value), mHasValue(true) {}

    /**
     * @brief Construct a failed Result with an error
     * @param error The error information
     */
    Result(const Error& error) : mError(error), mHasValue(false) {}

    /**
     * @brief Check if this Result contains a successful value
     * @return true if value is available, false if error
     */
    bool isOk() const { return mHasValue; }

    /**
     * @brief Check if this Result contains an error
     * @return true if error, false if value is available
     */
    bool isError() const { return !mHasValue; }

    /**
     * @brief Get the successful value (const)
     * @return Reference to the value
     * @warning Only call this if isOk() is true
     */
    const T& value() const { return mValue; }

    /**
     * @brief Get the successful value (mutable)
     * @return Reference to the value
     * @warning Only call this if isOk() is true
     */
    T& value() { return mValue; }

    /**
     * @brief Get the error information
     * @return Reference to the error
     * @warning Only call this if isError() is true
     */
    const Error& error() const { return mError; }

    /**
     * @brief Get the value or a default if this Result is an error
     * @param defaultValue Value to return if this Result contains an error
     * @return The successful value or the provided default
     */
    T valueOr(const T& defaultValue) const {
        return mHasValue ? mValue : defaultValue;
    }
};

/**
 * @brief Create an error with category and message
 * @param category Error category name (without ErrorCategory:: prefix)
 * @param message Human-readable error message
 *
 * @example GLEAM_ERROR(Memory, "Failed to allocate buffer")
 */
#define GLEAM_ERROR(category, message) \
    Gleam::Error(Gleam::ErrorCategory::category, message, 0, "")

/**
 * @brief Create an error with category, message, and context
 * @param category Error category name
 * @param message Human-readable error message
 * @param context Additional context string
 *
 * @example GLEAM_ERROR_CTX(Symbol, "Ambiguous symbol", "kernel32!CreateFile")
 */
#define GLEAM_ERROR_CTX(category, message, context) \
    Gleam::Error(Gleam::ErrorCategory::category, message, 0, context)

/**
 * @brief Create an error with category, message, and Windows error code
 * @param category Error category name
 * @param message Human-readable error message
 * @param code Windows error code (from GetLastError())
 *
 * @example GLEAM_ERROR_SYS(Memory, "ReadProcessMemory failed", GetLastError())
 */
#define GLEAM_ERROR_SYS(category, message, code) \
    Gleam::Error(Gleam::ErrorCategory::category, message, code, "")

/**
 * @brief Convert Windows error code to human-readable string
 * @param errorCode Error code from GetLastError()
 * @return Formatted error message from FormatMessage
 */
std::string formatWindowsError(int errorCode);

} // namespace Gleam

#endif // GLEAM_ERROR_H
