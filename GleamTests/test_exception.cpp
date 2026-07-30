// Unit tests for exception handling strategy

#include <gtest/gtest.h>
#include <string>
#include <functional>

namespace Gleam {

// Mock Error and Result for testing
enum class ErrorCategory {
    None, Memory, Symbol, Process, Breakpoint, Internal
};

struct Error {
    ErrorCategory category;
    std::string message;
    int systemCode;
    std::string context;

    Error() : category(ErrorCategory::None), systemCode(0) {}
    Error(ErrorCategory cat, const std::string& msg, int code = 0, const std::string& ctx = "")
        : category(cat), message(msg), systemCode(code), context(ctx) {}

    bool hasError() const { return category != ErrorCategory::None; }

    std::string format() const {
        return message + (context.empty() ? "" : " [" + context + "]");
    }
};

template<typename T>
class Result {
private:
    bool mIsOk;
    T mValue;
    Error mError;

public:
    Result(const T& value) : mIsOk(true), mValue(value) {}
    Result(const Error& error) : mIsOk(false), mError(error) {}

    bool isOk() const { return mIsOk; }
    bool isError() const { return !mIsOk; }

    const T& value() const { return mValue; }
    const Error& error() const { return mError; }
};

// Mock exception classes
class GleamException : public std::exception {
private:
    Error mError;
    std::string mWhat;

public:
    explicit GleamException(const Error& error)
        : mError(error), mWhat(error.format()) {}

    GleamException(ErrorCategory category, const std::string& message)
        : mError(category, message), mWhat(message) {}

    const char* what() const noexcept override {
        return mWhat.c_str();
    }

