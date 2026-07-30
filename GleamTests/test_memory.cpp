// Unit tests for Memory operations

#include <gtest/gtest.h>
#include <vector>
#include <string>

namespace Gleam {

// Mock Error and Result for testing
enum class ErrorCategory {
    None, Memory, Process
};

struct Error {
    ErrorCategory category;
    std::string message;

    Error() : category(ErrorCategory::None) {}
    Error(ErrorCategory cat, const std::string& msg, int code = 0, const std::string& ctx = "")
        : category(cat), message(msg) {}

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
};

// Mock Process for testing
class MockProcess {
private:
    std::vector<uint8_t> mMemory;
    uint64_t mBaseAddress;

public:
    MockProcess() : mBaseAddress(0x140000000) {
        // Allocate 1MB of mock memory
        mMemory.resize(1024 * 1024);

        // Initialize with test data
        for (size_t i = 0; i < mMemory.size(); i++) {
            mMemory[i] = static_cast<uint8_t>(i & 0xFF);
        }
    }

    bool MemRead(uint64_t address, void* buffer, size_t size) {
        if (address < mBaseAddress)
            return false;

        uint64_t offset = address - mBaseAddress;
        if (offset + size > mMemory.size())
            return false;

        memcpy(buffer, &mMemory[offset], size);
        return true;
    }

    bool MemWrite(uint64_t address, const void* buffer, size_t size) {
        if (address < mBaseAddress)
            return false;

        uint64_t offset = address - mBaseAddress;
        if (offset + size > mMemory.size())
            return false;

        memcpy(&mMemory[offset], buffer, size);
        return true;
    }

    uint64_t baseAddress() const { return mBaseAddress; }

    // Helper to set specific values
    void setQword(uint64_t address, uint64_t value) {
        MemWrite(address, &value, sizeof(value));
    }

    void setDword(uint64_t address, uint32_t value) {
        MemWrite(address, &value, sizeof(value));
    }
};

// Mock MemoryReader for testing
class MockMemoryReader {
private:
    MockProcess* mProcess;

public:
    explicit MockMemoryReader(MockProcess* process) : mProcess(process) {}

    Result<uint8_t> readByte(uint64_t address) {
        uint8_t value;
        if (!mProcess->MemRead(address, &value, sizeof(value)))
            return Error(ErrorCategory::Memory, "Failed to read byte");
        return value;
    }

    Result<uint32_t> readDword(uint64_t address) {
        uint32_t value;
        if (!mProcess->MemRead(address, &value, sizeof(value)))
            return Error(ErrorCategory::Memory, "Failed to read dword");
        return value;
    }

    Result<uint64_t> readQword(uint64_t address) {
        uint64_t value;
        if (!mProcess->MemRead(address, &value, sizeof(value)))
            return Error(ErrorCategory::Memory, "Failed to read qword");
        return value;
    }

    Result<std::vector<uint8_t>> readBytes(uint64_t address, size_t size) {
        if (size > 100 * 1024 * 1024)
            return Error(ErrorCategory::Memory, "Size too large");

        std::vector<uint8_t> buffer(size);
        if (!mProcess->MemRead(address, buffer.data(), size))
            return Error(ErrorCategory::Memory, "Failed to read bytes");
        return buffer;
    }
};

// Mock MemoryWriter for testing
class MockMemoryWriter {
private:
    MockProcess* mProcess;

public:
    explicit MockMemoryWriter(MockProcess* process) : mProcess(process) {}

    Result<bool> writeByte(uint64_t address, uint8_t value) {
        if (!mProcess->MemWrite(address, &value, sizeof(value)))
            return Error(ErrorCategory::Memory, "Failed to write byte");
        return true;
    }

    Result<bool> writeDword(uint64_t address, uint32_t value) {
        if (!mProcess->MemWrite(address, &value, sizeof(value)))
            return Error(ErrorCategory::Memory, "Failed to write dword");
        return true;
    }

    Result<bool> writeQword(uint64_t address, uint64_t value) {
        if (!mProcess->MemWrite(address, &value, sizeof(value)))
            return Error(ErrorCategory::Memory, "Failed to write qword");
        return true;
    }

    Result<bool> writeBytes(uint64_t address, const std::vector<uint8_t>& data) {
        if (!mProcess->MemWrite(address, data.data(), data.size()))
            return Error(ErrorCategory::Memory, "Failed to write bytes");
        return true;
    }
};

} // namespace Gleam

// Tests

TEST(MemoryReaderTest, ReadByte) {
    Gleam::MockProcess process;
    Gleam::MockMemoryReader reader(&process);

    uint64_t addr = process.baseAddress();
    auto result = reader.readByte(addr);

    EXPECT_TRUE(result.isOk());
    EXPECT_EQ(result.value(), 0x00);
}

TEST(MemoryReaderTest, ReadDword) {
    Gleam::MockProcess process;
    Gleam::MockMemoryReader reader(&process);

    uint64_t addr = process.baseAddress() + 0x100;
    process.setDword(addr, 0x12345678);

    auto result = reader.readDword(addr);

    EXPECT_TRUE(result.isOk());
    EXPECT_EQ(result.value(), 0x12345678);
}

TEST(MemoryReaderTest, ReadQword) {
    Gleam::MockProcess process;
    Gleam::MockMemoryReader reader(&process);

    uint64_t addr = process.baseAddress() + 0x200;
    process.setQword(addr, 0x123456789ABCDEF0);

    auto result = reader.readQword(addr);

    EXPECT_TRUE(result.isOk());
    EXPECT_EQ(result.value(), 0x123456789ABCDEF0ULL);
}

