// Unit tests for exception handling strategy

#include <gtest/gtest.h>

// Exercise the real exception utilities rather than local mocks. Redefining
// Error/Result/GleamException/tryCatch in namespace Gleam collides with the
// real definitions reachable through SymbolResolver.cpp, an ODR violation that
// corrupts the stack (/RTC1 Check Failure #2).
//
// windows.h must come first: Exception.h's retryWithBackoff() calls Sleep().
#include <windows.h>
#include <string>
#include <functional>
#include "../Gleam/Exception.h"

// Exception Tests

TEST(ExceptionTest, GleamExceptionConstruction) {
    Gleam::GleamException ex(Gleam::ErrorCategory::Memory, "Test error");

    // what() is Error::format(), which prefixes the category.
    EXPECT_NE(std::string(ex.what()).find("Test error"), std::string::npos);
    EXPECT_EQ(ex.error().category, Gleam::ErrorCategory::Memory);
    EXPECT_EQ(ex.error().message, "Test error");
}

TEST(ExceptionTest, MemoryExceptionType) {
    Gleam::MemoryException ex("Out of memory");

    EXPECT_NE(std::string(ex.what()).find("Out of memory"), std::string::npos);
    EXPECT_EQ(ex.error().category, Gleam::ErrorCategory::Memory);
    EXPECT_EQ(ex.error().message, "Out of memory");
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