    const Error& error() const noexcept {
        return mError;
    }
};

class MemoryException : public GleamException {
public:
    explicit MemoryException(const std::string& message)
        : GleamException(ErrorCategory::Memory, message) {}
};

// Try-catch wrapper
template<typename T, typename Func>
Result<T> tryCatch(Func func, const char* operation = "operation") {
    try {
        return Result<T>(func());
    }
    catch (const GleamException& e) {
        return Result<T>(e.error());
    }
    catch (const std::exception& e) {
        return Result<T>(Error(ErrorCategory::Internal, e.what()));
    }
    catch (...) {
        return Result<T>(Error(ErrorCategory::Internal, "Unknown exception"));
    }
}

template<typename Func>
Result<bool> tryCatchVoid(Func func, const char* operation = "operation") {
    try {
        func();
        return Result<bool>(true);
    }
    catch (const GleamException& e) {
        return Result<bool>(e.error());
    }
    catch (const std::exception& e) {
        return Result<bool>(Error(ErrorCategory::Internal, e.what()));
    }
    catch (...) {
        return Result<bool>(Error(ErrorCategory::Internal, "Unknown exception"));
    }
}

// Retry with backoff (simplified for testing - no Sleep)
template<typename T, typename Func>
Result<T> retryWithBackoff(Func func, int maxRetries = 3) {
    for (int attempt = 0; attempt <= maxRetries; attempt++) {
        auto result = tryCatch<T>(func, "retry operation");
        if (result.isOk()) {
            return result;
        }
    }
    return Result<T>(Error(ErrorCategory::Internal, "Max retries exceeded"));
}

// Fallback chain
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

// Exception Tests

TEST(ExceptionTest, GleamExceptionConstruction) {
    Gleam::GleamException ex(Gleam::ErrorCategory::Memory, "Test error");

    EXPECT_STREQ(ex.what(), "Test error");
    EXPECT_EQ(ex.error().category, Gleam::ErrorCategory::Memory);
}

TEST(ExceptionTest, MemoryExceptionType) {
    Gleam::MemoryException ex("Out of memory");

    EXPECT_STREQ(ex.what(), "Out of memory");
    EXPECT_EQ(ex.error().category, Gleam::ErrorCategory::Memory);
}

TEST(TryCatchTest, SuccessfulOperation) {
    auto result = Gleam::tryCatch<int>([]() {
        return 42;
    }, "test op");

    EXPECT_TRUE(result.isOk());
    EXPECT_EQ(result.value(), 42);
}

TEST(TryCatchTest, ThrowsGleamException) {
    auto result = Gleam::tryCatch<int>([]() -> int {
        throw Gleam::MemoryException("Failed to allocate");
    }, "test op");

    EXPECT_TRUE(result.isError());
    EXPECT_EQ(result.error().category, Gleam::ErrorCategory::Memory);
    EXPECT_EQ(result.error().message, "Failed to allocate");
}

TEST(TryCatchTest, ThrowsStdException) {
    auto result = Gleam::tryCatch<int>([]() -> int {
        throw std::runtime_error("Standard error");
    }, "test op");

    EXPECT_TRUE(result.isError());
    EXPECT_EQ(result.error().category, Gleam::ErrorCategory::Internal);
    EXPECT_EQ(result.error().message, "Standard error");
}

TEST(TryCatchTest, ThrowsUnknownException) {
    auto result = Gleam::tryCatch<int>([]() -> int {
        throw 42;  // Unknown type
    }, "test op");

    EXPECT_TRUE(result.isError());
    EXPECT_EQ(result.error().category, Gleam::ErrorCategory::Internal);
    EXPECT_EQ(result.error().message, "Unknown exception");
}

TEST(TryCatchVoidTest, SuccessfulVoidOperation) {
    int counter = 0;

    auto result = Gleam::tryCatchVoid([&]() {
        counter++;
    }, "test op");

    EXPECT_TRUE(result.isOk());
    EXPECT_EQ(counter, 1);
}

TEST(TryCatchVoidTest, VoidOperationThrows) {
    auto result = Gleam::tryCatchVoid([]() {
        throw Gleam::MemoryException("Failed");
    }, "test op");

    EXPECT_TRUE(result.isError());
    EXPECT_EQ(result.error().category, Gleam::ErrorCategory::Memory);
}

TEST(RetryTest, SucceedsOnFirstAttempt) {
    int attempts = 0;

    auto result = Gleam::retryWithBackoff<int>([&]() {
        attempts++;
        return 42;
    }, 3);

    EXPECT_TRUE(result.isOk());
    EXPECT_EQ(result.value(), 42);
    EXPECT_EQ(attempts, 1);
}

TEST(RetryTest, SucceedsOnSecondAttempt) {
    int attempts = 0;

    auto result = Gleam::retryWithBackoff<int>([&]() {
        attempts++;
        if (attempts < 2) {
            throw Gleam::MemoryException("Temporary failure");
        }
        return 42;
    }, 3);

    EXPECT_TRUE(result.isOk());
    EXPECT_EQ(result.value(), 42);
    EXPECT_EQ(attempts, 2);
}

TEST(RetryTest, FailsAfterMaxRetries) {
    int attempts = 0;

    auto result = Gleam::retryWithBackoff<int>([&]() -> int {
        attempts++;
        throw Gleam::MemoryException("Permanent failure");
    }, 2);

    EXPECT_TRUE(result.isError());
    EXPECT_EQ(attempts, 3);  // Initial + 2 retries
}

TEST(FallbackTest, FirstAlternativeSucceeds) {
    auto result = Gleam::tryFallbacks<int>({
        []() { return Gleam::Result<int>(42); },
        []() { return Gleam::Result<int>(100); },
        []() { return Gleam::Result<int>(200); }
    });

    EXPECT_TRUE(result.isOk());
    EXPECT_EQ(result.value(), 42);
}

TEST(FallbackTest, SecondAlternativeSucceeds) {
    auto result = Gleam::tryFallbacks<int>({
        []() { return Gleam::Result<int>(Gleam::Error(Gleam::ErrorCategory::Memory, "Failed")); },
        []() { return Gleam::Result<int>(100); },
        []() { return Gleam::Result<int>(200); }
    });

    EXPECT_TRUE(result.isOk());
    EXPECT_EQ(result.value(), 100);
}

TEST(FallbackTest, AllAlternativesFail) {
    auto result = Gleam::tryFallbacks<int>({
        []() { return Gleam::Result<int>(Gleam::Error(Gleam::ErrorCategory::Memory, "Failed 1")); },
        []() { return Gleam::Result<int>(Gleam::Error(Gleam::ErrorCategory::Symbol, "Failed 2")); },
        []() { return Gleam::Result<int>(Gleam::Error(Gleam::ErrorCategory::Process, "Failed 3")); }
    });

    EXPECT_TRUE(result.isError());
    // Last error should be from third alternative
    EXPECT_EQ(result.error().category, Gleam::ErrorCategory::Process);
}

TEST(FallbackTest, EmptyAlternatives) {
    auto result = Gleam::tryFallbacks<int>({});

    EXPECT_TRUE(result.isError());
    EXPECT_EQ(result.error().message, "No alternatives succeeded");
}

// Integration Tests

TEST(ExceptionIntegrationTest, TryCatchWithRetry) {
    int attempts = 0;

    auto result = Gleam::retryWithBackoff<std::string>([&]() {
        attempts++;
        if (attempts < 2) {
            throw std::runtime_error("Transient error");
        }
        return std::string("Success");
    }, 3);

    EXPECT_TRUE(result.isOk());
    EXPECT_EQ(result.value(), "Success");
    EXPECT_EQ(attempts, 2);
}

TEST(ExceptionIntegrationTest, FallbackWithExceptions) {
    int fallback1Calls = 0;
    int fallback2Calls = 0;
    int fallback3Calls = 0;

    auto result = Gleam::tryFallbacks<int>({
        [&]() -> Gleam::Result<int> {
            fallback1Calls++;
            return Gleam::tryCatch<int>([]() -> int {
                throw Gleam::MemoryException("Fallback 1 failed");
            });
        },
        [&]() -> Gleam::Result<int> {
            fallback2Calls++;
            return Gleam::tryCatch<int>([]() -> int {
                throw Gleam::MemoryException("Fallback 2 failed");
            });
        },
        [&]() -> Gleam::Result<int> {
            fallback3Calls++;
            return Gleam::tryCatch<int>([]() {
                return 42;  // Success
            });
        }
    });

    EXPECT_TRUE(result.isOk());
    EXPECT_EQ(result.value(), 42);
    EXPECT_EQ(fallback1Calls, 1);
    EXPECT_EQ(fallback2Calls, 1);
    EXPECT_EQ(fallback3Calls, 1);
}
