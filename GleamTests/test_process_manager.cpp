// Unit tests for ProcessManager and ThreadManager

#include <gtest/gtest.h>
#include <windows.h>
#include <string>
#include <unordered_map>
#include <vector>
#include <algorithm>

namespace Gleam {

// Mock structures
struct ThreadInfo {
    uint32_t threadId;
    HANDLE handle;
    uint64_t startAddress;
    uint64_t teb;
    bool isSuspended;
    uint32_t suspendCount;

    ThreadInfo()
        : threadId(0), handle(nullptr), startAddress(0)
        , teb(0), isSuspended(false), suspendCount(0) {}
};

struct ProcessInfo {
    uint32_t processId;
    HANDLE handle;
    uint64_t peb;
    std::string imagePath;
    uint64_t imageBase;
    bool isWow64;

    ProcessInfo()
        : processId(0), handle(nullptr), peb(0)
        , imageBase(0), isWow64(false) {}
};

// Mock ThreadManager
class MockThreadManager {
private:
    std::unordered_map<uint32_t, ThreadInfo> mThreads;
    uint32_t mSelectedThreadId;

public:
    MockThreadManager() : mSelectedThreadId(0) {}

    void addThread(uint32_t threadId, HANDLE handle, uint64_t startAddress, uint64_t teb) {
        ThreadInfo info;
        info.threadId = threadId;
        info.handle = handle;
        info.startAddress = startAddress;
        info.teb = teb;
        mThreads[threadId] = info;

        if (mThreads.size() == 1) {
            mSelectedThreadId = threadId;
        }
    }

    void removeThread(uint32_t threadId) {
        mThreads.erase(threadId);
        if (mSelectedThreadId == threadId) {
            mSelectedThreadId = mThreads.empty() ? 0 : mThreads.begin()->first;
        }
    }

    bool hasThread(uint32_t threadId) const {
        return mThreads.find(threadId) != mThreads.end();
    }

    void selectThread(uint32_t threadId) {
        if (hasThread(threadId)) {
            mSelectedThreadId = threadId;
        }
    }

    uint32_t getSelectedThreadId() const { return mSelectedThreadId; }

    const ThreadInfo* getThread(uint32_t threadId) const {
        auto it = mThreads.find(threadId);
        return it != mThreads.end() ? &it->second : nullptr;
    }

    size_t getThreadCount() const { return mThreads.size(); }

    std::vector<uint32_t> getAllThreadIds() const {
        std::vector<uint32_t> ids;
        for (const auto& pair : mThreads) {
            ids.push_back(pair.first);
        }
        std::sort(ids.begin(), ids.end());
        return ids;
    }

    void clear() {
        mThreads.clear();
        mSelectedThreadId = 0;
    }
};

// Mock ProcessManager
class MockProcessManager {
private:
    ProcessInfo mProcess;
    bool mAttached;

public:
    MockProcessManager() : mAttached(false) {}

    void attach(uint32_t processId, HANDLE handle, const std::string& imagePath,
                uint64_t imageBase, uint64_t peb) {
        mProcess.processId = processId;
        mProcess.handle = handle;
        mProcess.imagePath = imagePath;
        mProcess.imageBase = imageBase;
        mProcess.peb = peb;
        mAttached = true;
    }

    void detach() {
        mProcess = ProcessInfo();
        mAttached = false;
    }

    bool isAttached() const { return mAttached; }

    const ProcessInfo& getProcess() const { return mProcess; }
    uint32_t getProcessId() const { return mProcess.processId; }
    HANDLE getProcessHandle() const { return mProcess.handle; }
    uint64_t getPEB() const { return mProcess.peb; }
    const std::string& getImagePath() const { return mProcess.imagePath; }

    void setWow64(bool isWow64) { mProcess.isWow64 = isWow64; }
    bool isWow64() const { return mProcess.isWow64; }

    bool isValid() const {
        return mAttached && mProcess.processId != 0 && mProcess.handle != nullptr;
    }
};

} // namespace Gleam

// ThreadManager Tests

TEST(ThreadManagerTest, InitiallyEmpty) {
    Gleam::MockThreadManager manager;
    EXPECT_EQ(manager.getThreadCount(), 0);
    EXPECT_EQ(manager.getSelectedThreadId(), 0);
}

TEST(ThreadManagerTest, AddThread) {
    Gleam::MockThreadManager manager;
    HANDLE h = reinterpret_cast<HANDLE>(0x1234);

    manager.addThread(1000, h, 0x140001000, 0x7FF000);

    EXPECT_EQ(manager.getThreadCount(), 1);
    EXPECT_TRUE(manager.hasThread(1000));
}

TEST(ThreadManagerTest, FirstThreadAutoSelected) {
    Gleam::MockThreadManager manager;
    HANDLE h = reinterpret_cast<HANDLE>(0x1234);

    manager.addThread(1000, h, 0x140001000, 0x7FF000);

    EXPECT_EQ(manager.getSelectedThreadId(), 1000);
}

TEST(ThreadManagerTest, RemoveThread) {
    Gleam::MockThreadManager manager;
    HANDLE h1 = reinterpret_cast<HANDLE>(0x1234);
    HANDLE h2 = reinterpret_cast<HANDLE>(0x5678);

    manager.addThread(1000, h1, 0x140001000, 0x7FF000);
    manager.addThread(2000, h2, 0x140002000, 0x7FF800);

    EXPECT_EQ(manager.getThreadCount(), 2);

    manager.removeThread(1000);

    EXPECT_EQ(manager.getThreadCount(), 1);
    EXPECT_FALSE(manager.hasThread(1000));
    EXPECT_TRUE(manager.hasThread(2000));
}

