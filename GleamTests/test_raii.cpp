// Unit tests for RAII utilities

#include <gtest/gtest.h>
#include <windows.h>

namespace Gleam {

// Mock UniqueHandle for testing
class MockUniqueHandle {
private:
    HANDLE mHandle;
    static int sCloseCount;

public:
    explicit MockUniqueHandle(HANDLE handle = INVALID_HANDLE_VALUE)
        : mHandle(handle) {}

    ~MockUniqueHandle() {
        close();
    }

    MockUniqueHandle(const MockUniqueHandle&) = delete;
    MockUniqueHandle& operator=(const MockUniqueHandle&) = delete;

    MockUniqueHandle(MockUniqueHandle&& other) noexcept
        : mHandle(other.mHandle) {
        other.mHandle = INVALID_HANDLE_VALUE;
    }

    MockUniqueHandle& operator=(MockUniqueHandle&& other) noexcept {
        if (this != &other) {
            close();
            mHandle = other.mHandle;
            other.mHandle = INVALID_HANDLE_VALUE;
        }
        return *this;
    }

    HANDLE get() const { return mHandle; }

    HANDLE release() {
        HANDLE h = mHandle;
        mHandle = INVALID_HANDLE_VALUE;
        return h;
    }

    void reset(HANDLE newHandle = INVALID_HANDLE_VALUE) {
        close();
        mHandle = newHandle;
    }

    bool isValid() const {
        return mHandle != INVALID_HANDLE_VALUE && mHandle != nullptr;
    }

    explicit operator bool() const {
        return isValid();
    }

    void close() {
        if (isValid()) {
            sCloseCount++;
            mHandle = INVALID_HANDLE_VALUE;
        }
    }

    static void resetCloseCount() { sCloseCount = 0; }
    static int getCloseCount() { return sCloseCount; }
};

int MockUniqueHandle::sCloseCount = 0;

// Mock VirtualMemoryGuard
class MockVirtualMemoryGuard {
private:
    HANDLE mProcess;
    void* mAddress;
    size_t mSize;
    bool mOwned;
    static int sFreeCount;

public:
    MockVirtualMemoryGuard(HANDLE process, void* address, size_t size)
        : mProcess(process), mAddress(address), mSize(size), mOwned(true) {}

    ~MockVirtualMemoryGuard() {
        free();
    }

    MockVirtualMemoryGuard(const MockVirtualMemoryGuard&) = delete;
    MockVirtualMemoryGuard& operator=(const MockVirtualMemoryGuard&) = delete;

    MockVirtualMemoryGuard(MockVirtualMemoryGuard&& other) noexcept
        : mProcess(other.mProcess)
        , mAddress(other.mAddress)
        , mSize(other.mSize)
        , mOwned(other.mOwned) {
        other.mOwned = false;
    }

    void* address() const { return mAddress; }
    size_t size() const { return mSize; }
    bool isOwned() const { return mOwned; }

    void* release() {
        mOwned = false;
        return mAddress;
    }

    void free() {
        if (mOwned && mAddress) {
            sFreeCount++;
            mOwned = false;
            mAddress = nullptr;
        }
    }

    static void resetFreeCount() { sFreeCount = 0; }
    static int getFreeCount() { return sFreeCount; }
};

int MockVirtualMemoryGuard::sFreeCount = 0;

// Mock ScopeGuard
template<typename Func>
class MockScopeGuard {
private:
    Func mFunc;
    bool mDismissed;

public:
    explicit MockScopeGuard(Func func)
        : mFunc(func), mDismissed(false) {}

    ~MockScopeGuard() {
        if (!mDismissed) {
            mFunc();
        }
    }

    MockScopeGuard(const MockScopeGuard&) = delete;
    MockScopeGuard& operator=(const MockScopeGuard&) = delete;

    void dismiss() {
        mDismissed = true;
    }
};

} // namespace Gleam

// UniqueHandle Tests

TEST(UniqueHandleTest, DefaultConstructor) {
    Gleam::MockUniqueHandle handle;
    EXPECT_FALSE(handle.isValid());
    EXPECT_EQ(handle.get(), INVALID_HANDLE_VALUE);
}

TEST(UniqueHandleTest, ConstructWithHandle) {
    HANDLE h = reinterpret_cast<HANDLE>(0x1234);
    Gleam::MockUniqueHandle handle(h);
    EXPECT_TRUE(handle.isValid());
    EXPECT_EQ(handle.get(), h);
}

TEST(UniqueHandleTest, AutomaticClose) {
    Gleam::MockUniqueHandle::resetCloseCount();

    {
        HANDLE h = reinterpret_cast<HANDLE>(0x1234);
        Gleam::MockUniqueHandle handle(h);
    } // Destructor called here

    EXPECT_EQ(Gleam::MockUniqueHandle::getCloseCount(), 1);
}

TEST(UniqueHandleTest, Release) {
    Gleam::MockUniqueHandle::resetCloseCount();

    HANDLE h = reinterpret_cast<HANDLE>(0x1234);
    Gleam::MockUniqueHandle handle(h);

    HANDLE released = handle.release();
    EXPECT_EQ(released, h);
    EXPECT_FALSE(handle.isValid());

    // Destructor should not close after release
    handle.close();
    EXPECT_EQ(Gleam::MockUniqueHandle::getCloseCount(), 0);
}

