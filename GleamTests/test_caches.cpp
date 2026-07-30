// Unit tests for GleamDebugger caching mechanisms

#include <gtest/gtest.h>
#include <string>
#include <unordered_map>
#include <unordered_set>

// Mock implementations for testing cache logic independently

class MockSymbolCache {
public:
    std::unordered_map<std::string, uint64_t> cache;

    uint64_t getCached(const std::string& key) const {
        auto it = cache.find(key);
        return it != cache.end() ? it->second : 0;
    }

    void store(const std::string& key, uint64_t value) {
        if (value != 0)
            cache[key] = value;
    }

    void clear() {
        cache.clear();
    }

    size_t size() const {
        return cache.size();
    }
};

class MockIltCache {
public:
    std::unordered_map<uint64_t, std::unordered_set<uint64_t>> cache;

    const std::unordered_set<uint64_t>* getTargets(uint64_t moduleBase) {
        auto it = cache.find(moduleBase);
        if (it != cache.end())
            return &it->second;

        // Simulate expensive scan - create dummy ILT targets
        std::unordered_set<uint64_t> targets;
        targets.insert(moduleBase + 0x1000);
        targets.insert(moduleBase + 0x2000);
        targets.insert(moduleBase + 0x3000);

        auto inserted = cache.emplace(moduleBase, std::move(targets));
        return &inserted.first->second;
    }

    void clear() {
        cache.clear();
    }

    size_t size() const {
        return cache.size();
    }
};

// Symbol Cache Tests

TEST(SymbolCacheTest, InitiallyEmpty) {
    MockSymbolCache cache;
    EXPECT_EQ(cache.size(), 0);
}

TEST(SymbolCacheTest, StoreAndRetrieve) {
    MockSymbolCache cache;

    cache.store("kernel32!CreateFileW", 0x7FFA12340000);

    EXPECT_EQ(cache.size(), 1);
    EXPECT_EQ(cache.getCached("kernel32!CreateFileW"), 0x7FFA12340000);
}

TEST(SymbolCacheTest, RetrieveNonExistent) {
    MockSymbolCache cache;

    EXPECT_EQ(cache.getCached("nonexistent!symbol"), 0);
}

TEST(SymbolCacheTest, StoreMultipleSymbols) {
    MockSymbolCache cache;

    cache.store("kernel32!CreateFileW", 0x7FFA12340000);
    cache.store("kernel32!ReadFile", 0x7FFA12345000);
    cache.store("ntdll!NtCreateFile", 0x7FFA23450000);

    EXPECT_EQ(cache.size(), 3);
    EXPECT_EQ(cache.getCached("kernel32!CreateFileW"), 0x7FFA12340000);
    EXPECT_EQ(cache.getCached("kernel32!ReadFile"), 0x7FFA12345000);
    EXPECT_EQ(cache.getCached("ntdll!NtCreateFile"), 0x7FFA23450000);
}

TEST(SymbolCacheTest, DoNotStoreZero) {
    MockSymbolCache cache;

    cache.store("failed!symbol", 0);

    EXPECT_EQ(cache.size(), 0);
    EXPECT_EQ(cache.getCached("failed!symbol"), 0);
}

TEST(SymbolCacheTest, OverwriteExisting) {
    MockSymbolCache cache;

    cache.store("test!func", 0x1000);
    cache.store("test!func", 0x2000);

    EXPECT_EQ(cache.size(), 1);
    EXPECT_EQ(cache.getCached("test!func"), 0x2000);
}

TEST(SymbolCacheTest, ClearCache) {
    MockSymbolCache cache;

    cache.store("kernel32!CreateFileW", 0x7FFA12340000);
    cache.store("kernel32!ReadFile", 0x7FFA12345000);
    EXPECT_EQ(cache.size(), 2);

    cache.clear();

    EXPECT_EQ(cache.size(), 0);
    EXPECT_EQ(cache.getCached("kernel32!CreateFileW"), 0);
}

TEST(SymbolCacheTest, PdbSymbolKeyFormat) {
    MockSymbolCache cache;

    uint64_t moduleBase = 0x140000000;
    std::string symbol = "MyFunction";

    char keyBuf[256];
    snprintf(keyBuf, sizeof(keyBuf), "%llX:%s",
             (unsigned long long)moduleBase, symbol.c_str());
    std::string cacheKey(keyBuf);

    cache.store(cacheKey, 0x140001000);

    EXPECT_EQ(cache.getCached(cacheKey), 0x140001000);
    EXPECT_EQ(cache.getCached("140000000:MyFunction"), 0x140001000);
}

// ILT Cache Tests

TEST(IltCacheTest, InitiallyEmpty) {
    MockIltCache cache;
    EXPECT_EQ(cache.size(), 0);
}

