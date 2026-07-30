// RAII utility classes for Windows resource management
//
// These classes provide exception-safe, automatic resource cleanup
// following the RAII (Resource Acquisition Is Initialization) pattern.

#ifndef GLEAM_RAII_H
#define GLEAM_RAII_H

#include <windows.h>
#include <utility>

namespace Gleam {

// RAII wrapper for Windows HANDLE resources
// Automatically closes handle on destruction
class UniqueHandle {
public:
    // Construct from handle (takes ownership)
    explicit UniqueHandle(HANDLE handle = INVALID_HANDLE_VALUE) noexcept
        : mHandle(handle) {}

    // Destructor: close handle if valid
    ~UniqueHandle() noexcept {
        close();
    }

    // Non-copyable
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    // Movable
    UniqueHandle(UniqueHandle&& other) noexcept
        : mHandle(other.mHandle) {
        other.mHandle = INVALID_HANDLE_VALUE;
    }

    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            close();
            mHandle = other.mHandle;
            other.mHandle = INVALID_HANDLE_VALUE;
        }
        return *this;
    }

    // Get the raw handle (does not release ownership)
    HANDLE get() const noexcept {
        return mHandle;
    }

    // Release ownership and return the raw handle
    HANDLE release() noexcept {
        HANDLE h = mHandle;
        mHandle = INVALID_HANDLE_VALUE;
        return h;
    }

    // Replace the managed handle (closes old handle if valid)
    void reset(HANDLE newHandle = INVALID_HANDLE_VALUE) noexcept {
        if (mHandle != newHandle) {
            close();
            mHandle = newHandle;
        }
    }

    // Check if handle is valid
    explicit operator bool() const noexcept {
        return mHandle != INVALID_HANDLE_VALUE && mHandle != NULL;
    }

    // Swap with another UniqueHandle
    void swap(UniqueHandle& other) noexcept {
        HANDLE temp = mHandle;
        mHandle = other.mHandle;
        other.mHandle = temp;
    }

private:
    HANDLE mHandle;

    void close() noexcept {
        if (mHandle != INVALID_HANDLE_VALUE && mHandle != NULL) {
            CloseHandle(mHandle);
            mHandle = INVALID_HANDLE_VALUE;
        }
    }
};

// RAII wrapper for file mapping views (MapViewOfFile)
class MappedView {
public:
    explicit MappedView(void* view = nullptr) noexcept
        : mView(view) {}

    ~MappedView() noexcept {
        if (mView) {
            UnmapViewOfFile(mView);
            mView = nullptr;
        }
    }

    // Non-copyable
    MappedView(const MappedView&) = delete;
    MappedView& operator=(const MappedView&) = delete;

    // Movable
    MappedView(MappedView&& other) noexcept
        : mView(other.mView) {
        other.mView = nullptr;
    }

    MappedView& operator=(MappedView&& other) noexcept {
        if (this != &other) {
            if (mView) UnmapViewOfFile(mView);
            mView = other.mView;
            other.mView = nullptr;
        }
        return *this;
    }

    void* get() const noexcept { return mView; }
    const uint8_t* as_bytes() const noexcept { return static_cast<const uint8_t*>(mView); }

    void* release() noexcept {
        void* v = mView;
        mView = nullptr;
        return v;
    }

    void reset(void* newView = nullptr) noexcept {
        if (mView != newView) {
            if (mView) UnmapViewOfFile(mView);
            mView = newView;
        }
    }

    explicit operator bool() const noexcept { return mView != nullptr; }

private:
    void* mView;
};

// RAII wrapper for VirtualAllocEx memory
class VirtualMemoryGuard {
public:
    VirtualMemoryGuard(HANDLE process, void* address, size_t size) noexcept
        : mProcess(process), mAddress(address), mSize(size), mOwned(true) {}

    ~VirtualMemoryGuard() noexcept {
        free();
    }

    // Non-copyable
    VirtualMemoryGuard(const VirtualMemoryGuard&) = delete;
    VirtualMemoryGuard& operator=(const VirtualMemoryGuard&) = delete;

    // Movable
    VirtualMemoryGuard(VirtualMemoryGuard&& other) noexcept
        : mProcess(other.mProcess)
        , mAddress(other.mAddress)
        , mSize(other.mSize)
        , mOwned(other.mOwned) {
        other.mOwned = false;
    }

