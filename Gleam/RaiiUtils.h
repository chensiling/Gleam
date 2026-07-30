/**
 * @file RaiiUtils.h
 * @brief RAII utility classes for Windows resource management
 *
 * Provides exception-safe, automatic resource cleanup following the RAII
 * (Resource Acquisition Is Initialization) pattern. Each wrapper:
 * - Acquires the resource on construction
 * - Releases the resource on destruction
 * - Prevents copying (to avoid double-free)
 * - Supports move semantics for ownership transfer
 *
 * **Design rationale:**
 * Manual resource cleanup (CloseHandle, UnmapViewOfFile, VirtualFreeEx, etc.)
 * is error-prone and unsafe in the presence of early returns or exceptions.
 * These RAII wrappers ensure resources are always released, even when
 * exceptional control flow occurs.
 *
 * @example Basic usage:
 * @code
 * UniqueHandle hProcess(OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid));
 * if (hProcess) {
 *     // Use hProcess.get() for Win32 API calls
 *     ReadProcessMemory(hProcess.get(), ...);
 * }  // Automatically closed here
 * @endcode
 */

#ifndef GLEAM_RAII_H
#define GLEAM_RAII_H

#include <windows.h>
#include <utility>

namespace Gleam {

/**
 * @brief RAII wrapper for Windows HANDLE resources
 *
 * Automatically closes the handle on destruction via CloseHandle().
 * Supports move semantics for ownership transfer. Non-copyable to
 * prevent double-close bugs.
 *
 * @example Process handle management:
 * @code
 * UniqueHandle hProcess(OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid));
 * if (!hProcess) {
 *     printf("Failed to open process\n");
 *     return;
 * }
 *
 * // Use in Win32 API calls
 * DWORD exitCode;
 * GetExitCodeProcess(hProcess.get(), &exitCode);
 * @endcode
 *
 * @example Ownership transfer:
 * @code
 * UniqueHandle createHandle() {
 *     UniqueHandle h(CreateEvent(nullptr, TRUE, FALSE, nullptr));
 *     return h;  // Move semantics transfer ownership
 * }
 * @endcode
 */
class UniqueHandle {
public:
    /**
     * @brief Construct from handle (takes ownership)
     * @param handle Windows handle to manage (default: INVALID_HANDLE_VALUE)
     */
    explicit UniqueHandle(HANDLE handle = INVALID_HANDLE_VALUE) noexcept
        : mHandle(handle) {}

    /**
     * @brief Destructor: close handle if valid
     *
     * Calls CloseHandle() if the handle is not INVALID_HANDLE_VALUE or NULL.
     */
    ~UniqueHandle() noexcept {
        close();
    }

    // Non-copyable (prevent double-close)
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    /**
     * @brief Move constructor: transfer ownership
     * @param other Source handle (becomes invalid after move)
     */
    UniqueHandle(UniqueHandle&& other) noexcept
        : mHandle(other.mHandle) {
        other.mHandle = INVALID_HANDLE_VALUE;
    }

    /**
     * @brief Move assignment: transfer ownership
     * @param other Source handle (becomes invalid after move)
     * @return Reference to this
     */
    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            close();
            mHandle = other.mHandle;
            other.mHandle = INVALID_HANDLE_VALUE;
        }
        return *this;
    }

    /**
     * @brief Get the raw handle (does not release ownership)
     * @return The managed HANDLE
     *
     * @note Use this to pass the handle to Win32 API functions
     */
    HANDLE get() const noexcept {
        return mHandle;
    }

    /**
     * @brief Release ownership and return the raw handle
     * @return The managed HANDLE (caller assumes ownership)
     *
     * @note After release(), the UniqueHandle no longer manages the resource
     */
    HANDLE release() noexcept {
        HANDLE h = mHandle;
        mHandle = INVALID_HANDLE_VALUE;
        return h;
    }

    /**
     * @brief Replace the managed handle (closes old handle if valid)
     * @param newHandle New handle to manage (default: INVALID_HANDLE_VALUE)
     *
     * @example
     * @code
     * UniqueHandle h(hOld);
     * h.reset(CreateEvent(...));  // Closes hOld, manages new event
     * @endcode
     */
    void reset(HANDLE newHandle = INVALID_HANDLE_VALUE) noexcept {
        if (mHandle != newHandle) {
            close();
            mHandle = newHandle;
        }
    }

    /**
     * @brief Check if handle is valid
     * @return true if handle is not INVALID_HANDLE_VALUE or NULL
     */
    explicit operator bool() const noexcept {
        return mHandle != INVALID_HANDLE_VALUE && mHandle != NULL;
    }

    /**
     * @brief Swap with another UniqueHandle
     * @param other Handle to swap with
     */
    void swap(UniqueHandle& other) noexcept {
        HANDLE temp = mHandle;
        mHandle = other.mHandle;
        other.mHandle = temp;
    }

