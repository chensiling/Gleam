// Test suite for SymbolResolver
// Uses standalone mocks to avoid GleeBug dependencies

#include <gtest/gtest.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <cstdint>

// Mock GleeBug::Process for testing
namespace GleeBug {
    class Process {
    public:
        // Mock methods as needed
        bool MemReadSafe(uint64_t, void*, size_t) { return true; }
    };
}

// Include the actual header after mocks
#include "../Gleam/SymbolResolver.h"

using namespace Gleam;

// ============================================================================
// SymbolSpec Tests
// ============================================================================

TEST(SymbolSpecTest, ParseUnqualified) {
    auto spec = SymbolSpec::parse("CreateFileW");
    EXPECT_TRUE(spec.module.empty());
    EXPECT_EQ(spec.symbol, "CreateFileW");
    EXPECT_FALSE(spec.isModuleQualified());
}

TEST(SymbolSpecTest, ParseQualified) {
    auto spec = SymbolSpec::parse("kernel32!CreateFileW");
    EXPECT_EQ(spec.module, "kernel32");
    EXPECT_EQ(spec.symbol, "CreateFileW");
    EXPECT_TRUE(spec.isModuleQualified());
}

TEST(SymbolSpecTest, ParseMultipleBangs) {
    // First bang is the delimiter
    auto spec = SymbolSpec::parse("module!sym!bol");
    EXPECT_EQ(spec.module, "module");
    EXPECT_EQ(spec.symbol, "sym!bol");
}

TEST(SymbolSpecTest, FormatUnqualified) {
    SymbolSpec spec;
    spec.symbol = "CreateFileW";
    EXPECT_EQ(spec.format(), "CreateFileW");
}

TEST(SymbolSpecTest, FormatQualified) {
    SymbolSpec spec;
    spec.module = "kernel32";
    spec.symbol = "CreateFileW";
    EXPECT_EQ(spec.format(), "kernel32!CreateFileW");
}

// ============================================================================
// SymbolResolver Basic Tests
// ============================================================================

TEST(SymbolResolverTest, InitialState) {
    GleeBug::Process mockProcess;
    SymbolResolver resolver(&mockProcess);

    auto stats = resolver.getStats();
    EXPECT_EQ(stats.cacheHits, 0u);
    EXPECT_EQ(stats.cacheMisses, 0u);
    EXPECT_EQ(stats.iltCacheHits, 0u);
    EXPECT_DOUBLE_EQ(stats.hitRatio(), 0.0);
}

TEST(SymbolResolverTest, ProcessAccess) {
    GleeBug::Process mockProcess;
    SymbolResolver resolver(&mockProcess);

    EXPECT_EQ(resolver.process(), &mockProcess);
}

TEST(SymbolResolverTest, ResolveNotFoundWithoutDbgHelp) {
    GleeBug::Process mockProcess;
    SymbolResolver resolver(&mockProcess);

    auto result = resolver.resolve("kernel32!CreateFileW");
    EXPECT_EQ(result.status, SymbolStatus::NotFound);
    EXPECT_TRUE(result.isNotFound());
    EXPECT_FALSE(result.isFound());
    EXPECT_FALSE(result.isAmbiguous());
    EXPECT_EQ(result.address(), 0u);
}

TEST(SymbolResolverTest, AddressToSymbol) {
    GleeBug::Process mockProcess;
    SymbolResolver resolver(&mockProcess);

    auto result = resolver.addressToSymbol(0x12345678);
    EXPECT_TRUE(result.isOk());
    EXPECT_NE(result.value().find("12345678"), std::string::npos);
}

TEST(SymbolResolverTest, AddressToSymbolZero) {
    GleeBug::Process mockProcess;
    SymbolResolver resolver(&mockProcess);

    auto result = resolver.addressToSymbol(0);
    EXPECT_TRUE(result.isError());
    EXPECT_EQ(result.error().category, ErrorCategory::Symbol);
}

// ============================================================================
// Cache Tests
// ============================================================================

