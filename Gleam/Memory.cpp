// Memory operation wrappers implementation

#include "Memory.h"
#include "Log.h"
#include "GleeBug/Debugger.h"
#include <cstring>

namespace Gleam {

// Helper function declaration
static std::string formatAddress(uint64_t address);

// MemoryReader implementation

MemoryReader::MemoryReader(GleeBug::Process* process)
    : mProcess(process) {
}

Result<uint8_t> MemoryReader::readByte(uint64_t address) {
    if (!mProcess)
        return Error(ErrorCategory::Process, "Process is null");

    uint8_t value = 0;
    if (!mProcess->MemRead(address, &value, sizeof(value)))
        return Error(ErrorCategory::Memory, "Failed to read byte", 0, formatAddress(address));

    return value;
}

Result<uint16_t> MemoryReader::readWord(uint64_t address) {
    if (!mProcess)
        return Error(ErrorCategory::Process, "Process is null");

    uint16_t value = 0;
    if (!mProcess->MemRead(address, &value, sizeof(value)))
        return Error(ErrorCategory::Memory, "Failed to read word", 0, formatAddress(address));

    return value;
}

Result<uint32_t> MemoryReader::readDword(uint64_t address) {
    if (!mProcess)
        return Error(ErrorCategory::Process, "Process is null");

    uint32_t value = 0;
    if (!mProcess->MemRead(address, &value, sizeof(value)))
        return Error(ErrorCategory::Memory, "Failed to read dword", 0, formatAddress(address));

    return value;
}

Result<uint64_t> MemoryReader::readQword(uint64_t address) {
    if (!mProcess)
        return Error(ErrorCategory::Process, "Process is null");

    uint64_t value = 0;
    if (!mProcess->MemRead(address, &value, sizeof(value)))
        return Error(ErrorCategory::Memory, "Failed to read qword", 0, formatAddress(address));

    return value;
}

Result<intptr_t> MemoryReader::readPointer(uint64_t address) {
    auto result = readQword(address);
    if (result.isError())
        return Error(result.error());

    return static_cast<intptr_t>(result.value());
}

Result<std::vector<uint8_t>> MemoryReader::readBytes(uint64_t address, size_t size) {
    if (!mProcess)
        return Error(ErrorCategory::Process, "Process is null");

    if (size == 0)
        return std::vector<uint8_t>();

    if (size > 100 * 1024 * 1024) // 100MB sanity check
        return Error(ErrorCategory::Memory, "Read size too large");

    std::vector<uint8_t> buffer(size);
    if (!mProcess->MemRead(address, buffer.data(), size))
        return Error(ErrorCategory::Memory, "Failed to read bytes", 0, formatAddress(address));

    return buffer;
}

Result<std::string> MemoryReader::readString(uint64_t address, size_t maxLength) {
    if (!mProcess)
        return Error(ErrorCategory::Process, "Process is null");

    std::string result;
    result.reserve(256);

    for (size_t i = 0; i < maxLength; i++) {
        auto byteResult = readByte(address + i);
        if (byteResult.isError())
            return Error(byteResult.error());

        uint8_t ch = byteResult.value();
        if (ch == 0)
            break;

        result.push_back(static_cast<char>(ch));
    }

    return result;
}

Result<std::wstring> MemoryReader::readWideString(uint64_t address, size_t maxLength) {
    if (!mProcess)
        return Error(ErrorCategory::Process, "Process is null");

    std::wstring result;
    result.reserve(256);

    for (size_t i = 0; i < maxLength; i++) {
        auto wordResult = readWord(address + i * 2);
        if (wordResult.isError())
            return Error(wordResult.error());

        wchar_t ch = static_cast<wchar_t>(wordResult.value());
        if (ch == 0)
            break;

        result.push_back(ch);
    }

    return result;
}

bool MemoryReader::isValidAddress(uint64_t address, size_t size) const {
    if (!mProcess)
        return false;

    // Try to read one byte to check validity
    uint8_t probe;
    return mProcess->MemRead(address, &probe, 1);
}

// MemoryWriter implementation

MemoryWriter::MemoryWriter(GleeBug::Process* process)
    : mProcess(process) {
}

Result<bool> MemoryWriter::writeByte(uint64_t address, uint8_t value) {
    if (!mProcess)
        return Error(ErrorCategory::Process, "Process is null");

    if (!mProcess->MemWrite(address, &value, sizeof(value)))
        return Error(ErrorCategory::Memory, "Failed to write byte", 0, formatAddress(address));

    return true;
}

Result<bool> MemoryWriter::writeWord(uint64_t address, uint16_t value) {
    if (!mProcess)
        return Error(ErrorCategory::Process, "Process is null");

    if (!mProcess->MemWrite(address, &value, sizeof(value)))
        return Error(ErrorCategory::Memory, "Failed to write word", 0, formatAddress(address));

    return true;
}

Result<bool> MemoryWriter::writeDword(uint64_t address, uint32_t value) {
    if (!mProcess)
        return Error(ErrorCategory::Process, "Process is null");

    if (!mProcess->MemWrite(address, &value, sizeof(value)))
        return Error(ErrorCategory::Memory, "Failed to write dword", 0, formatAddress(address));

    return true;
}

Result<bool> MemoryWriter::writeQword(uint64_t address, uint64_t value) {
    if (!mProcess)
        return Error(ErrorCategory::Process, "Process is null");

    if (!mProcess->MemWrite(address, &value, sizeof(value)))
        return Error(ErrorCategory::Memory, "Failed to write qword", 0, formatAddress(address));

    return true;
}

Result<bool> MemoryWriter::writePointer(uint64_t address, intptr_t value) {
    return writeQword(address, static_cast<uint64_t>(value));
}

Result<bool> MemoryWriter::writeBytes(uint64_t address, const void* data, size_t size) {
    if (!mProcess)
        return Error(ErrorCategory::Process, "Process is null");

    if (size == 0)
        return true;

    if (!mProcess->MemWrite(address, data, size))
        return Error(ErrorCategory::Memory, "Failed to write bytes", 0, formatAddress(address));

    return true;
}

Result<bool> MemoryWriter::writeBytes(uint64_t address, const std::vector<uint8_t>& data) {
    return writeBytes(address, data.data(), data.size());
}

Result<bool> MemoryWriter::writeString(uint64_t address, const std::string& str) {
    // Include null terminator
    return writeBytes(address, str.c_str(), str.length() + 1);
}

Result<bool> MemoryWriter::writeWideString(uint64_t address, const std::wstring& str) {
    // Include null terminator
    return writeBytes(address, str.c_str(), (str.length() + 1) * sizeof(wchar_t));
}

bool MemoryWriter::isValidAddress(uint64_t address, size_t size) const {
    if (!mProcess)
        return false;

    // Try to read one byte to check validity
    uint8_t probe;
    return mProcess->MemRead(address, &probe, 1);
}

// MemoryAccessor implementation

MemoryAccessor::MemoryAccessor(GleeBug::Process* process)
    : mReader(process), mWriter(process) {
}

// MemoryProtectGuard implementation

MemoryProtectGuard::MemoryProtectGuard(GleeBug::Process* process, uint64_t address,
                                       size_t size, uint32_t newProtect)
    : mProcess(process), mAddress(address), mSize(size), mOldProtect(0), mRestored(false) {

    if (!mProcess)
        return;

    // Change protection (cast to DWORD*)
    DWORD oldProt = 0;
    if (!mProcess->MemProtect(address, size, newProtect, &oldProt)) {
        logWarn("Failed to change memory protection at 0x%llX",
                (unsigned long long)address);
        mRestored = true;  // Mark as invalid
    }
    mOldProtect = static_cast<uint32_t>(oldProt);
}

MemoryProtectGuard::~MemoryProtectGuard() {
    restore();
}

bool MemoryProtectGuard::restore() {
    if (mRestored || !mProcess)
        return false;

    // Restore original protection (cast to DWORD)
    DWORD dummy = 0;
    bool success = mProcess->MemProtect(mAddress, mSize, static_cast<DWORD>(mOldProtect), &dummy);

    if (!success) {
        logWarn("Failed to restore memory protection at 0x%llX",
                (unsigned long long)mAddress);
    }

    mRestored = true;
    return success;
}

// BatchMemoryReader implementation

BatchMemoryReader::BatchMemoryReader(GleeBug::Process* process)
    : mProcess(process) {
}

void BatchMemoryReader::addRead(uint64_t address, size_t size) {
    mAddresses.push_back(address);
    mSizes.push_back(size);
}

Result<std::vector<std::vector<uint8_t>>> BatchMemoryReader::execute() {
    if (!mProcess)
        return Error(ErrorCategory::Process, "Process is null");

    std::vector<std::vector<uint8_t>> results;
    results.reserve(mAddresses.size());

    MemoryReader reader(mProcess);

    for (size_t i = 0; i < mAddresses.size(); i++) {
        auto result = reader.readBytes(mAddresses[i], mSizes[i]);

        if (result.isError()) {
            // Return error on first failure
            return Error(result.error());
        }

        results.push_back(result.value());
    }

    return results;
}

void BatchMemoryReader::clear() {
    mAddresses.clear();
    mSizes.clear();
}

// Helper function implementation
std::string formatAddress(uint64_t address) {
    char buf[32];
    snprintf(buf, sizeof(buf), "0x%llX", (unsigned long long)address);
    return std::string(buf);
}

} // namespace Gleam
