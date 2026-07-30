// Unit tests for error handling

#include <gtest/gtest.h>
#include <string>

// Mock Error implementation for testing
namespace Gleam {

enum class ErrorCategory {
    None,
    Memory,
    Symbol,
    Process,
    Breakpoint,
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

    T valueOr(const T& defaultValue) const {
        return isOk() ? mValue : defaultValue;
    }
};

} // namespace Gleam

// Error Tests

TEST(ErrorTest, DefaultConstructor) {
    Gleam::Error err;
    EXPECT_FALSE(err.hasError());
    EXPECT_EQ(err.category, Gleam::ErrorCategory::None);
    EXPECT_EQ(err.systemCode, 0);
}

TEST(ErrorTest, ConstructWithMessage) {
    Gleam::Error err(Gleam::ErrorCategory::Memory, "Failed to read memory");
    EXPECT_TRUE(err.hasError());
    EXPECT_EQ(err.category, Gleam::ErrorCategory::Memory);
    EXPECT_EQ(err.message, "Failed to read memory");
    EXPECT_EQ(err.systemCode, 0);
}

TEST(ErrorTest, ConstructWithSystemCode) {
    Gleam::Error err(Gleam::ErrorCategory::Process, "API call failed", 5);
    EXPECT_TRUE(err.hasError());
    EXPECT_EQ(err.systemCode, 5);
}

TEST(ErrorTest, ConstructWithContext) {
    Gleam::Error err(Gleam::ErrorCategory::Symbol, "Symbol not found", 0, "kernel32!CreateFileW");
    EXPECT_TRUE(err.hasError());
    EXPECT_EQ(err.context, "kernel32!CreateFileW");
}

// Result<T> Tests

TEST(ResultTest, ConstructWithValue) {
    Gleam::Result<int> result(42);
    EXPECT_TRUE(result.isOk());
    EXPECT_FALSE(result.isError());
    EXPECT_EQ(result.value(), 42);
}

TEST(ResultTest, ConstructWithError) {
    Gleam::Error err(Gleam::ErrorCategory::Memory, "Out of memory");
    Gleam::Result<int> result(err);
    EXPECT_FALSE(result.isOk());
    EXPECT_TRUE(result.isError());
    EXPECT_EQ(result.error().message, "Out of memory");
}

TEST(ResultTest, ValueOr) {
    Gleam::Result<int> success(42);
    EXPECT_EQ(success.valueOr(0), 42);

    Gleam::Error err(Gleam::ErrorCategory::Memory, "Failed");
    Gleam::Result<int> failure(err);
    EXPECT_EQ(failure.valueOr(99), 99);
}

TEST(ResultTest, PointerResult) {
    int value = 123;
    int* ptr = &value;
    Gleam::Result<int*> result(ptr);
    EXPECT_TRUE(result.isOk());
    EXPECT_EQ(*result.value(), 123);
}

TEST(ResultTest, StringResult) {
    Gleam::Result<std::string> result("Success");
    EXPECT_TRUE(result.isOk());
    EXPECT_EQ(result.value(), "Success");
}

// Error Propagation Tests

TEST(ErrorPropagationTest, ChainedOperations) {
    auto readMemory = [](uint64_t addr) -> Gleam::Result<uint32_t> {
        if (addr == 0)
            return Gleam::Error(Gleam::ErrorCategory::Memory, "Invalid address", 0, "0x0");
        return 0xDEADBEEF;
    };

    auto result1 = readMemory(0x1000);
    EXPECT_TRUE(result1.isOk());
    EXPECT_EQ(result1.value(), 0xDEADBEEF);

    auto result2 = readMemory(0);
    EXPECT_TRUE(result2.isError());
    EXPECT_EQ(result2.error().category, Gleam::ErrorCategory::Memory);
}

TEST(ErrorPropagationTest, ErrorContext) {
    auto resolveSymbol = [](const std::string& name) -> Gleam::Result<uint64_t> {
        if (name.empty())
            return Gleam::Error(Gleam::ErrorCategory::Symbol, "Empty symbol name");

        if (name == "invalid")
            return Gleam::Error(Gleam::ErrorCategory::Symbol, "Symbol not found", 0, name);

        return 0x12345678;
    };

    auto result = resolveSymbol("invalid");
    EXPECT_TRUE(result.isError());
    EXPECT_EQ(result.error().context, "invalid");
}

// Real-world scenario tests

TEST(ErrorScenarioTest, MemoryReadFailure) {
    Gleam::Error err(Gleam::ErrorCategory::Memory,
                     "ReadProcessMemory failed",
                     5,  // ERROR_ACCESS_DENIED
                     "address=0x7FFA12340000");

    EXPECT_EQ(err.category, Gleam::ErrorCategory::Memory);
    EXPECT_EQ(err.systemCode, 5);
    EXPECT_FALSE(err.context.empty());
}

TEST(ErrorScenarioTest, SymbolResolutionFailure) {
    Gleam::Error err(Gleam::ErrorCategory::Symbol,
                     "Ambiguous symbol",
                     0,
                     "kernel32!CreateFile has 3 candidates");

    EXPECT_EQ(err.category, Gleam::ErrorCategory::Symbol);
    EXPECT_TRUE(err.context.find("3 candidates") != std::string::npos);
}

TEST(ErrorScenarioTest, BreakpointFailure) {
    Gleam::Error err(Gleam::ErrorCategory::Breakpoint,
                     "Failed to write INT3",
                     998,  // ERROR_NOACCESS
                     "address=0x140001000");

    EXPECT_EQ(err.category, Gleam::ErrorCategory::Breakpoint);
    EXPECT_NE(err.systemCode, 0);
}
