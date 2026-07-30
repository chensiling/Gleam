/**
 * @file Memory.h
 * @brief Type-safe memory operations with automatic error handling
 *
 * Provides high-level wrappers around process memory access that:
 * - Return Result<T> for automatic error propagation
 * - Validate addresses before access
 * - Handle common patterns (read pointer, read string, batch reads)
 * - RAII memory protection guards for temporary permission changes
 *
 * **Design rationale:**
 * Raw ReadProcessMemory/WriteProcessMemory require manual error checking
 * and type casting. These wrappers provide type safety and uniform error
 * handling via Result<T>.
 *
 * @see Error.h for Result<T> monad
 */

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

/**
 * @brief Type-safe memory reader with automatic error handling
 *
 * Wraps a GleeBug::Process and provides type-safe read operations that
 * return Result<T> for uniform error handling. All methods validate the
 * address before attempting to read.
 *
 * @example Basic usage:
 * @code
 * MemoryReader reader(process);
 *
 * auto result = reader.readQword(0x7FFF12345678);
 * if (result.isOk()) {
 *     printf("Value: 0x%llX\n", result.value());
 * } else {
 *     printf("Error: %s\n", result.error().message.c_str());
 * }
 * @endcode
 *
 * @example String reading:
 * @code
 * auto str = reader.readString(stringAddr, 256);
 * if (str.isOk()) {
 *     printf("String: %s\n", str.value().c_str());
 * }
 * @endcode
 */
class MemoryReader {
private:
    GleeBug::Process* mProcess;  ///< Target process

public:
    /**
     * @brief Construct a memory reader for a process
     * @param process Target process (must remain valid)
     */
    explicit MemoryReader(GleeBug::Process* process);

    /**
     * @brief Read a single byte
     * @param address Address to read from
     * @return Result<uint8_t> containing the byte or error
     */
    Result<uint8_t> readByte(uint64_t address);

    /**
     * @brief Read a 16-bit word
     * @param address Address to read from
     * @return Result<uint16_t> containing the word or error
     */
    Result<uint16_t> readWord(uint64_t address);

    /**
     * @brief Read a 32-bit double word
     * @param address Address to read from
     * @return Result<uint32_t> containing the dword or error
     */
    Result<uint32_t> readDword(uint64_t address);

    /**
     * @brief Read a 64-bit quad word
     * @param address Address to read from
     * @return Result<uint64_t> containing the qword or error
     */
    Result<uint64_t> readQword(uint64_t address);

    /**
     * @brief Read a pointer (architecture-dependent size)
     * @param address Address to read from
     * @return Result<intptr_t> containing the pointer or error
     */
    Result<intptr_t> readPointer(uint64_t address);

    /**
     * @brief Read a byte buffer
     * @param address Starting address
     * @param size Number of bytes to read
     * @return Result<vector<uint8_t>> containing the data or error
     *
     * @example
     * @code
     * auto data = reader.readBytes(addr, 16);
     * if (data.isOk()) {
     *     for (uint8_t byte : data.value()) {
     *         printf("%02X ", byte);
     *     }
     * }
     * @endcode
     */
    Result<std::vector<uint8_t>> readBytes(uint64_t address, size_t size);

    /**
     * @brief Read a null-terminated ASCII string
     * @param address Starting address of the string
     * @param maxLength Maximum characters to read (safety limit)
     * @return Result<string> containing the string or error
     *
     * @note Stops at null terminator or maxLength, whichever comes first
     */
    Result<std::string> readString(uint64_t address, size_t maxLength = 4096);

    /**
     * @brief Read a null-terminated wide string (UTF-16)
     * @param address Starting address of the string
     * @param maxLength Maximum characters to read (safety limit)
     * @return Result<wstring> containing the string or error
     */
    Result<std::wstring> readWideString(uint64_t address, size_t maxLength = 4096);

    /**
     * @brief Check if an address range is valid for reading
     * @param address Starting address
     * @param size Number of bytes (default: 1)
     * @return true if the address is readable
     *
     * @note Uses VirtualQueryEx to check page accessibility
     */
    bool isValidAddress(uint64_t address, size_t size = 1) const;

    /**
     * @brief Get the underlying process
     * @return Pointer to the GleeBug::Process
     */
    GleeBug::Process* process() const { return mProcess; }
};

/**
 * @brief Type-safe memory writer with automatic error handling
 *
 * Wraps a GleeBug::Process and provides type-safe write operations that
 * return Result<bool> for uniform error handling. All methods validate
 * the address before attempting to write.
 *
 * @example Basic usage:
 * @code
 * MemoryWriter writer(process);
 *
 * auto result = writer.writeQword(0x7FFF12345678, 0xDEADBEEF);
 * if (!result.isOk()) {
 *     printf("Write failed: %s\n", result.error().message.c_str());
 * }
 * @endcode
 */