TEST(MemoryReaderTest, ReadInvalidAddress) {
    Gleam::MockProcess process;
    Gleam::MockMemoryReader reader(&process);

    // Address outside valid range
    uint64_t addr = 0x1000;
    auto result = reader.readByte(addr);

    EXPECT_TRUE(result.isError());
    EXPECT_EQ(result.error().category, Gleam::ErrorCategory::Memory);
}

TEST(MemoryReaderTest, ReadBytes) {
    Gleam::MockProcess process;
    Gleam::MockMemoryReader reader(&process);

    uint64_t addr = process.baseAddress();
    auto result = reader.readBytes(addr, 16);

    EXPECT_TRUE(result.isOk());
    EXPECT_EQ(result.value().size(), 16);

    // Check pattern
    for (size_t i = 0; i < 16; i++) {
        EXPECT_EQ(result.value()[i], static_cast<uint8_t>(i & 0xFF));
    }
}

TEST(MemoryReaderTest, ReadBytesTooLarge) {
    Gleam::MockProcess process;
    Gleam::MockMemoryReader reader(&process);

    uint64_t addr = process.baseAddress();
    auto result = reader.readBytes(addr, 200 * 1024 * 1024);  // 200MB

    EXPECT_TRUE(result.isError());
}

TEST(MemoryWriterTest, WriteByte) {
    Gleam::MockProcess process;
    Gleam::MockMemoryWriter writer(&process);
    Gleam::MockMemoryReader reader(&process);

    uint64_t addr = process.baseAddress() + 0x50;
    auto writeResult = writer.writeByte(addr, 0xAB);

    EXPECT_TRUE(writeResult.isOk());

    auto readResult = reader.readByte(addr);
    EXPECT_TRUE(readResult.isOk());
    EXPECT_EQ(readResult.value(), 0xAB);
}

TEST(MemoryWriterTest, WriteDword) {
    Gleam::MockProcess process;
    Gleam::MockMemoryWriter writer(&process);
    Gleam::MockMemoryReader reader(&process);

    uint64_t addr = process.baseAddress() + 0x100;
    auto writeResult = writer.writeDword(addr, 0xDEADBEEF);

    EXPECT_TRUE(writeResult.isOk());

    auto readResult = reader.readDword(addr);
    EXPECT_TRUE(readResult.isOk());
    EXPECT_EQ(readResult.value(), 0xDEADBEEF);
}

TEST(MemoryWriterTest, WriteQword) {
    Gleam::MockProcess process;
    Gleam::MockMemoryWriter writer(&process);
    Gleam::MockMemoryReader reader(&process);

    uint64_t addr = process.baseAddress() + 0x200;
    auto writeResult = writer.writeQword(addr, 0xCAFEBABEDEADBEEFULL);

    EXPECT_TRUE(writeResult.isOk());

    auto readResult = reader.readQword(addr);
    EXPECT_TRUE(readResult.isOk());
    EXPECT_EQ(readResult.value(), 0xCAFEBABEDEADBEEFULL);
}

TEST(MemoryWriterTest, WriteBytes) {
    Gleam::MockProcess process;
    Gleam::MockMemoryWriter writer(&process);
    Gleam::MockMemoryReader reader(&process);

    uint64_t addr = process.baseAddress() + 0x300;
    std::vector<uint8_t> data = {0x11, 0x22, 0x33, 0x44, 0x55};

    auto writeResult = writer.writeBytes(addr, data);
    EXPECT_TRUE(writeResult.isOk());

    auto readResult = reader.readBytes(addr, data.size());
    EXPECT_TRUE(readResult.isOk());
    EXPECT_EQ(readResult.value(), data);
}

TEST(MemoryWriterTest, WriteInvalidAddress) {
    Gleam::MockProcess process;
    Gleam::MockMemoryWriter writer(&process);

    // Address outside valid range
    uint64_t addr = 0x1000;
    auto result = writer.writeByte(addr, 0xFF);

    EXPECT_TRUE(result.isError());
}

TEST(MemoryRoundTripTest, ByteRoundTrip) {
    Gleam::MockProcess process;
    Gleam::MockMemoryWriter writer(&process);
    Gleam::MockMemoryReader reader(&process);

    uint64_t addr = process.baseAddress();

    for (uint8_t val = 0; val < 255; val++) {
        writer.writeByte(addr, val);
        auto result = reader.readByte(addr);
        EXPECT_EQ(result.value(), val);
    }
}

TEST(MemoryRoundTripTest, DwordRoundTrip) {
    Gleam::MockProcess process;
    Gleam::MockMemoryWriter writer(&process);
    Gleam::MockMemoryReader reader(&process);

    uint64_t addr = process.baseAddress();
    uint32_t testValues[] = {0, 1, 0xFFFFFFFF, 0x12345678, 0xDEADBEEF};

    for (uint32_t val : testValues) {
        writer.writeDword(addr, val);
        auto result = reader.readDword(addr);
        EXPECT_EQ(result.value(), val);
    }
}

TEST(MemoryRoundTripTest, QwordRoundTrip) {
    Gleam::MockProcess process;
    Gleam::MockMemoryWriter writer(&process);
    Gleam::MockMemoryReader reader(&process);

    uint64_t addr = process.baseAddress();
    uint64_t testValues[] = {
        0,
        1,
        0xFFFFFFFFFFFFFFFFULL,
        0x123456789ABCDEF0ULL,
        0xCAFEBABEDEADBEEFULL
    };

    for (uint64_t val : testValues) {
        writer.writeQword(addr, val);
        auto result = reader.readQword(addr);
        EXPECT_EQ(result.value(), val);
    }
}
