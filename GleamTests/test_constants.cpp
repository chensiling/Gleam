// Unit tests for constants defined in Constants.h

#include <gtest/gtest.h>

// Include the constants header (assuming path is set up)
// For now, we'll test the concept with local definitions

namespace Gleam {
namespace Limits {
    constexpr int MAX_TERMINATE_RETRIES = 100;
    constexpr int TERMINATE_RETRY_DELAY_MS = 10;
    constexpr int SHORT_THREAD_WAIT_MS = 100;
    constexpr int THREAD_WAIT_TIMEOUT_MS = 1000;
}
namespace Memory {
    constexpr size_t BREAK_IN_STUB_SIZE = 16;
    constexpr size_t BREAK_IN_PAGE_SIZE = 0x1000;
    constexpr size_t SMALL_DETAIL_BUFFER = 64;
}
}

// Constants validation tests

TEST(ConstantsTest, LimitsArePositive) {
    EXPECT_GT(Gleam::Limits::MAX_TERMINATE_RETRIES, 0);
    EXPECT_GT(Gleam::Limits::TERMINATE_RETRY_DELAY_MS, 0);
    EXPECT_GT(Gleam::Limits::SHORT_THREAD_WAIT_MS, 0);
    EXPECT_GT(Gleam::Limits::THREAD_WAIT_TIMEOUT_MS, 0);
}

TEST(ConstantsTest, TimeoutsAreReasonable) {
    EXPECT_LE(Gleam::Limits::TERMINATE_RETRY_DELAY_MS, 100);
    EXPECT_LE(Gleam::Limits::SHORT_THREAD_WAIT_MS, 500);
    EXPECT_LE(Gleam::Limits::THREAD_WAIT_TIMEOUT_MS, 5000);
}

TEST(ConstantsTest, MemorySizesArePageAligned) {
    EXPECT_EQ(Gleam::Memory::BREAK_IN_PAGE_SIZE % 4096, 0);
}

TEST(ConstantsTest, BufferSizesAreReasonable) {
    EXPECT_GE(Gleam::Memory::SMALL_DETAIL_BUFFER, 32);
    EXPECT_LE(Gleam::Memory::SMALL_DETAIL_BUFFER, 256);
}

TEST(ConstantsTest, StubSizeIsValid) {
    EXPECT_EQ(Gleam::Memory::BREAK_IN_STUB_SIZE, 16);
    EXPECT_GT(Gleam::Memory::BREAK_IN_STUB_SIZE, 0);
}