    VirtualMemoryGuard& operator=(VirtualMemoryGuard&& other) noexcept {
        if (this != &other) {
            free();
            mProcess = other.mProcess;
            mAddress = other.mAddress;
            mSize = other.mSize;
            mOwned = other.mOwned;
            other.mOwned = false;
        }
        return *this;
    }

    void* address() const noexcept { return mAddress; }
    size_t size() const noexcept { return mSize; }

    void* release() noexcept {
        mOwned = false;
        return mAddress;
    }

    void free() noexcept {
        if (mOwned && mAddress) {
            VirtualFreeEx(mProcess, mAddress, 0, MEM_RELEASE);
            mOwned = false;
            mAddress = nullptr;
        }
    }

    explicit operator bool() const noexcept { return mOwned && mAddress; }

private:
    HANDLE mProcess;
    void* mAddress;
    size_t mSize;
    bool mOwned;
};

// RAII wrapper for thread suspension
class ThreadSuspendGuard {
public:
    explicit ThreadSuspendGuard(HANDLE thread) noexcept
        : mThread(thread), mSuspended(false) {
        if (mThread && mThread != INVALID_HANDLE_VALUE) {
            DWORD result = SuspendThread(mThread);
            mSuspended = (result != static_cast<DWORD>(-1));
        }
    }

    ~ThreadSuspendGuard() noexcept {
        resume();
    }

    // Non-copyable, non-movable (suspension is tied to specific thread)
    ThreadSuspendGuard(const ThreadSuspendGuard&) = delete;
    ThreadSuspendGuard& operator=(const ThreadSuspendGuard&) = delete;
    ThreadSuspendGuard(ThreadSuspendGuard&&) = delete;
    ThreadSuspendGuard& operator=(ThreadSuspendGuard&&) = delete;

    bool isSuspended() const noexcept { return mSuspended; }

    void resume() noexcept {
        if (mSuspended && mThread && mThread != INVALID_HANDLE_VALUE) {
            ResumeThread(mThread);
            mSuspended = false;
        }
    }

private:
    HANDLE mThread;
    bool mSuspended;
};

// RAII wrapper for library (DLL) loading
class LibraryGuard {
public:
    explicit LibraryGuard(HMODULE module = nullptr) noexcept
        : mModule(module) {}

    ~LibraryGuard() noexcept {
        free();
    }

    // Non-copyable
    LibraryGuard(const LibraryGuard&) = delete;
    LibraryGuard& operator=(const LibraryGuard&) = delete;

    // Movable
    LibraryGuard(LibraryGuard&& other) noexcept
        : mModule(other.mModule) {
        other.mModule = nullptr;
    }

    LibraryGuard& operator=(LibraryGuard&& other) noexcept {
        if (this != &other) {
            free();
            mModule = other.mModule;
            other.mModule = nullptr;
        }
        return *this;
    }

    HMODULE get() const noexcept { return mModule; }

    HMODULE release() noexcept {
        HMODULE h = mModule;
        mModule = nullptr;
        return h;
    }

    void free() noexcept {
        if (mModule) {
            FreeLibrary(mModule);
            mModule = nullptr;
        }
    }

    explicit operator bool() const noexcept { return mModule != nullptr; }

private:
    HMODULE mModule;
};

// Generic scope exit guard - executes function on scope exit
template<typename Func>
class ScopeGuard {
public:
    explicit ScopeGuard(Func func) noexcept
        : mFunc(func), mDismissed(false) {}

    ~ScopeGuard() noexcept {
        if (!mDismissed) {
            mFunc();
        }
    }

    // Non-copyable
    ScopeGuard(const ScopeGuard&) = delete;
    ScopeGuard& operator=(const ScopeGuard&) = delete;

    void dismiss() noexcept {
        mDismissed = true;
    }

private:
    Func mFunc;
    bool mDismissed;
};

// Helper to create ScopeGuard
template<typename Func>
ScopeGuard<Func> makeScopeGuard(Func func) noexcept {
    return ScopeGuard<Func>(func);
}

} // namespace Gleam

#endif // GLEAM_RAII_H
