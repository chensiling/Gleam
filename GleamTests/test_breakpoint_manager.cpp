// Unit tests for BreakpointManager

#include <gtest/gtest.h>
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>

// Use the real Error/Result types (see note in test_error.cpp): local mocks in
// namespace Gleam would be an ODR violation against the real definitions.
#include "../Gleam/Error.h"

namespace Gleam {

// Mock enums and structures
enum class BreakpointType { Software, Hardware, Memory };
enum class BreakpointState { Enabled, Disabled, OneShot, Pending };
enum class HardwareCondition { Execute, Write, ReadWrite, IO };
enum class HardwareSize { Byte = 1, Word = 2, Dword = 4, Qword = 8 };

struct BreakpointInfo {
    uint32_t id;
    BreakpointType type;
    BreakpointState state;
    uint64_t address;
    uint8_t originalByte;
    HardwareCondition hwCondition;
    HardwareSize hwSize;
    int hwRegister;
    std::string condition;
    bool hasCondition;
    uint32_t hitCount;
    uint32_t ignoreCount;
    std::string name;
    bool isTemporary;

    BreakpointInfo()
        : id(0), type(BreakpointType::Software), state(BreakpointState::Enabled)
        , address(0), originalByte(0), hwCondition(HardwareCondition::Execute)
        , hwSize(HardwareSize::Byte), hwRegister(-1), hasCondition(false)
        , hitCount(0), ignoreCount(0), isTemporary(false) {}
};

// Mock BreakpointManager
class MockBreakpointManager {
private:
    std::unordered_map<uint32_t, BreakpointInfo> mBreakpoints;
    uint32_t mNextId;
    bool mHardwareUsed[4];
    std::unordered_map<uint64_t, uint8_t> mMemory;

public:
    MockBreakpointManager() : mNextId(1) {
        for (int i = 0; i < 4; i++) {
            mHardwareUsed[i] = false;
        }
    }

    Result<uint32_t> setSoftwareBreakpoint(uint64_t address, const std::string& name = "") {
        if (hasBreakpointAt(address)) {
            return Error(ErrorCategory::Breakpoint, "Breakpoint already exists");
        }

        BreakpointInfo info;
        info.id = mNextId++;
        info.type = BreakpointType::Software;
        info.state = BreakpointState::Enabled;
        info.address = address;
        info.name = name;
        info.originalByte = 0x90;  // Mock NOP

        mBreakpoints[info.id] = info;
        mMemory[address] = 0xCC;  // INT3

        return info.id;
    }

    Result<uint32_t> setHardwareBreakpoint(uint64_t address, HardwareCondition condition,
                                            HardwareSize size, const std::string& name = "") {
        if (hasBreakpointAt(address)) {
            return Error(ErrorCategory::Breakpoint, "Breakpoint already exists");
        }

        int reg = allocateHardwareRegister();
        if (reg < 0) {
            return Error(ErrorCategory::Breakpoint, "No available hardware registers");
        }

        BreakpointInfo info;
        info.id = mNextId++;
        info.type = BreakpointType::Hardware;
        info.state = BreakpointState::Enabled;
        info.address = address;
        info.name = name;
        info.hwCondition = condition;
        info.hwSize = size;
        info.hwRegister = reg;

        mBreakpoints[info.id] = info;
        return info.id;
    }

    Result<bool> removeBreakpoint(uint32_t id) {
        auto* bp = getBreakpoint(id);
        if (!bp) {
            return Error(ErrorCategory::Breakpoint, "Breakpoint not found");
        }

        if (bp->type == BreakpointType::Hardware && bp->hwRegister >= 0) {
            freeHardwareRegister(bp->hwRegister);
        }

        mMemory.erase(bp->address);
        mBreakpoints.erase(id);
        return true;
    }

    Result<bool> enableBreakpoint(uint32_t id) {
        auto* bp = getBreakpoint(id);
        if (!bp) {
            return Error(ErrorCategory::Breakpoint, "Breakpoint not found");
        }
        bp->state = BreakpointState::Enabled;
        return true;
    }

    Result<bool> disableBreakpoint(uint32_t id) {
        auto* bp = getBreakpoint(id);
        if (!bp) {
            return Error(ErrorCategory::Breakpoint, "Breakpoint not found");
        }
        bp->state = BreakpointState::Disabled;
        return true;
    }

    Result<uint32_t> setTemporaryBreakpoint(uint64_t address) {
        auto result = setSoftwareBreakpoint(address, "temp");
        if (result.isOk()) {
            auto* bp = getBreakpoint(result.value());
            if (bp) {
                bp->isTemporary = true;
                bp->state = BreakpointState::OneShot;
            }
        }
        return result;
    }

    Result<bool> setCondition(uint32_t id, const std::string& condition) {
        auto* bp = getBreakpoint(id);
        if (!bp) {
            return Error(ErrorCategory::Breakpoint, "Breakpoint not found");
        }
        bp->condition = condition;
        bp->hasCondition = !condition.empty();
        return true;
    }

