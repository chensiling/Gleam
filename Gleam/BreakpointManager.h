// Breakpoint management for GleamDebugger

#ifndef GLEAM_BREAKPOINT_MANAGER_H
#define GLEAM_BREAKPOINT_MANAGER_H

#include "Error.h"
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include <functional>

namespace Gleam {

// Breakpoint types
enum class BreakpointType {
    Software,    // INT3 software breakpoint
    Hardware,    // Hardware debug register breakpoint
    Memory       // Memory access breakpoint (via page guard)
};

// Hardware breakpoint conditions
enum class HardwareCondition {
    Execute,     // Break on execution
    Write,       // Break on write
    ReadWrite,   // Break on read or write
    IO           // Break on I/O (not commonly used)
};

// Hardware breakpoint size
enum class HardwareSize {
    Byte = 1,
    Word = 2,
    Dword = 4,
    Qword = 8
};

// Breakpoint state
enum class BreakpointState {
    Enabled,     // Active and armed
    Disabled,    // Exists but not active
    OneShot,     // Will be removed after first hit
    Pending      // Waiting for module load
};

// Breakpoint information
struct BreakpointInfo {
    uint32_t id;
    BreakpointType type;
    BreakpointState state;
    uint64_t address;
    uint8_t originalByte;  // For software breakpoints

    // Hardware breakpoint specific
    HardwareCondition hwCondition;
    HardwareSize hwSize;
    int hwRegister;  // 0-3 for DR0-DR3, -1 if not hardware

    // Condition
    std::string condition;  // Expression to evaluate
    bool hasCondition;

    // Hit count
    uint32_t hitCount;
    uint32_t ignoreCount;  // Skip first N hits

    // Metadata
    std::string name;
    bool isTemporary;

    BreakpointInfo()
        : id(0), type(BreakpointType::Software), state(BreakpointState::Enabled)
        , address(0), originalByte(0)
        , hwCondition(HardwareCondition::Execute), hwSize(HardwareSize::Byte)
        , hwRegister(-1), hasCondition(false)
        , hitCount(0), ignoreCount(0), isTemporary(false) {}
};

// Breakpoint manager
class BreakpointManager {
private:
    std::unordered_map<uint32_t, BreakpointInfo> mBreakpoints;
    uint32_t mNextId;

    // Hardware breakpoint tracking (DR0-DR3)
    bool mHardwareUsed[4];

    // Callback for memory operations
    std::function<bool(uint64_t, void*, size_t)> mMemRead;
    std::function<bool(uint64_t, const void*, size_t)> mMemWrite;

public:
    BreakpointManager();

    // Set memory operation callbacks
    void setMemoryCallbacks(
        std::function<bool(uint64_t, void*, size_t)> memRead,
        std::function<bool(uint64_t, const void*, size_t)> memWrite);

    // Software breakpoints
    Result<uint32_t> setSoftwareBreakpoint(uint64_t address, const std::string& name = "");
    Result<bool> removeSoftwareBreakpoint(uint32_t id);

    // Hardware breakpoints
    Result<uint32_t> setHardwareBreakpoint(uint64_t address,
                                            HardwareCondition condition,
                                            HardwareSize size,
                                            const std::string& name = "");
    Result<bool> removeHardwareBreakpoint(uint32_t id);

    // Generic breakpoint operations
    Result<bool> removeBreakpoint(uint32_t id);
    Result<bool> enableBreakpoint(uint32_t id);
    Result<bool> disableBreakpoint(uint32_t id);

    // Temporary breakpoints (auto-removed after hit)
    Result<uint32_t> setTemporaryBreakpoint(uint64_t address);

    // Conditional breakpoints
    Result<bool> setCondition(uint32_t id, const std::string& condition);
    Result<bool> clearCondition(uint32_t id);

    // Ignore count (skip first N hits)
    Result<bool> setIgnoreCount(uint32_t id, uint32_t count);

    // Queries
    const BreakpointInfo* getBreakpoint(uint32_t id) const;
    BreakpointInfo* getBreakpoint(uint32_t id);
    const BreakpointInfo* getBreakpointAt(uint64_t address) const;

    bool hasBreakpoint(uint32_t id) const;
    bool hasBreakpointAt(uint64_t address) const;

    size_t getBreakpointCount() const { return mBreakpoints.size(); }
    std::vector<uint32_t> getAllBreakpointIds() const;
    std::vector<const BreakpointInfo*> getBreakpointsInRange(uint64_t start, uint64_t end) const;

    // Hit handling
    Result<bool> onBreakpointHit(uint32_t id);
    bool shouldBreak(uint32_t id);  // Check condition and ignore count

    // Hardware register management
    int allocateHardwareRegister();
    void freeHardwareRegister(int reg);
    bool isHardwareRegisterUsed(int reg) const;
    int getAvailableHardwareCount() const;

    // Bulk operations
    void removeAllBreakpoints();
    void removeTemporaryBreakpoints();
    std::vector<uint32_t> getBreakpointsByType(BreakpointType type) const;

    // State management
    Result<bool> saveOriginalByte(uint32_t id, uint8_t byte);
    uint8_t getOriginalByte(uint32_t id) const;

    // Clear all state
    void clear();

private:
    uint32_t generateId();
    Result<bool> writeSoftwareBreakpoint(uint64_t address, uint8_t& originalByte);
    Result<bool> restoreSoftwareBreakpoint(uint64_t address, uint8_t originalByte);
};

} // namespace Gleam

#endif // GLEAM_BREAKPOINT_MANAGER_H