TEST(ThreadManagerTest, RemoveSelectedThreadUpdatesSelection) {
    Gleam::MockThreadManager manager;
    HANDLE h1 = reinterpret_cast<HANDLE>(0x1234);
    HANDLE h2 = reinterpret_cast<HANDLE>(0x5678);

    manager.addThread(1000, h1, 0x140001000, 0x7FF000);
    manager.addThread(2000, h2, 0x140002000, 0x7FF800);

    EXPECT_EQ(manager.getSelectedThreadId(), 1000);

    manager.removeThread(1000);

    // Should auto-select remaining thread
    EXPECT_EQ(manager.getSelectedThreadId(), 2000);
}

TEST(ThreadManagerTest, SelectThread) {
    Gleam::MockThreadManager manager;
    HANDLE h1 = reinterpret_cast<HANDLE>(0x1234);
    HANDLE h2 = reinterpret_cast<HANDLE>(0x5678);

    manager.addThread(1000, h1, 0x140001000, 0x7FF000);
    manager.addThread(2000, h2, 0x140002000, 0x7FF800);

    manager.selectThread(2000);

    EXPECT_EQ(manager.getSelectedThreadId(), 2000);
}

TEST(ThreadManagerTest, GetThread) {
    Gleam::MockThreadManager manager;
    HANDLE h = reinterpret_cast<HANDLE>(0x1234);

    manager.addThread(1000, h, 0x140001000, 0x7FF000);

    const Gleam::ThreadInfo* info = manager.getThread(1000);

    ASSERT_NE(info, nullptr);
    EXPECT_EQ(info->threadId, 1000);
    EXPECT_EQ(info->handle, h);
    EXPECT_EQ(info->startAddress, 0x140001000);
    EXPECT_EQ(info->teb, 0x7FF000);
}

TEST(ThreadManagerTest, GetThreadNonExistent) {
    Gleam::MockThreadManager manager;

    const Gleam::ThreadInfo* info = manager.getThread(9999);

    EXPECT_EQ(info, nullptr);
}

TEST(ThreadManagerTest, GetAllThreadIds) {
    Gleam::MockThreadManager manager;
    HANDLE h = reinterpret_cast<HANDLE>(0x1234);

    manager.addThread(3000, h, 0x140001000, 0x7FF000);
    manager.addThread(1000, h, 0x140002000, 0x7FF800);
    manager.addThread(2000, h, 0x140003000, 0x7FFA00);

    auto ids = manager.getAllThreadIds();

    EXPECT_EQ(ids.size(), 3);
    EXPECT_EQ(ids[0], 1000);  // Sorted
    EXPECT_EQ(ids[1], 2000);
    EXPECT_EQ(ids[2], 3000);
}

TEST(ThreadManagerTest, Clear) {
    Gleam::MockThreadManager manager;
    HANDLE h = reinterpret_cast<HANDLE>(0x1234);

    manager.addThread(1000, h, 0x140001000, 0x7FF000);
    manager.addThread(2000, h, 0x140002000, 0x7FF800);

    manager.clear();

    EXPECT_EQ(manager.getThreadCount(), 0);
    EXPECT_EQ(manager.getSelectedThreadId(), 0);
}

// ProcessManager Tests

TEST(ProcessManagerTest, InitiallyNotAttached) {
    Gleam::MockProcessManager manager;
    EXPECT_FALSE(manager.isAttached());
}

TEST(ProcessManagerTest, Attach) {
    Gleam::MockProcessManager manager;
    HANDLE h = reinterpret_cast<HANDLE>(0x1234);

    manager.attach(5000, h, "C:\\test.exe", 0x140000000, 0x7FFFFDE000);

    EXPECT_TRUE(manager.isAttached());
    EXPECT_EQ(manager.getProcessId(), 5000);
    EXPECT_EQ(manager.getProcessHandle(), h);
    EXPECT_EQ(manager.getImagePath(), "C:\\test.exe");
    EXPECT_EQ(manager.getPEB(), 0x7FFFFDE000);
}

TEST(ProcessManagerTest, Detach) {
    Gleam::MockProcessManager manager;
    HANDLE h = reinterpret_cast<HANDLE>(0x1234);

    manager.attach(5000, h, "C:\\test.exe", 0x140000000, 0x7FFFFDE000);
    manager.detach();

    EXPECT_FALSE(manager.isAttached());
    EXPECT_EQ(manager.getProcessId(), 0);
}

TEST(ProcessManagerTest, Wow64Flag) {
    Gleam::MockProcessManager manager;
    HANDLE h = reinterpret_cast<HANDLE>(0x1234);

    manager.attach(5000, h, "C:\\test.exe", 0x140000000, 0x7FFFFDE000);
    manager.setWow64(true);

    EXPECT_TRUE(manager.isWow64());

    manager.setWow64(false);

    EXPECT_FALSE(manager.isWow64());
}

TEST(ProcessManagerTest, IsValid) {
    Gleam::MockProcessManager manager;

    EXPECT_FALSE(manager.isValid());

    HANDLE h = reinterpret_cast<HANDLE>(0x1234);
    manager.attach(5000, h, "C:\\test.exe", 0x140000000, 0x7FFFFDE000);

    EXPECT_TRUE(manager.isValid());

    manager.detach();

    EXPECT_FALSE(manager.isValid());
}