    Result<bool> setIgnoreCount(uint32_t id, uint32_t count) {
        auto* bp = getBreakpoint(id);
        if (!bp) {
            return Error(ErrorCategory::Breakpoint, "Breakpoint not found");
        }
        bp->ignoreCount = count;
        return true;
    }

    const BreakpointInfo* getBreakpoint(uint32_t id) const {
        auto it = mBreakpoints.find(id);
        return it != mBreakpoints.end() ? &it->second : nullptr;
    }

    BreakpointInfo* getBreakpoint(uint32_t id) {
        auto it = mBreakpoints.find(id);
        return it != mBreakpoints.end() ? &it->second : nullptr;
    }

    const BreakpointInfo* getBreakpointAt(uint64_t address) const {
        for (const auto& pair : mBreakpoints) {
            if (pair.second.address == address) {
                return &pair.second;
            }
        }
        return nullptr;
    }

    bool hasBreakpoint(uint32_t id) const {
        return mBreakpoints.find(id) != mBreakpoints.end();
    }

    bool hasBreakpointAt(uint64_t address) const {
        return getBreakpointAt(address) != nullptr;
    }

    size_t getBreakpointCount() const { return mBreakpoints.size(); }

    Result<bool> onBreakpointHit(uint32_t id) {
        auto* bp = getBreakpoint(id);
        if (!bp) {
            return Error(ErrorCategory::Breakpoint, "Breakpoint not found");
        }
        bp->hitCount++;
        if (bp->state == BreakpointState::OneShot || bp->isTemporary) {
            return removeBreakpoint(id);
        }
        return true;
    }

    bool shouldBreak(uint32_t id) {
        auto* bp = getBreakpoint(id);
        if (!bp) return false;

        if (bp->ignoreCount > 0) {
            bp->ignoreCount--;
            return false;
        }
        return true;
    }

    int allocateHardwareRegister() {
        for (int i = 0; i < 4; i++) {
            if (!mHardwareUsed[i]) {
                mHardwareUsed[i] = true;
                return i;
            }
        }
        return -1;
    }

    void freeHardwareRegister(int reg) {
        if (reg >= 0 && reg < 4) {
            mHardwareUsed[reg] = false;
        }
    }

    int getAvailableHardwareCount() const {
        int count = 0;
        for (int i = 0; i < 4; i++) {
            if (!mHardwareUsed[i]) count++;
        }
        return count;
    }

    void clear() {
        mBreakpoints.clear();
        mMemory.clear();
        mNextId = 1;
        for (int i = 0; i < 4; i++) {
            mHardwareUsed[i] = false;
        }
    }
};

} // namespace Gleam

// Tests

TEST(BreakpointManagerTest, InitiallyEmpty) {
    Gleam::MockBreakpointManager mgr;
    EXPECT_EQ(mgr.getBreakpointCount(), 0);
}

TEST(BreakpointManagerTest, SetSoftwareBreakpoint) {
    Gleam::MockBreakpointManager mgr;

    auto result = mgr.setSoftwareBreakpoint(0x140001000, "test");

    EXPECT_TRUE(result.isOk());
    EXPECT_GT(result.value(), 0);
    EXPECT_EQ(mgr.getBreakpointCount(), 1);
}

TEST(BreakpointManagerTest, DuplicateBreakpointFails) {
    Gleam::MockBreakpointManager mgr;

    mgr.setSoftwareBreakpoint(0x140001000, "test1");
    auto result = mgr.setSoftwareBreakpoint(0x140001000, "test2");

    EXPECT_TRUE(result.isError());
    EXPECT_EQ(mgr.getBreakpointCount(), 1);
}

TEST(BreakpointManagerTest, RemoveBreakpoint) {
    Gleam::MockBreakpointManager mgr;

    auto id = mgr.setSoftwareBreakpoint(0x140001000).value();
    auto result = mgr.removeBreakpoint(id);

    EXPECT_TRUE(result.isOk());
    EXPECT_EQ(mgr.getBreakpointCount(), 0);
}

TEST(BreakpointManagerTest, EnableDisableBreakpoint) {
    Gleam::MockBreakpointManager mgr;

    auto id = mgr.setSoftwareBreakpoint(0x140001000).value();
    auto* bp = mgr.getBreakpoint(id);

    EXPECT_EQ(bp->state, Gleam::BreakpointState::Enabled);

    mgr.disableBreakpoint(id);
    EXPECT_EQ(bp->state, Gleam::BreakpointState::Disabled);

    mgr.enableBreakpoint(id);
    EXPECT_EQ(bp->state, Gleam::BreakpointState::Enabled);
}

TEST(BreakpointManagerTest, HardwareBreakpoint) {
    Gleam::MockBreakpointManager mgr;

    auto result = mgr.setHardwareBreakpoint(0x140001000,
                                            Gleam::HardwareCondition::Write,
                                            Gleam::HardwareSize::Dword,
                                            "hwbp");

    EXPECT_TRUE(result.isOk());

    auto* bp = mgr.getBreakpoint(result.value());
    ASSERT_NE(bp, nullptr);
    EXPECT_EQ(bp->type, Gleam::BreakpointType::Hardware);
    EXPECT_EQ(bp->hwCondition, Gleam::HardwareCondition::Write);
    EXPECT_EQ(bp->hwSize, Gleam::HardwareSize::Dword);
    EXPECT_GE(bp->hwRegister, 0);
    EXPECT_LE(bp->hwRegister, 3);
}

