// Memory operation wrappers with error handling and validation

#ifndef GLEAM_MEMORY_H
#define GLEAM_MEMORY_H

#include "Error.h"
#include <cstdint>
#include <vector>
#include <string>

namespace GleeBug {
    class Process;  // Forward declaration
}

namespace Gleam {

// Memory reader with automatic error handling
class MemoryReader {
private:
    GleeBug::Process* mProcess;

public:
    explicit MemoryReader(GleeBug::Process* process);

    // Read single values with type safety
    Result<uint8_t> readByte(uint64_t address);
    Result<uint16_t> readWord(uint64_t address);
    Result<uint32_t> readDword(uint64_t address);
    Result<uint64_t> readQword(uint64_t address);
    Result<intptr_t> readPointer(uint64_t address);

    // Read buffer
    Result<std::vector<uint8_t>> readBytes(uint64_t address, size_t size);

    // Read null-terminated string
    Result<std::string> readString(uint64_t address, size_t maxLength = 4096);
    Result<std::wstring> readWideString(uint64_t address, size_t maxLength = 4096);

    // Validate address before reading
    bool isValidAddress(uint64_t address, size_t size = 1) const;

    // Get underlying process
    GleeBug::Process* process() const { return mProcess; }
};

// Memory writer with automatic error handling
class MemoryWriter {
private:
    GleeBug::Process* mProcess;

public:
    explicit MemoryWriter(GleeBug::Process* process);

    // Write single values with type safety
    Result<bool> writeByte(uint64_t address, uint8_t value);
    Result<bool> writeWord(uint64_t address, uint16_t value);
    Result<bool> writeDword(uint64_t address, uint32_t value);
    Result<bool> writeQword(uint64_t address, uint64_t value);
    Result<bool> writePointer(uint64_t address, intptr_t value);

    // Write buffer
    Result<bool> writeBytes(uint64_t address, const void* data, size_t size);
    Result<bool> writeBytes(uint64_t address, const std::vector<uint8_t>& data);

    // Write string
    Result<bool> writeString(uint64_t address, const std::string& str);
    Result<bool> writeWideString(uint64_t address, const std::wstring& str);

    // Validate address before writing
    bool isValidAddress(uint64_t address, size_t size = 1) const;

    // Get underlying process
    GleeBug::Process* process() const { return mProcess; }
};

// Combined reader/writer for convenience
class MemoryAccessor {
private:
    MemoryReader mReader;
    MemoryWriter mWriter;

public:
    explicit MemoryAccessor(GleeBug::Process* process);

    MemoryReader& reader() { return mReader; }
    MemoryWriter& writer() { return mWriter; }

    const MemoryReader& reader() const { return mReader; }
    const MemoryWriter& writer() const { return mWriter; }

    // Convenience methods
    Result<uint64_t> readQword(uint64_t address) { return mReader.readQword(address); }
    Result<bool> writeQword(uint64_t address, uint64_t value) { return mWriter.writeQword(address, value); }

    bool isValidAddress(uint64_t address, size_t size = 1) const {
        return mReader.isValidAddress(address, size);
    }
};

// Memory protection guard (RAII)
class MemoryProtectGuard {
private:
    GleeBug::Process* mProcess;
    uint64_t mAddress;
    size_t mSize;
    uint32_t mOldProtect;
    bool mRestored;

public:
    MemoryProtectGuard(GleeBug::Process* process, uint64_t address, size_t size, uint32_t newProtect);
    ~MemoryProtectGuard();

    // Disable copy
    MemoryProtectGuard(const MemoryProtectGuard&) = delete;
    MemoryProtectGuard& operator=(const MemoryProtectGuard&) = delete;

    // Manually restore protection (if needed before destructor)
    bool restore();

    bool isValid() const { return !mRestored; }
    uint32_t oldProtect() const { return mOldProtect; }
};

// Batch memory operations for performance
class BatchMemoryReader {
private:
    GleeBug::Process* mProcess;
    std::vector<uint64_t> mAddresses;
    std::vector<size_t> mSizes;

public:
    explicit BatchMemoryReader(GleeBug::Process* process);

    // Add read request
    void addRead(uint64_t address, size_t size);

    // Execute all reads
    Result<std::vector<std::vector<uint8_t>>> execute();

    // Clear pending reads
    void clear();

    size_t count() const { return mAddresses.size(); }
};

} // namespace Gleam

#endif // GLEAM_MEMORY_H