TEST(UniqueHandleTest, Reset) {
    Gleam::MockUniqueHandle::resetCloseCount();

    HANDLE h1 = reinterpret_cast<HANDLE>(0x1234);
    HANDLE h2 = reinterpret_cast<HANDLE>(0x5678);

    Gleam::MockUniqueHandle handle(h1);
    handle.reset(h2);

    EXPECT_EQ(handle.get(), h2);
    EXPECT_EQ(Gleam::MockUniqueHandle::getCloseCount(), 1); // h1 closed
}

TEST(UniqueHandleTest, MoveConstructor) {
    Gleam::MockUniqueHandle::resetCloseCount();

    HANDLE h = reinterpret_cast<HANDLE>(0x1234);
    Gleam::MockUniqueHandle handle1(h);
    Gleam::MockUniqueHandle handle2(std::move(handle1));

    EXPECT_FALSE(handle1.isValid());
    EXPECT_TRUE(handle2.isValid());
    EXPECT_EQ(handle2.get(), h);
    EXPECT_EQ(Gleam::MockUniqueHandle::getCloseCount(), 0);
}

TEST(UniqueHandleTest, MoveAssignment) {
    Gleam::MockUniqueHandle::resetCloseCount();

    HANDLE h1 = reinterpret_cast<HANDLE>(0x1234);
    HANDLE h2 = reinterpret_cast<HANDLE>(0x5678);

    Gleam::MockUniqueHandle handle1(h1);
    Gleam::MockUniqueHandle handle2(h2);

    handle2 = std::move(handle1);

    EXPECT_FALSE(handle1.isValid());
    EXPECT_TRUE(handle2.isValid());
    EXPECT_EQ(handle2.get(), h1);
    EXPECT_EQ(Gleam::MockUniqueHandle::getCloseCount(), 1); // h2 closed
}

// VirtualMemoryGuard Tests

TEST(VirtualMemoryGuardTest, Constructor) {
    void* addr = reinterpret_cast<void*>(0x140000000);
    Gleam::MockVirtualMemoryGuard guard(nullptr, addr, 4096);

    EXPECT_EQ(guard.address(), addr);
    EXPECT_EQ(guard.size(), 4096);
    EXPECT_TRUE(guard.isOwned());
}

TEST(VirtualMemoryGuardTest, AutomaticFree) {
    Gleam::MockVirtualMemoryGuard::resetFreeCount();

    {
        void* addr = reinterpret_cast<void*>(0x140000000);
        Gleam::MockVirtualMemoryGuard guard(nullptr, addr, 4096);
    } // Destructor called here

    EXPECT_EQ(Gleam::MockVirtualMemoryGuard::getFreeCount(), 1);
}

TEST(VirtualMemoryGuardTest, Release) {
    Gleam::MockVirtualMemoryGuard::resetFreeCount();

    void* addr = reinterpret_cast<void*>(0x140000000);
    Gleam::MockVirtualMemoryGuard guard(nullptr, addr, 4096);

    void* released = guard.release();
    EXPECT_EQ(released, addr);
    EXPECT_FALSE(guard.isOwned());

    // Destructor should not free after release
    guard.free();
    EXPECT_EQ(Gleam::MockVirtualMemoryGuard::getFreeCount(), 0);
}

TEST(VirtualMemoryGuardTest, MoveConstructor) {
    Gleam::MockVirtualMemoryGuard::resetFreeCount();

    void* addr = reinterpret_cast<void*>(0x140000000);
    Gleam::MockVirtualMemoryGuard guard1(nullptr, addr, 4096);
    Gleam::MockVirtualMemoryGuard guard2(std::move(guard1));

    EXPECT_FALSE(guard1.isOwned());
    EXPECT_TRUE(guard2.isOwned());
    EXPECT_EQ(guard2.address(), addr);
    EXPECT_EQ(Gleam::MockVirtualMemoryGuard::getFreeCount(), 0);
}

// ScopeGuard Tests

TEST(ScopeGuardTest, ExecutesOnDestruction) {
    int counter = 0;

    {
        Gleam::MockScopeGuard<std::function<void()>> guard([&]() {
            counter++;
        });
    } // Destructor executes lambda

    EXPECT_EQ(counter, 1);
}

TEST(ScopeGuardTest, Dismiss) {
    int counter = 0;

    {
        Gleam::MockScopeGuard<std::function<void()>> guard([&]() {
            counter++;
        });
        guard.dismiss();
    } // Destructor does not execute lambda

    EXPECT_EQ(counter, 0);
}

TEST(ScopeGuardTest, MultipleActions) {
    int counter = 0;

    {
        Gleam::MockScopeGuard<std::function<void()>> guard1([&]() { counter += 1; });
        Gleam::MockScopeGuard<std::function<void()>> guard2([&]() { counter += 2; });
        Gleam::MockScopeGuard<std::function<void()>> guard3([&]() { counter += 4; });
    } // All execute in reverse order

    EXPECT_EQ(counter, 7);
}
