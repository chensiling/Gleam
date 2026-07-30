// Process and Thread management implementation

#include "ProcessManager.h"
#include "Log.h"
#include <algorithm>

namespace Gleam {

// ThreadManager implementation

ThreadManager::ThreadManager()
    : mSelectedThreadId(0) {
}

void ThreadManager::addThread(uint32_t threadId, HANDLE handle, uint64_t startAddress, uint64_t teb) {
    ThreadInfo info;
    info.threadId = threadId;
    info.handle = handle;
    info.startAddress = startAddress;
    info.teb = teb;
    info.isSuspended = false;
    info.suspendCount = 0;

    mThreads[threadId] = info;

    // Auto-select first thread
    if (mThreads.size() == 1) {
        mSelectedThreadId = threadId;
    }

    logDebug("Thread added: tid=%u start=0x%llX teb=0x%llX",
             threadId, (unsigned long long)startAddress, (unsigned long long)teb);
}

void ThreadManager::removeThread(uint32_t threadId) {
    auto it = mThreads.find(threadId);
    if (it == mThreads.end()) {
        logWarn("Attempt to remove unknown thread: tid=%u", threadId);
        return;
    }

    mThreads.erase(it);

    // If we removed the selected thread, select another one
    if (mSelectedThreadId == threadId) {
        if (!mThreads.empty()) {
            mSelectedThreadId = mThreads.begin()->first;
            logDebug("Selected thread changed to tid=%u", mSelectedThreadId);
        } else {
            mSelectedThreadId = 0;
        }
    }

    logDebug("Thread removed: tid=%u", threadId);
}

bool ThreadManager::hasThread(uint32_t threadId) const {
    return mThreads.find(threadId) != mThreads.end();
}

void ThreadManager::selectThread(uint32_t threadId) {
    if (!hasThread(threadId)) {
        logWarn("Cannot select unknown thread: tid=%u", threadId);
        return;
    }

    mSelectedThreadId = threadId;
    logDebug("Thread selected: tid=%u", threadId);
}

const ThreadInfo* ThreadManager::getSelectedThread() const {
    return getThread(mSelectedThreadId);
}

ThreadInfo* ThreadManager::getSelectedThread() {
    return getThread(mSelectedThreadId);
}

const ThreadInfo* ThreadManager::getThread(uint32_t threadId) const {
    auto it = mThreads.find(threadId);
    return it != mThreads.end() ? &it->second : nullptr;
}

ThreadInfo* ThreadManager::getThread(uint32_t threadId) {
    auto it = mThreads.find(threadId);
    return it != mThreads.end() ? &it->second : nullptr;
}

std::vector<uint32_t> ThreadManager::getAllThreadIds() const {
    std::vector<uint32_t> ids;
    ids.reserve(mThreads.size());

    for (const auto& pair : mThreads) {
        ids.push_back(pair.first);
    }

    std::sort(ids.begin(), ids.end());
    return ids;
}

Result<bool> ThreadManager::suspendThread(uint32_t threadId) {
    ThreadInfo* info = getThread(threadId);
    if (!info) {
        return Error(ErrorCategory::Process, "Thread not found");
    }

    if (!info->handle || info->handle == INVALID_HANDLE_VALUE) {
        return Error(ErrorCategory::Process, "Invalid thread handle");
    }

    DWORD result = SuspendThread(info->handle);
    if (result == static_cast<DWORD>(-1)) {
        return Error(ErrorCategory::Process, "SuspendThread failed", GetLastError());
    }

    info->isSuspended = true;
    info->suspendCount++;

    logDebug("Thread suspended: tid=%u count=%u", threadId, info->suspendCount);
    return true;
}

Result<bool> ThreadManager::resumeThread(uint32_t threadId) {
    ThreadInfo* info = getThread(threadId);
    if (!info) {
        return Error(ErrorCategory::Process, "Thread not found");
    }

    if (!info->handle || info->handle == INVALID_HANDLE_VALUE) {
        return Error(ErrorCategory::Process, "Invalid thread handle");
    }

    DWORD result = ResumeThread(info->handle);
    if (result == static_cast<DWORD>(-1)) {
        return Error(ErrorCategory::Process, "ResumeThread failed", GetLastError());
    }

    if (info->suspendCount > 0) {
        info->suspendCount--;
    }

    if (info->suspendCount == 0) {
        info->isSuspended = false;
    }

    logDebug("Thread resumed: tid=%u count=%u", threadId, info->suspendCount);
    return true;
}

bool ThreadManager::isThreadSuspended(uint32_t threadId) const {
    const ThreadInfo* info = getThread(threadId);
    return info ? info->isSuspended : false;
}

void ThreadManager::clear() {
    mThreads.clear();
    mSelectedThreadId = 0;
    logDebug("All threads cleared");
}

// ProcessManager implementation

ProcessManager::ProcessManager()
    : mAttached(false) {
}

void ProcessManager::attach(uint32_t processId, HANDLE handle, const std::string& imagePath,
                            uint64_t imageBase, uint64_t peb) {
    mProcess.processId = processId;
    mProcess.handle = handle;
    mProcess.imagePath = imagePath;
    mProcess.imageBase = imageBase;
    mProcess.peb = peb;
    mProcess.isWow64 = false;
    mAttached = true;

    logInfo("Process attached: pid=%u path=%s base=0x%llX",
            processId, imagePath.c_str(), (unsigned long long)imageBase);
}

void ProcessManager::detach() {
    if (mAttached) {
        logInfo("Process detached: pid=%u", mProcess.processId);
    }

    mProcess = ProcessInfo();
    mAttached = false;
}

bool ProcessManager::isValid() const {
    return mAttached &&
           mProcess.processId != 0 &&
           mProcess.handle != nullptr &&
           mProcess.handle != INVALID_HANDLE_VALUE;
}

// ProcessThreadManager implementation

ProcessThreadManager::ProcessThreadManager() {
}

void ProcessThreadManager::reset() {
    mThreadManager.clear();
    mProcessManager.detach();
    logInfo("Process and thread state reset");
}

} // namespace Gleam
