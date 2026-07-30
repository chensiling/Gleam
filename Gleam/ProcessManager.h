// Process and Thread management for GleamDebugger

#ifndef GLEAM_PROCESS_MANAGER_H
#define GLEAM_PROCESS_MANAGER_H

#include "Error.h"
#include "RaiiUtils.h"
#include <windows.h>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace Gleam {

// Thread information
struct ThreadInfo {
    uint32_t threadId;
    HANDLE handle;
    uint64_t startAddress;
    uint64_t teb;  // Thread Environment Block
    bool isSuspended;
    uint32_t suspendCount;

    ThreadInfo()
        : threadId(0), handle(nullptr), startAddress(0)
        , teb(0), isSuspended(false), suspendCount(0) {}
};

// Process information
struct ProcessInfo {
    uint32_t processId;
    HANDLE handle;
    uint64_t peb;  // Process Environment Block
    std::string imagePath;
    uint64_t imageBase;
    bool isWow64;

    ProcessInfo()
        : processId(0), handle(nullptr), peb(0)
        , imageBase(0), isWow64(false) {}
};

// Thread manager - manages all threads in the debugged process
class ThreadManager {
private:
    std::unordered_map<uint32_t, ThreadInfo> mThreads;
    uint32_t mSelectedThreadId;

public:
    ThreadManager();

    // Thread lifecycle
    void addThread(uint32_t threadId, HANDLE handle, uint64_t startAddress, uint64_t teb);
    void removeThread(uint32_t threadId);
    bool hasThread(uint32_t threadId) const;

    // Thread selection
    void selectThread(uint32_t threadId);
    uint32_t getSelectedThreadId() const { return mSelectedThreadId; }
    const ThreadInfo* getSelectedThread() const;
    ThreadInfo* getSelectedThread();

    // Thread queries
    const ThreadInfo* getThread(uint32_t threadId) const;
    ThreadInfo* getThread(uint32_t threadId);
    size_t getThreadCount() const { return mThreads.size(); }
    std::vector<uint32_t> getAllThreadIds() const;

    // Thread state
    Result<bool> suspendThread(uint32_t threadId);
    Result<bool> resumeThread(uint32_t threadId);
    bool isThreadSuspended(uint32_t threadId) const;

    // Clear all threads
    void clear();

    // Iteration
    const std::unordered_map<uint32_t, ThreadInfo>& getThreads() const { return mThreads; }
};

// Process manager - manages process state
class ProcessManager {
private:
    ProcessInfo mProcess;
    bool mAttached;

public:
    ProcessManager();

    // Process lifecycle
    void attach(uint32_t processId, HANDLE handle, const std::string& imagePath,
                uint64_t imageBase, uint64_t peb);
    void detach();
    bool isAttached() const { return mAttached; }

    // Process queries
    const ProcessInfo& getProcess() const { return mProcess; }
    uint32_t getProcessId() const { return mProcess.processId; }
    HANDLE getProcessHandle() const { return mProcess.handle; }
    uint64_t getPEB() const { return mProcess.peb; }
    uint64_t getImageBase() const { return mProcess.imageBase; }
    const std::string& getImagePath() const { return mProcess.imagePath; }

    // Process state
    void setWow64(bool isWow64) { mProcess.isWow64 = isWow64; }
    bool isWow64() const { return mProcess.isWow64; }

    // Validation
    bool isValid() const;
};

// Combined process and thread manager
class ProcessThreadManager {
private:
    ProcessManager mProcessManager;
    ThreadManager mThreadManager;

public:
    ProcessThreadManager();

    // Process management
    ProcessManager& processManager() { return mProcessManager; }
    const ProcessManager& processManager() const { return mProcessManager; }

    // Thread management
    ThreadManager& threadManager() { return mThreadManager; }
    const ThreadManager& threadManager() const { return mThreadManager; }

    // Convenience methods
    bool isAttached() const { return mProcessManager.isAttached(); }
    uint32_t getProcessId() const { return mProcessManager.getProcessId(); }
    HANDLE getProcessHandle() const { return mProcessManager.getProcessHandle(); }

    uint32_t getSelectedThreadId() const { return mThreadManager.getSelectedThreadId(); }
    const ThreadInfo* getSelectedThread() const { return mThreadManager.getSelectedThread(); }

    // Reset all state
    void reset();
};

} // namespace Gleam

#endif // GLEAM_PROCESS_MANAGER_H