TEST(IltCacheTest, FirstAccessCreatesEntry) {
    MockIltCache cache;

    uint64_t moduleBase = 0x140000000;
    const auto* targets = cache.getTargets(moduleBase);

    ASSERT_NE(targets, nullptr);
    EXPECT_EQ(cache.size(), 1);
    EXPECT_EQ(targets->size(), 3);
    EXPECT_TRUE(targets->count(moduleBase + 0x1000) > 0);
    EXPECT_TRUE(targets->count(moduleBase + 0x2000) > 0);
    EXPECT_TRUE(targets->count(moduleBase + 0x3000) > 0);
}

TEST(IltCacheTest, SecondAccessHitsCache) {
    MockIltCache cache;

    uint64_t moduleBase = 0x140000000;

    // First access - creates entry
    const auto* targets1 = cache.getTargets(moduleBase);
    EXPECT_EQ(cache.size(), 1);

    // Second access - should return same set
    const auto* targets2 = cache.getTargets(moduleBase);
    EXPECT_EQ(cache.size(), 1);
    EXPECT_EQ(targets1, targets2);  // Same pointer = cache hit
}

TEST(IltCacheTest, MultipleModules) {
    MockIltCache cache;

    uint64_t base1 = 0x140000000;
    uint64_t base2 = 0x7FFA12340000;
    uint64_t base3 = 0x7FFA23450000;

    cache.getTargets(base1);
    cache.getTargets(base2);
    cache.getTargets(base3);

    EXPECT_EQ(cache.size(), 3);
}

TEST(IltCacheTest, ClearCache) {
    MockIltCache cache;

    cache.getTargets(0x140000000);
    cache.getTargets(0x7FFA12340000);
    EXPECT_EQ(cache.size(), 2);

    cache.clear();

    EXPECT_EQ(cache.size(), 0);
}

// Performance simulation tests

TEST(CachePerformanceTest, SymbolCacheSavesLookups) {
    MockSymbolCache cache;
    int lookupCount = 0;

    auto expensiveLookup = [&](const std::string& symbol) -> uint64_t {
        lookupCount++;
        // Simulate expensive dbghelp operation
        return 0x7FFA12340000 + (lookupCount * 0x1000);
    };

    std::string symbol = "kernel32!CreateFileW";

    // First lookup - cache miss
    uint64_t cached = cache.getCached(symbol);
    if (cached == 0) {
        cached = expensiveLookup(symbol);
        cache.store(symbol, cached);
    }
    EXPECT_EQ(lookupCount, 1);

    // Subsequent lookups - cache hits
    for (int i = 0; i < 100; i++) {
        uint64_t addr = cache.getCached(symbol);
        EXPECT_NE(addr, 0);
    }
    EXPECT_EQ(lookupCount, 1);  // Still only 1 expensive lookup
}

TEST(CachePerformanceTest, IltCacheSavesScans) {
    MockIltCache cache;
    int scanCount = 0;

    auto expensiveScan = [&](uint64_t base) -> std::unordered_set<uint64_t> {
        scanCount++;
        // Simulate expensive PE scan
        std::unordered_set<uint64_t> targets;
        targets.insert(base + 0x1000);
        return targets;
    };

    uint64_t moduleBase = 0x140000000;

    // First access - triggers scan
    const auto* targets1 = cache.getTargets(moduleBase);
    EXPECT_EQ(scanCount, 1);

    // Subsequent accesses - no scan
    for (int i = 0; i < 100; i++) {
        const auto* targets = cache.getTargets(moduleBase);
        EXPECT_EQ(targets, targets1);
    }
    EXPECT_EQ(scanCount, 1);  // Still only 1 expensive scan
}

// Cache invalidation tests

TEST(CacheInvalidationTest, SymbolCacheClearedOnRestart) {
    MockSymbolCache cache;

    // Populate cache for "session 1"
    cache.store("kernel32!CreateFileW", 0x7FFA12340000);
    cache.store("kernel32!ReadFile", 0x7FFA12345000);
    EXPECT_EQ(cache.size(), 2);

    // Simulate process restart - must clear cache
    cache.clear();

    EXPECT_EQ(cache.size(), 0);
    EXPECT_EQ(cache.getCached("kernel32!CreateFileW"), 0);
}

TEST(CacheInvalidationTest, IltCacheClearedOnRestart) {
    MockIltCache cache;

    // Populate cache for "session 1"
    cache.getTargets(0x140000000);
    cache.getTargets(0x7FFA12340000);
    EXPECT_EQ(cache.size(), 2);

    // Simulate process restart - must clear cache
    cache.clear();

    EXPECT_EQ(cache.size(), 0);
}
