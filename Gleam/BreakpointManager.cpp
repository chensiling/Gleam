// Breakpoint management implementation

#include "BreakpointManager.h"
#include "Log.h"
#include <algorithm>

namespace Gleam {

BreakpointManager::BreakpointManager()
    : mNextId(1) {
    for (int i = 0; i < 4; i++) {
        mHardwareUsed[i] = false;
    }
}

void BreakpointManager::setMemoryCallbacks(
    std::function<bool(uint64_t, void*, size_t)> memRead,
    std::function<bool(uint64_t, const void*, size_t)> memWrite) {
    mMemRead = memRead;
    mMemWrite = memWrite;
}

Result<uint32_t> BreakpointManager::setSoftwareBreakpoint(uint64_t address, const std::string& name) {
    // Check if breakpoint already exists at this address
    if (hasBreakpointAt(address)) {
        return Error(ErrorCategory::Breakpoint, "Breakpoint already exists at address");
    }

    BreakpointInfo info;
    info.id = generateId();
    info.type = BreakpointType::Software;
    info.state = BreakpointState::Enabled;
    info.address = address;
    info.name = name;

    // Write INT3 instruction
    auto result = writeSoftwareBreakpoint(address, info.originalByte);
    if (result.isError()) {
        return Error(result.error());
    }

    mBreakpoints[info.id] = info;

    logInfo("Software breakpoint set: id=%u addr=0x%llX name=%s",
            info.id, (unsigned long long)address, name.c_str());

    return info.id;
}

Result<bool> BreakpointManager::removeSoftwareBreakpoint(uint32_t id) {
    auto* bp = getBreakpoint(id);
    if (!bp) {
        return Error(ErrorCategory::Breakpoint, "Breakpoint not found");
    }

    if (bp->type != BreakpointType::Software) {
        return Error(ErrorCategory::Breakpoint, "Not a software breakpoint");
    }

    // Restore original byte
    if (bp->state == BreakpointState::Enabled) {
        auto result = restoreSoftwareBreakpoint(bp->address, bp->originalByte);
        if (result.isError()) {
            return Error(result.error());
        }
    }

    mBreakpoints.erase(id);

    logInfo("Software breakpoint removed: id=%u addr=0x%llX",
            id, (unsigned long long)bp->address);

    return true;
}

Result<uint32_t> BreakpointManager::setHardwareBreakpoint(uint64_t address,
                                                           HardwareCondition condition,
                                                           HardwareSize size,
                                                           const std::string& name) {
    // Check if breakpoint already exists at this address
    if (hasBreakpointAt(address)) {
        return Error(ErrorCategory::Breakpoint, "Breakpoint already exists at address");
    }

    // Allocate hardware register
    int reg = allocateHardwareRegister();
    if (reg < 0) {
        return Error(ErrorCategory::Breakpoint, "No available hardware debug registers");
    }

    BreakpointInfo info;
    info.id = generateId();
    info.type = BreakpointType::Hardware;
    info.state = BreakpointState::Enabled;
    info.address = address;
    info.name = name;
    info.hwCondition = condition;
    info.hwSize = size;
    info.hwRegister = reg;

    mBreakpoints[info.id] = info;

    logInfo("Hardware breakpoint set: id=%u addr=0x%llX reg=DR%d",
            info.id, (unsigned long long)address, reg);

    return info.id;
}

Result<bool> BreakpointManager::removeHardwareBreakpoint(uint32_t id) {
    auto* bp = getBreakpoint(id);
    if (!bp) {
        return Error(ErrorCategory::Breakpoint, "Breakpoint not found");
    }

    if (bp->type != BreakpointType::Hardware) {
        return Error(ErrorCategory::Breakpoint, "Not a hardware breakpoint");
    }

    // Free hardware register
    if (bp->hwRegister >= 0 && bp->hwRegister < 4) {
        freeHardwareRegister(bp->hwRegister);
    }

    mBreakpoints.erase(id);

    logInfo("Hardware breakpoint removed: id=%u addr=0x%llX",
            id, (unsigned long long)bp->address);

    return true;
}

Result<bool> BreakpointManager::removeBreakpoint(uint32_t id) {
    auto* bp = getBreakpoint(id);
    if (!bp) {
        return Error(ErrorCategory::Breakpoint, "Breakpoint not found");
    }

    if (bp->type == BreakpointType::Software) {
        return removeSoftwareBreakpoint(id);
    } else if (bp->type == BreakpointType::Hardware) {
        return removeHardwareBreakpoint(id);
    }

    return Error(ErrorCategory::Breakpoint, "Unknown breakpoint type");
}

Result<bool> BreakpointManager::enableBreakpoint(uint32_t id) {
    auto* bp = getBreakpoint(id);
    if (!bp) {
        return Error(ErrorCategory::Breakpoint, "Breakpoint not found");
    }

    if (bp->state == BreakpointState::Enabled) {
        return true;  // Already enabled
    }

    if (bp->type == BreakpointType::Software) {
        uint8_t dummy;
        auto result = writeSoftwareBreakpoint(bp->address, dummy);
        if (result.isError()) {
            return Error(result.error());
        }
    }

    bp->state = BreakpointState::Enabled;

    logDebug("Breakpoint enabled: id=%u", id);
    return true;
}

Result<bool> BreakpointManager::disableBreakpoint(uint32_t id) {
    auto* bp = getBreakpoint(id);
    if (!bp) {
        return Error(ErrorCategory::Breakpoint, "Breakpoint not found");
    }

    if (bp->state == BreakpointState::Disabled) {
        return true;  // Already disabled
    }

    if (bp->type == BreakpointType::Software) {
        auto result = restoreSoftwareBreakpoint(bp->address, bp->originalByte);
        if (result.isError()) {
            return Error(result.error());
        }
    }

    bp->state = BreakpointState::Disabled;

    logDebug("Breakpoint disabled: id=%u", id);
    return true;
}

Result<uint32_t> BreakpointManager::setTemporaryBreakpoint(uint64_t address) {
    auto result = setSoftwareBreakpoint(address, "temp");
    if (result.isError()) {
        return Error(result.error());
    }

    uint32_t id = result.value();
    auto* bp = getBreakpoint(id);
    if (bp) {
        bp->isTemporary = true;
        bp->state = BreakpointState::OneShot;
    }

    return id;
}

Result<bool> BreakpointManager::setCondition(uint32_t id, const std::string& condition) {
    auto* bp = getBreakpoint(id);
    if (!bp) {
        return Error(ErrorCategory::Breakpoint, "Breakpoint not found");
    }

    bp->condition = condition;
    bp->hasCondition = !condition.empty();

    logDebug("Breakpoint condition set: id=%u condition=%s", id, condition.c_str());
    return true;
}

Result<bool> BreakpointManager::clearCondition(uint32_t id) {
    auto* bp = getBreakpoint(id);
    if (!bp) {
        return Error(ErrorCategory::Breakpoint, "Breakpoint not found");
    }

    bp->condition.clear();
    bp->hasCondition = false;

    logDebug("Breakpoint condition cleared: id=%u", id);
    return true;
}

Result<bool> BreakpointManager::setIgnoreCount(uint32_t id, uint32_t count) {
    auto* bp = getBreakpoint(id);
    if (!bp) {
        return Error(ErrorCategory::Breakpoint, "Breakpoint not found");
    }

    bp->ignoreCount = count;

    logDebug("Breakpoint ignore count set: id=%u count=%u", id, count);
    return true;
}

const BreakpointInfo* BreakpointManager::getBreakpoint(uint32_t id) const {
    auto it = mBreakpoints.find(id);
    return it != mBreakpoints.end() ? &it->second : nullptr;
}

BreakpointInfo* BreakpointManager::getBreakpoint(uint32_t id) {
    auto it = mBreakpoints.find(id);
    return it != mBreakpoints.end() ? &it->second : nullptr;
}

const BreakpointInfo* BreakpointManager::getBreakpointAt(uint64_t address) const {
    for (const auto& pair : mBreakpoints) {
        if (pair.second.address == address) {
            return &pair.second;
        }
    }
    return nullptr;
}

bool BreakpointManager::hasBreakpoint(uint32_t id) const {
    return mBreakpoints.find(id) != mBreakpoints.end();
}

bool BreakpointManager::hasBreakpointAt(uint64_t address) const {
    return getBreakpointAt(address) != nullptr;
}

std::vector<uint32_t> BreakpointManager::getAllBreakpointIds() const {
    std::vector<uint32_t> ids;
    ids.reserve(mBreakpoints.size());

    for (const auto& pair : mBreakpoints) {
        ids.push_back(pair.first);
    }

    std::sort(ids.begin(), ids.end());
    return ids;
}

std::vector<const BreakpointInfo*> BreakpointManager::getBreakpointsInRange(uint64_t start, uint64_t end) const {
    std::vector<const BreakpointInfo*> result;

    for (const auto& pair : mBreakpoints) {
        if (pair.second.address >= start && pair.second.address <= end) {
            result.push_back(&pair.second);
        }
    }

    return result;
}

Result<bool> BreakpointManager::onBreakpointHit(uint32_t id) {
    auto* bp = getBreakpoint(id);
    if (!bp) {
        return Error(ErrorCategory::Breakpoint, "Breakpoint not found");
    }

    bp->hitCount++;

    logDebug("Breakpoint hit: id=%u count=%u", id, bp->hitCount);

    // Handle one-shot breakpoints
    if (bp->state == BreakpointState::OneShot || bp->isTemporary) {
        return removeBreakpoint(id);
    }

    return true;
}

bool BreakpointManager::shouldBreak(uint32_t id) {
    auto* bp = getBreakpoint(id);
    if (!bp) {
        return false;
    }

    // Check ignore count
    if (bp->ignoreCount > 0) {
        bp->ignoreCount--;
        logDebug("Breakpoint ignored: id=%u remaining=%u", id, bp->ignoreCount);
        return false;
    }

    // TODO: Evaluate condition if present
    // For now, always break if no condition or ignore count

    return true;
}

int BreakpointManager::allocateHardwareRegister() {
    for (int i = 0; i < 4; i++) {
        if (!mHardwareUsed[i]) {
            mHardwareUsed[i] = true;
            return i;
        }
    }
    return -1;
}

void BreakpointManager::freeHardwareRegister(int reg) {
    if (reg >= 0 && reg < 4) {
        mHardwareUsed[reg] = false;
    }
}

bool BreakpointManager::isHardwareRegisterUsed(int reg) const {
    return reg >= 0 && reg < 4 && mHardwareUsed[reg];
}

int BreakpointManager::getAvailableHardwareCount() const {
    int count = 0;
    for (int i = 0; i < 4; i++) {
        if (!mHardwareUsed[i]) {
            count++;
        }
    }
    return count;
}

void BreakpointManager::removeAllBreakpoints() {
    auto ids = getAllBreakpointIds();
    for (uint32_t id : ids) {
        removeBreakpoint(id);
    }

    logInfo("All breakpoints removed");
}

void BreakpointManager::removeTemporaryBreakpoints() {
    std::vector<uint32_t> toRemove;

    for (const auto& pair : mBreakpoints) {
        if (pair.second.isTemporary) {
            toRemove.push_back(pair.first);
        }
    }

    for (uint32_t id : toRemove) {
        removeBreakpoint(id);
    }

    logDebug("Temporary breakpoints removed: count=%zu", toRemove.size());
}

std::vector<uint32_t> BreakpointManager::getBreakpointsByType(BreakpointType type) const {
    std::vector<uint32_t> result;

    for (const auto& pair : mBreakpoints) {
        if (pair.second.type == type) {
            result.push_back(pair.first);
        }
    }

    return result;
}

Result<bool> BreakpointManager::saveOriginalByte(uint32_t id, uint8_t byte) {
    auto* bp = getBreakpoint(id);
    if (!bp) {
        return Error(ErrorCategory::Breakpoint, "Breakpoint not found");
    }

    bp->originalByte = byte;
    return true;
}

uint8_t BreakpointManager::getOriginalByte(uint32_t id) const {
    auto* bp = getBreakpoint(id);
    return bp ? bp->originalByte : 0;
}

void BreakpointManager::clear() {
    removeAllBreakpoints();
    mNextId = 1;

    for (int i = 0; i < 4; i++) {
        mHardwareUsed[i] = false;
    }

    logInfo("Breakpoint manager cleared");
}

uint32_t BreakpointManager::generateId() {
    return mNextId++;
}

Result<bool> BreakpointManager::writeSoftwareBreakpoint(uint64_t address, uint8_t& originalByte) {
    if (!mMemRead || !mMemWrite) {
        return Error(ErrorCategory::Breakpoint, "Memory callbacks not set");
    }

    // Read original byte
    if (!mMemRead(address, &originalByte, 1)) {
        return Error(ErrorCategory::Memory, "Failed to read original byte");
    }

    // Write INT3 (0xCC)
    uint8_t int3 = 0xCC;
    if (!mMemWrite(address, &int3, 1)) {
        return Error(ErrorCategory::Memory, "Failed to write INT3");
    }

    return true;
}

Result<bool> BreakpointManager::restoreSoftwareBreakpoint(uint64_t address, uint8_t originalByte) {
    if (!mMemWrite) {
        return Error(ErrorCategory::Breakpoint, "Memory callbacks not set");
    }

    // Restore original byte
    if (!mMemWrite(address, &originalByte, 1)) {
        return Error(ErrorCategory::Memory, "Failed to restore original byte");
    }

    return true;
}

} // namespace Gleam