class MemoryWriter {
private:
    GleeBug::Process* mProcess;  ///< Target process

public:
    /**
     * @brief Construct a memory writer for a process
     * @param process Target process (must remain valid)
     */
    explicit MemoryWriter(GleeBug::Process* process);

    /**
     * @brief Write a single byte
     * @param address Address to write to
     * @param value Byte value
     * @return Result<bool> - Ok(true) on success, Error on failure
     */
    Result<bool> writeByte(uint64_t address, uint8_t value);

    /**
     * @brief Write a 16-bit word
     * @param address Address to write to
     * @param value Word value
     * @return Result<bool> - Ok(true) on success, Error on failure
     */
    Result<bool> writeWord(uint64_t address, uint16_t value);

    /**
     * @brief Write a 32-bit double word
     * @param address Address to write to
     * @param value Dword value
     * @return Result<bool> - Ok(true) on success, Error on failure
     */
    Result<bool> writeDword(uint64_t address, uint32_t value);

    /**
     * @brief Write a 64-bit quad word
     * @param address Address to write to
     * @param value Qword value
     * @return Result<bool> - Ok(true) on success, Error on failure
     */
    Result<bool> writeQword(uint64_t address, uint64_t value);

    /**
     * @brief Write a pointer (architecture-dependent size)
     * @param address Address to write to
     * @param value Pointer value
     * @return Result<bool> - Ok(true) on success, Error on failure
     */
    Result<bool> writePointer(uint64_t address, intptr_t value);

    /**
     * @brief Write a byte buffer from raw pointer
     * @param address Starting address
     * @param data Pointer to data
     * @param size Number of bytes to write
     * @return Result<bool> - Ok(true) on success, Error on failure
     */
    Result<bool> writeBytes(uint64_t address, const void* data, size_t size);

    /**
     * @brief Write a byte buffer from vector
     * @param address Starting address
     * @param data Vector of bytes to write
     * @return Result<bool> - Ok(true) on success, Error on failure
     */
    Result<bool> writeBytes(uint64_t address, const std::vector<uint8_t>& data);

    /**
     * @brief Write an ASCII string (including null terminator)
     * @param address Starting address
     * @param str String to write
     * @return Result<bool> - Ok(true) on success, Error on failure
     */
    Result<bool> writeString(uint64_t address, const std::string& str);

    /**
     * @brief Write a wide string (including null terminator)
     * @param address Starting address
     * @param str Wide string to write
     * @return Result<bool> - Ok(true) on success, Error on failure
     */
    Result<bool> writeWideString(uint64_t address, const std::wstring& str);

    /**
     * @brief Check if an address range is valid for writing
     * @param address Starting address
     * @param size Number of bytes (default: 1)
     * @return true if the address is writable
     */
    bool isValidAddress(uint64_t address, size_t size = 1) const;

    /**
     * @brief Get the underlying process
     * @return Pointer to the GleeBug::Process
     */
    GleeBug::Process* process() const { return mProcess; }
};

/**
 * @brief Combined reader/writer for convenience
 *
 * Holds both a MemoryReader and MemoryWriter for the same process.
 * Useful when you need both read and write access.
 *
 * @example
 * @code
 * MemoryAccessor mem(process);
 *
 * auto oldValue = mem.readQword(addr);
 * if (oldValue.isOk()) {
 *     uint64_t newValue = oldValue.value() | 0x1;
 *     mem.writeQword(addr, newValue);
 * }
 * @endcode
 */
class MemoryAccessor {
private:
    MemoryReader mReader;  ///< Reader instance
    MemoryWriter mWriter;  ///< Writer instance

public:
    /**
     * @brief Construct a memory accessor for a process
     * @param process Target process (must remain valid)
     */
    explicit MemoryAccessor(GleeBug::Process* process);

    /** @brief Access the reader */
    MemoryReader& reader() { return mReader; }

    /** @brief Access the writer */
    MemoryWriter& writer() { return mWriter; }

    /** @brief Const access to reader */
    const MemoryReader& reader() const { return mReader; }

    /** @brief Const access to writer */
    const MemoryWriter& writer() const { return mWriter; }

    /**
     * @brief Convenience: read a qword
     * @param address Address to read from
     * @return Result<uint64_t> containing the value or error
     */
    Result<uint64_t> readQword(uint64_t address) { return mReader.readQword(address); }