private:
    HANDLE mHandle;  ///< Managed Windows handle

    void close() noexcept {
        if (mHandle != INVALID_HANDLE_VALUE && mHandle != NULL) {
            CloseHandle(mHandle);
            mHandle = INVALID_HANDLE_VALUE;
        }
    }
};

/**
 * @brief RAII wrapper for file mapping views (MapViewOfFile)
 *
 * Automatically unmaps the view on destruction via UnmapViewOfFile().
 *
 * @example Memory-mapped file:
 * @code
 * HANDLE hFile = CreateFile(...);
 * HANDLE hMapping = CreateFileMapping(hFile, ...);
 * MappedView view(MapViewOfFile(hMapping, FILE_MAP_READ, 0, 0, 0));
 *
 * if (view) {
 *     const uint8_t* data = view.as_bytes();
 *     // Read from mapped memory
 * }  // Automatically unmapped here
 * @endcode
 */
class MappedView {
public:
    /**
     * @brief Construct from mapped view pointer
     * @param view Pointer returned by MapViewOfFile (default: nullptr)
     */
    explicit MappedView(void* view = nullptr) noexcept
        : mView(view) {}

    /**
     * @brief Destructor: unmap the view if valid
     */
    ~MappedView() noexcept {
        if (mView) {
            UnmapViewOfFile(mView);
            mView = nullptr;
        }
    }

    // Non-copyable
    MappedView(const MappedView&) = delete;
    MappedView& operator=(const MappedView&) = delete;

    /** @brief Move constructor */
    MappedView(MappedView&& other) noexcept
        : mView(other.mView) {
        other.mView = nullptr;
    }

    /** @brief Move assignment */
    MappedView& operator=(MappedView&& other) noexcept {
        if (this != &other) {
            if (mView) UnmapViewOfFile(mView);
            mView = other.mView;
            other.mView = nullptr;
        }
        return *this;
    }

    /** @brief Get the raw pointer */
    void* get() const noexcept { return mView; }

    /** @brief Get the pointer as byte array */
    const uint8_t* as_bytes() const noexcept { return static_cast<const uint8_t*>(mView); }

    /** @brief Release ownership */
    void* release() noexcept {
        void* v = mView;
        mView = nullptr;
        return v;
    }

    /** @brief Replace the managed view */
    void reset(void* newView = nullptr) noexcept {
        if (mView != newView) {
            if (mView) UnmapViewOfFile(mView);
            mView = newView;
        }
    }

    /** @brief Check if view is valid */
    explicit operator bool() const noexcept { return mView != nullptr; }

private:
    void* mView;  ///< Mapped view pointer
};

/**
 * @brief RAII wrapper for VirtualAllocEx memory
 *
 * Automatically frees the allocated memory on destruction via VirtualFreeEx().
 *
 * @example Remote process memory allocation:
 * @code
 * HANDLE hProcess = OpenProcess(...);
 * void* addr = VirtualAllocEx(hProcess, nullptr, 4096, MEM_COMMIT, PAGE_READWRITE);
 * VirtualMemoryGuard guard(hProcess, addr, 4096);
 *
 * if (guard) {
 *     WriteProcessMemory(hProcess, guard.address(), data, size, nullptr);
 * }  // Automatically freed here
 * @endcode
 */
class VirtualMemoryGuard {
public:
    /**
     * @brief Construct and take ownership of allocated memory
     * @param process Process handle where memory is allocated
     * @param address Base address of the allocation
     * @param size Size of the allocation in bytes
     */
    VirtualMemoryGuard(HANDLE process, void* address, size_t size) noexcept
        : mProcess(process), mAddress(address), mSize(size), mOwned(true) {}