TEST(SymbolResolverTest, CacheInvalidation) {
    GleeBug::Process mockProcess;
    SymbolResolver resolver(&mockProcess);

    // Stats start empty
    auto stats = resolver.getStats();
    EXPECT_EQ(stats.cacheHits, 0u);

    // Invalidate (no-op when empty)
    resolver.invalidateSymbolCache();
    resolver.invalidateIltCache(0x12340000);
    resolver.invalidateAllCaches();

    // Stats unchanged
    stats = resolver.getStats();
    EXPECT_EQ(stats.cacheHits, 0u);
}

TEST(SymbolResolverTest, StatsReset) {
    GleeBug::Process mockProcess;
    SymbolResolver resolver(&mockProcess);

    // Trigger some misses
    resolver.resolve("test");
    resolver.resolve("another");

    auto stats = resolver.getStats();
    EXPECT_EQ(stats.cacheMisses, 2u);

    // Reset
    resolver.resetStats();
    stats = resolver.getStats();
    EXPECT_EQ(stats.cacheMisses, 0u);
    EXPECT_EQ(stats.cacheHits, 0u);
}

TEST(SymbolResolverTest, HitRatioCalculation) {
    GleeBug::Process mockProcess;
    SymbolResolver resolver(&mockProcess);

    // No operations yet
    EXPECT_DOUBLE_EQ(resolver.getStats().hitRatio(), 0.0);

    // After some misses (would need actual cache hits for non-zero ratio)
    resolver.resolve("test");
    auto stats = resolver.getStats();
    EXPECT_DOUBLE_EQ(stats.hitRatio(), 0.0);  // 0 hits / 1 total = 0%
}

// ============================================================================
// ILT Disambiguation Tests
// ============================================================================

TEST(SymbolResolverTest, PickLiveCandidateEmpty) {
    std::vector<uint64_t> candidates;
    std::unordered_set<uint64_t> iltTargets;

    int result = SymbolResolver::pickLiveCandidate(candidates, iltTargets);
    EXPECT_EQ(result, -1);
}

TEST(SymbolResolverTest, PickLiveCandidateSingle) {
    std::vector<uint64_t> candidates = {0x1000};
    std::unordered_set<uint64_t> iltTargets;

    int result = SymbolResolver::pickLiveCandidate(candidates, iltTargets);
    EXPECT_EQ(result, 0);  // Single candidate always accepted
}

TEST(SymbolResolverTest, PickLiveCandidateOneZombie) {
    std::vector<uint64_t> candidates = {0x1000, 0x2000};
    std::unordered_set<uint64_t> iltTargets = {0x2000};

    int result = SymbolResolver::pickLiveCandidate(candidates, iltTargets);
    EXPECT_EQ(result, 1);  // Second candidate is live
}

TEST(SymbolResolverTest, PickLiveCandidateAllZombies) {
    std::vector<uint64_t> candidates = {0x1000, 0x4000};
    std::unordered_set<uint64_t> iltTargets = {0x2000, 0x3000};

    int result = SymbolResolver::pickLiveCandidate(candidates, iltTargets);
    EXPECT_EQ(result, -1);  // No candidate is in ILT targets
}

TEST(SymbolResolverTest, PickLiveCandidateAmbiguous) {
    std::vector<uint64_t> candidates = {0x2000, 0x3000};
    std::unordered_set<uint64_t> iltTargets = {0x2000, 0x3000};

    int result = SymbolResolver::pickLiveCandidate(candidates, iltTargets);
    EXPECT_EQ(result, -1);  // Multiple live candidates = ambiguous
}

TEST(SymbolResolverTest, PickLiveCandidateThreeWithOneAlive) {
    std::vector<uint64_t> candidates = {0x1000, 0x2000, 0x4000};
    std::unordered_set<uint64_t> iltTargets = {0x2000};

    int result = SymbolResolver::pickLiveCandidate(candidates, iltTargets);
    EXPECT_EQ(result, 1);  // Middle candidate is the unique live one
}

// ============================================================================
// ILT Cache Tests
// ============================================================================