    /**
     * @brief Convenience: write a qword
     * @param address Address to write to
     * @param value Value to write
     * @return Result<bool> - Ok(true) on success, Error on failure
     */
    Result<bool> writeQword(uint64_t address, uint64_t value) { return mWriter.writeQword(address, value); }

    /**
     * @brief Check if an address is valid for access
     * @param address Starting address
     * @param size Number of bytes (default: 1)
     * @return true if accessible
     */
    bool isValidAddress(uint64_t address, size_t size = 1) const {
        return mReader.isValidAddress(address, size);
    }
};

/**
 * @brief RAII guard for temporary memory protection changes
 *
 * Automatically changes memory protection on construction and restores
 * the original protection on destruction. Use this when you need to
 * temporarily make memory writable or executable.
 *
 * @example Making read-only memory writable:
 * @code
 * {
 *     MemoryProtectGuard guard(process, codeAddr, 16, PAGE_EXECUTE_READWRITE);
 *     if (guard.isValid()) {
 *         writer.writeBytes(codeAddr, patchBytes, 16);
 *     }
 * }  // Protection automatically restored here
 * @endcode
 *
 * @warning Do not use for large regions or long-lived protection changes
 */
class MemoryProtectGuard {
private:
    GleeBug::Process* mProcess;  ///< Target process
    uint64_t mAddress;           ///< Protected region start
    size_t mSize;                ///< Protected region size
    uint32_t mOldProtect;        ///< Original protection flags
    bool mRestored;              ///< Whether protection was restored

public:
    /**
     * @brief Construct and change memory protection
     * @param process Target process
     * @param address Starting address of the region
     * @param size Size of the region in bytes
     * @param newProtect New protection flags (PAGE_* constants)
     *
     * @note If VirtualProtectEx fails, isValid() returns false
     */
    MemoryProtectGuard(GleeBug::Process* process, uint64_t address, size_t size, uint32_t newProtect);

    /**
     * @brief Destructor: restore original protection
     *
     * Automatically restores the protection that was in place before
     * the guard was created.
     */
    ~MemoryProtectGuard();

    // Disable copy (move semantics could be added if needed)
    MemoryProtectGuard(const MemoryProtectGuard&) = delete;
    MemoryProtectGuard& operator=(const MemoryProtectGuard&) = delete;

    /**
     * @brief Manually restore protection early
     *
     * Allows restoring protection before the destructor runs.
     * Safe to call multiple times.
     *
     * @return true if restore succeeded
     */
    bool restore();

    /**
     * @brief Check if the guard is active
     * @return true if protection change succeeded and not yet restored
     */
    bool isValid() const { return !mRestored; }

    /**
     * @brief Get the original protection flags
     * @return Original PAGE_* flags
     */
    uint32_t oldProtect() const { return mOldProtect; }
};

/**
 * @brief Batch memory reader for performance optimization
 *
 * Allows queuing multiple memory reads and executing them in a single
 * batch to reduce overhead. Useful when you need to read many small
 * regions scattered across memory.
 *
 * @example
 * @code
 * BatchMemoryReader batch(process);
 *
 * // Queue reads
 * batch.addRead(addr1, 8);
 * batch.addRead(addr2, 16);
 * batch.addRead(addr3, 4);
 *
 * // Execute all at once
 * auto results = batch.execute();
 * if (results.isOk()) {
 *     for (const auto& data : results.value()) {
 *         // Process each read result
 *     }
 * }
 * @endcode
 *
 * @note Order of results matches order of addRead() calls
 */
class BatchMemoryReader {
private:
    GleeBug::Process* mProcess;         ///< Target process
    std::vector<uint64_t> mAddresses;   ///< Queued read addresses
    std::vector<size_t> mSizes;         ///< Queued read sizes

public:
    /**
     * @brief Construct a batch reader for a process
     * @param process Target process (must remain valid)
     */
    explicit BatchMemoryReader(GleeBug::Process* process);

    /**
     * @brief Add a read request to the batch
     * @param address Address to read from
     * @param size Number of bytes to read
     */
    void addRead(uint64_t address, size_t size);

    /**
     * @brief Execute all queued reads
     * @return Result<vector<vector<uint8_t>>> containing all read data or error
     *
     * @note Results are in the same order as addRead() calls
     * @note Clears the batch after execution
     */
    Result<std::vector<std::vector<uint8_t>>> execute();

    /**
     * @brief Clear all pending reads without executing
     */
    void clear();

    /**
     * @brief Get the number of queued reads
     * @return Count of pending read operations
     */
    size_t count() const { return mAddresses.size(); }
};

} // namespace Gleam

#endif // GLEAM_MEMORY_H