    /**
     * @brief Destructor: free the memory if owned
     */
    ~VirtualMemoryGuard() noexcept {
        free();
    }

    // Non-copyable
    VirtualMemoryGuard(const VirtualMemoryGuard&) = delete;
    VirtualMemoryGuard& operator=(const VirtualMemoryGuard&) = delete;

    /** @brief Move constructor */
    VirtualMemoryGuard(VirtualMemoryGuard&& other) noexcept
        : mProcess(other.mProcess)
        , mAddress(other.mAddress)
        , mSize(other.mSize)
        , mOwned(other.mOwned) {
        other.mOwned = false;
    }

    /** @brief Move assignment */
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

    /** @brief Get the base address */
    void* address() const noexcept { return mAddress; }

    /** @brief Get the allocation size */
    size_t size() const noexcept { return mSize; }

    /** @brief Release ownership (caller must free manually) */
    void* release() noexcept {
        mOwned = false;
        return mAddress;
    }

    /** @brief Manually free the memory early */
    void free() noexcept {
        if (mOwned && mAddress) {
            VirtualFreeEx(mProcess, mAddress, 0, MEM_RELEASE);
            mOwned = false;
            mAddress = nullptr;
        }
    }

    /** @brief Check if memory is still owned */
    explicit operator bool() const noexcept { return mOwned && mAddress; }

private:
    HANDLE mProcess;   ///< Process where memory is allocated
    void* mAddress;    ///< Base address
    size_t mSize;      ///< Allocation size
    bool mOwned;       ///< Ownership flag
};

/**
 * @brief RAII wrapper for thread suspension
 *
 * Automatically suspends a thread on construction and resumes it on destruction.
 * Non-movable to ensure suspension is properly paired with resume.
 *
 * @example Safe thread suspension:
 * @code
 * HANDLE hThread = OpenThread(THREAD_ALL_ACCESS, FALSE, tid);
 * {
 *     ThreadSuspendGuard guard(hThread);
 *     if (guard.isSuspended()) {
 *         // Thread is suspended, safe to inspect state
 *         CONTEXT ctx;
 *         GetThreadContext(hThread, &ctx);
 *     }
 * }  // Thread automatically resumed here
 * @endcode
 *
 * @warning Non-movable by design: suspension must be resumed on same thread
 */
class ThreadSuspendGuard {
public:
    /**
     * @brief Construct and suspend the thread
     * @param thread Handle to the thread to suspend
     *
     * @note If suspension fails, isSuspended() returns false
     */
    explicit ThreadSuspendGuard(HANDLE thread) noexcept
        : mThread(thread), mSuspended(false) {
        if (mThread && mThread != INVALID_HANDLE_VALUE) {
            DWORD result = SuspendThread(mThread);
            mSuspended = (result != static_cast<DWORD>(-1));
        }
    }

    /**
     * @brief Destructor: resume the thread if suspended
     */
    ~ThreadSuspendGuard() noexcept {
        resume();
    }

    // Non-copyable, non-movable (suspension tied to specific thread)
    ThreadSuspendGuard(const ThreadSuspendGuard&) = delete;
    ThreadSuspendGuard& operator=(const ThreadSuspendGuard&) = delete;
    ThreadSuspendGuard(ThreadSuspendGuard&&) = delete;
    ThreadSuspendGuard& operator=(ThreadSuspendGuard&&) = delete;

    /**
     * @brief Check if the thread was successfully suspended
     * @return true if SuspendThread() succeeded
     */
    bool isSuspended() const noexcept { return mSuspended; }

    /**
     * @brief Manually resume the thread early
     *
     * Safe to call multiple times.
     */
    void resume() noexcept {
        if (mSuspended && mThread && mThread != INVALID_HANDLE_VALUE) {
            ResumeThread(mThread);
            mSuspended = false;
        }
    }

private:
    HANDLE mThread;      ///< Thread handle
    bool mSuspended;     ///< Suspension state
};