TEST(SymbolResolverTest, IltCachePopulation) {
    GleeBug::Process mockProcess;
    SymbolResolver resolver(&mockProcess);

    uint64_t moduleBase = 0x10000000;

    // First access - cache miss
    auto targets1 = resolver.getIltTargets(moduleBase);
    EXPECT_NE(targets1, nullptr);
    auto stats1 = resolver.getStats();
    EXPECT_EQ(stats1.iltCacheHits, 0u);

    // Second access - cache hit
    auto targets2 = resolver.getIltTargets(moduleBase);
    EXPECT_EQ(targets1, targets2);  // Same pointer = cached
    auto stats2 = resolver.getStats();
    EXPECT_EQ(stats2.iltCacheHits, 1u);
}

TEST(SymbolResolverTest, IltCacheWarmup) {
    GleeBug::Process mockProcess;
    SymbolResolver resolver(&mockProcess);

    uint64_t moduleBase = 0x10000000;

    // Warmup
    resolver.warmIltCache(moduleBase);

    // Next access should be a hit
    resolver.resetStats();
    resolver.getIltTargets(moduleBase);
    auto stats = resolver.getStats();
    EXPECT_EQ(stats.iltCacheHits, 1u);
}

TEST(SymbolResolverTest, IltCacheInvalidationByModule) {
    GleeBug::Process mockProcess;
    SymbolResolver resolver(&mockProcess);

    uint64_t moduleBase1 = 0x10000000;
    uint64_t moduleBase2 = 0x20000000;

    // Populate both
    resolver.getIltTargets(moduleBase1);
    resolver.getIltTargets(moduleBase2);

    // Invalidate one
    resolver.resetStats();
    resolver.invalidateIltCache(moduleBase1);

    // Base1 should miss, Base2 should hit
    resolver.getIltTargets(moduleBase1);
    auto stats1 = resolver.getStats();
    EXPECT_EQ(stats1.iltCacheHits, 0u);

    resolver.getIltTargets(moduleBase2);
    auto stats2 = resolver.getStats();
    EXPECT_EQ(stats2.iltCacheHits, 1u);
}

// ============================================================================
// ResolvedSymbol and SymbolLookupResult Tests
// ============================================================================

TEST(ResolvedSymbolTest, BasicFields) {
    ResolvedSymbol sym;
    sym.address = 0x12345678;
    sym.module = "kernel32";
    sym.name = "CreateFileW";
    sym.isIltTarget = true;

    EXPECT_EQ(sym.address, 0x12345678u);
    EXPECT_EQ(sym.module, "kernel32");
    EXPECT_EQ(sym.name, "CreateFileW");
    EXPECT_TRUE(sym.isIltTarget);
}

TEST(SymbolLookupResultTest, StatusChecks) {
    SymbolLookupResult result;

    result.status = SymbolStatus::Found;
    EXPECT_TRUE(result.isFound());
    EXPECT_FALSE(result.isAmbiguous());
    EXPECT_FALSE(result.isNotFound());

    result.status = SymbolStatus::Ambiguous;
    EXPECT_FALSE(result.isFound());
    EXPECT_TRUE(result.isAmbiguous());
    EXPECT_FALSE(result.isNotFound());

    result.status = SymbolStatus::NotFound;
    EXPECT_FALSE(result.isFound());
    EXPECT_FALSE(result.isAmbiguous());
    EXPECT_TRUE(result.isNotFound());
}

TEST(SymbolLookupResultTest, AddressConvenience) {
    SymbolLookupResult result;
    result.status = SymbolStatus::Found;
    result.candidates.push_back({0x1000, "mod", "sym", false});

    EXPECT_EQ(result.address(), 0x1000u);
}

TEST(SymbolLookupResultTest, AddressZeroWhenNotFound) {
    SymbolLookupResult result;
    result.status = SymbolStatus::NotFound;

    EXPECT_EQ(result.address(), 0u);
}

TEST(SymbolLookupResultTest, AddressZeroWhenEmpty) {
    SymbolLookupResult result;
    result.status = SymbolStatus::Found;
    // No candidates

    EXPECT_EQ(result.address(), 0u);
}