TEST(BreakpointManagerTest, HardwareRegisterAllocation) {
    Gleam::MockBreakpointManager mgr;

    EXPECT_EQ(mgr.getAvailableHardwareCount(), 4);

    // Allocate all 4 hardware breakpoints
    std::vector<uint32_t> ids;
    for (int i = 0; i < 4; i++) {
        auto result = mgr.setHardwareBreakpoint(0x140001000 + i * 0x10,
                                                Gleam::HardwareCondition::Execute,
                                                Gleam::HardwareSize::Byte);
        EXPECT_TRUE(result.isOk());
        ids.push_back(result.value());
    }

    EXPECT_EQ(mgr.getAvailableHardwareCount(), 0);

    // 5th should fail
    auto result = mgr.setHardwareBreakpoint(0x140001040,
                                            Gleam::HardwareCondition::Execute,
                                            Gleam::HardwareSize::Byte);
    EXPECT_TRUE(result.isError());
}

TEST(BreakpointManagerTest, TemporaryBreakpoint) {
    Gleam::MockBreakpointManager mgr;

    auto id = mgr.setTemporaryBreakpoint(0x140001000).value();
    auto* bp = mgr.getBreakpoint(id);

    ASSERT_NE(bp, nullptr);
    EXPECT_TRUE(bp->isTemporary);
    EXPECT_EQ(bp->state, Gleam::BreakpointState::OneShot);
}

TEST(BreakpointManagerTest, OneShotBreakpointRemoved) {
    Gleam::MockBreakpointManager mgr;

    auto id = mgr.setTemporaryBreakpoint(0x140001000).value();

    EXPECT_EQ(mgr.getBreakpointCount(), 1);

    mgr.onBreakpointHit(id);

    EXPECT_EQ(mgr.getBreakpointCount(), 0);
}

TEST(BreakpointManagerTest, SetCondition) {
    Gleam::MockBreakpointManager mgr;

    auto id = mgr.setSoftwareBreakpoint(0x140001000).value();
    mgr.setCondition(id, "rax == 5");

    auto* bp = mgr.getBreakpoint(id);
    ASSERT_NE(bp, nullptr);
    EXPECT_TRUE(bp->hasCondition);
    EXPECT_EQ(bp->condition, "rax == 5");
}

TEST(BreakpointManagerTest, IgnoreCount) {
    Gleam::MockBreakpointManager mgr;

    auto id = mgr.setSoftwareBreakpoint(0x140001000).value();
    mgr.setIgnoreCount(id, 3);

    EXPECT_FALSE(mgr.shouldBreak(id));  // Ignored (2 left)
    EXPECT_FALSE(mgr.shouldBreak(id));  // Ignored (1 left)
    EXPECT_FALSE(mgr.shouldBreak(id));  // Ignored (0 left)
    EXPECT_TRUE(mgr.shouldBreak(id));   // Should break now
}

TEST(BreakpointManagerTest, HitCount) {
    Gleam::MockBreakpointManager mgr;

    auto id = mgr.setSoftwareBreakpoint(0x140001000).value();

    for (int i = 0; i < 5; i++) {
        mgr.onBreakpointHit(id);
    }

    auto* bp = mgr.getBreakpoint(id);
    ASSERT_NE(bp, nullptr);
    EXPECT_EQ(bp->hitCount, 5);
}

TEST(BreakpointManagerTest, HasBreakpointAt) {
    Gleam::MockBreakpointManager mgr;

    EXPECT_FALSE(mgr.hasBreakpointAt(0x140001000));

    mgr.setSoftwareBreakpoint(0x140001000);

    EXPECT_TRUE(mgr.hasBreakpointAt(0x140001000));
    EXPECT_FALSE(mgr.hasBreakpointAt(0x140002000));
}

TEST(BreakpointManagerTest, GetBreakpointAt) {
    Gleam::MockBreakpointManager mgr;

    auto id = mgr.setSoftwareBreakpoint(0x140001000, "test").value();
    auto* bp = mgr.getBreakpointAt(0x140001000);

    ASSERT_NE(bp, nullptr);
    EXPECT_EQ(bp->id, id);
    EXPECT_EQ(bp->address, 0x140001000);
    EXPECT_EQ(bp->name, "test");
}

TEST(BreakpointManagerTest, Clear) {
    Gleam::MockBreakpointManager mgr;

    mgr.setSoftwareBreakpoint(0x140001000);
    mgr.setSoftwareBreakpoint(0x140002000);
    mgr.setHardwareBreakpoint(0x140003000, Gleam::HardwareCondition::Write,
                              Gleam::HardwareSize::Dword);

    EXPECT_EQ(mgr.getBreakpointCount(), 3);

    mgr.clear();

    EXPECT_EQ(mgr.getBreakpointCount(), 0);
    EXPECT_EQ(mgr.getAvailableHardwareCount(), 4);
}