/**
 * @brief RAII wrapper for library (DLL) loading
 *
 * Automatically frees the library on destruction via FreeLibrary().
 *
 * @example Dynamic library loading:
 * @code
 * LibraryGuard lib(LoadLibrary("kernel32.dll"));
 * if (lib) {
 *     auto createFile = (CreateFileW_t)GetProcAddress(lib.get(), "CreateFileW");
 *     if (createFile) {
 *         // Use the function
 *     }
 * }  // Library automatically freed here
 * @endcode
 */
class LibraryGuard {
public:
    /**
     * @brief Construct from HMODULE
     * @param module Module handle (default: nullptr)
     */
    explicit LibraryGuard(HMODULE module = nullptr) noexcept
        : mModule(module) {}

    /**
     * @brief Destructor: free the library if loaded
     */
    ~LibraryGuard() noexcept {
        free();
    }

    // Non-copyable
    LibraryGuard(const LibraryGuard&) = delete;
    LibraryGuard& operator=(const LibraryGuard&) = delete;

    /** @brief Move constructor */
    LibraryGuard(LibraryGuard&& other) noexcept
        : mModule(other.mModule) {
        other.mModule = nullptr;
    }

    /** @brief Move assignment */
    LibraryGuard& operator=(LibraryGuard&& other) noexcept {
        if (this != &other) {
            free();
            mModule = other.mModule;
            other.mModule = nullptr;
        }
        return *this;
    }

    /** @brief Get the module handle */
    HMODULE get() const noexcept { return mModule; }

    /** @brief Release ownership */
    HMODULE release() noexcept {
        HMODULE h = mModule;
        mModule = nullptr;
        return h;
    }

    /** @brief Manually free the library early */
    void free() noexcept {
        if (mModule) {
            FreeLibrary(mModule);
            mModule = nullptr;
        }
    }

    /** @brief Check if library is loaded */
    explicit operator bool() const noexcept { return mModule != nullptr; }

private:
    HMODULE mModule;  ///< Library handle
};

/**
 * @brief Generic scope exit guard - executes function on scope exit
 *
 * Provides a generic RAII pattern for arbitrary cleanup code. The provided
 * function is called on destruction unless explicitly dismissed.
 *
 * @tparam Func Callable type (lambda, function pointer, functor)
 *
 * @example Cleanup on scope exit:
 * @code
 * FILE* f = fopen("file.txt", "r");
 * auto guard = makeScopeGuard([&]() { if (f) fclose(f); });
 *
 * // Use file...
 * if (someCondition) {
 *     return;  // Guard ensures fclose() is called
 * }
 * @endcode
 *
 * @example Conditional cleanup:
 * @code
 * void* mem = malloc(1024);
 * auto guard = makeScopeGuard([&]() { free(mem); });
 *
 * if (processData(mem)) {
 *     guard.dismiss();  // Success, don't free
 *     return mem;       // Caller owns it now
 * }
 * // On failure path, guard still frees
 * @endcode
 */
template<typename Func>
class ScopeGuard {
public:
    /**
     * @brief Construct with cleanup function
     * @param func Function to call on destruction
     */
    explicit ScopeGuard(Func func) noexcept
        : mFunc(func), mDismissed(false) {}

    /**
     * @brief Destructor: call cleanup function unless dismissed
     */
    ~ScopeGuard() noexcept {
        if (!mDismissed) {
            mFunc();
        }
    }

    // Non-copyable
    ScopeGuard(const ScopeGuard&) = delete;
    ScopeGuard& operator=(const ScopeGuard&) = delete;

    /**
     * @brief Dismiss the guard (prevent cleanup on destruction)
     *
     * Use when the operation succeeded and cleanup is no longer needed.
     */
    void dismiss() noexcept {
        mDismissed = true;
    }

private:
    Func mFunc;          ///< Cleanup function
    bool mDismissed;     ///< Dismiss flag
};

/**
 * @brief Helper to create ScopeGuard with type deduction
 *
 * @tparam Func Callable type (deduced)
 * @param func Cleanup function
 * @return ScopeGuard<Func> instance
 *
 * @example
 * @code
 * auto guard = makeScopeGuard([&]() { cleanup(); });
 * @endcode
 */
template<typename Func>
ScopeGuard<Func> makeScopeGuard(Func func) noexcept {
    return ScopeGuard<Func>(func);
}

} // namespace Gleam

#endif // GLEAM_RAII_H
