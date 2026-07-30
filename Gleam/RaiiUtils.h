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

} // namespace Gleam

#endif // GLEAM_RAII_H
