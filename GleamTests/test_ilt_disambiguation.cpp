// Unit tests for ILT disambiguation logic

#include <gtest/gtest.h>
#include <vector>
#include <unordered_set>

// Forward declaration of the function to test
int pickLiveSymbolCandidate(const std::vector<uint64_t>& candidates,
                            const std::unordered_set<uint64_t>& iltTargets);

// Test fixture for ILT disambiguation
class IltDisambiguationTest : public ::testing::Test {
protected:
    std::vector<uint64_t> candidates;
    std::unordered_set<uint64_t> iltTargets;

    void SetUp() override {
        candidates.clear();
        iltTargets.clear();
    }
};

// Basic functionality tests

TEST_F(IltDisambiguationTest, EmptyCandidatesReturnsError) {
    int result = pickLiveSymbolCandidate(candidates, iltTargets);
    EXPECT_EQ(result, -1);
}

TEST_F(IltDisambiguationTest, SingleCandidateAlwaysSucceeds) {
    candidates.push_back(0x140001000);

    // Should succeed even if not in ILT (no disambiguation needed)
    int result = pickLiveSymbolCandidate(candidates, iltTargets);
    EXPECT_EQ(result, 0);
}

TEST_F(IltDisambiguationTest, TwoCandidatesOneInIlt) {
    candidates.push_back(0x140001000);  // Zombie
    candidates.push_back(0x140002000);  // Live

    iltTargets.insert(0x140002000);

    int result = pickLiveSymbolCandidate(candidates, iltTargets);
    EXPECT_EQ(result, 1);  // Second candidate is live
}

TEST_F(IltDisambiguationTest, TwoCandidatesNoneInIlt) {
    candidates.push_back(0x140001000);
    candidates.push_back(0x140002000);

    // No ILT targets - all zombies, cannot disambiguate
    int result = pickLiveSymbolCandidate(candidates, iltTargets);
    EXPECT_EQ(result, -1);
}

TEST_F(IltDisambiguationTest, TwoCandidatesBothInIlt) {
    candidates.push_back(0x140001000);
    candidates.push_back(0x140002000);

    iltTargets.insert(0x140001000);
    iltTargets.insert(0x140002000);

    // Ambiguous - both are live
    int result = pickLiveSymbolCandidate(candidates, iltTargets);
    EXPECT_EQ(result, -1);
}

TEST_F(IltDisambiguationTest, ManyCandidatesOneInIlt) {
    // Simulate incremental linking with many zombie records
    candidates.push_back(0x140001000);  // Zombie 1
    candidates.push_back(0x140002000);  // Zombie 2
    candidates.push_back(0x140003000);  // Live
    candidates.push_back(0x140004000);  // Zombie 3
    candidates.push_back(0x140005000);  // Zombie 4

    iltTargets.insert(0x140003000);

    int result = pickLiveSymbolCandidate(candidates, iltTargets);
    EXPECT_EQ(result, 2);  // Third candidate is live
}

TEST_F(IltDisambiguationTest, FirstCandidateIsLive) {
    candidates.push_back(0x140001000);  // Live
    candidates.push_back(0x140002000);  // Zombie
    candidates.push_back(0x140003000);  // Zombie

    iltTargets.insert(0x140001000);

    int result = pickLiveSymbolCandidate(candidates, iltTargets);
    EXPECT_EQ(result, 0);  // First candidate is live
}

TEST_F(IltDisambiguationTest, LastCandidateIsLive) {
    candidates.push_back(0x140001000);  // Zombie
    candidates.push_back(0x140002000);  // Zombie
    candidates.push_back(0x140003000);  // Live

    iltTargets.insert(0x140003000);

    int result = pickLiveSymbolCandidate(candidates, iltTargets);
    EXPECT_EQ(result, 2);  // Last candidate is live
}

// Edge cases

TEST_F(IltDisambiguationTest, LargeIltSetSmallCandidates) {
    // ILT contains many targets, but only one matches our candidates
    for (uint64_t addr = 0x140001000; addr < 0x140010000; addr += 0x1000) {
        iltTargets.insert(addr);
    }

    candidates.push_back(0x140005000);  // In ILT
    candidates.push_back(0x150001000);  // Not in ILT (different module)

    int result = pickLiveSymbolCandidate(candidates, iltTargets);
    EXPECT_EQ(result, 0);  // First candidate matches ILT
}

TEST_F(IltDisambiguationTest, ManyCandidatesNoneInIlt) {
    // All candidates are zombies
    for (uint64_t addr = 0x140001000; addr < 0x140010000; addr += 0x1000) {
        candidates.push_back(addr);
    }

    // ILT points to different addresses (perhaps from a different module)
    for (uint64_t addr = 0x150001000; addr < 0x150010000; addr += 0x1000) {
        iltTargets.insert(addr);
    }

    int result = pickLiveSymbolCandidate(candidates, iltTargets);
    EXPECT_EQ(result, -1);  // Cannot disambiguate
}

// Real-world scenario tests

TEST_F(IltDisambiguationTest, IncrementalLinkingScenario) {
    // Typical incremental linking: 3 records for same function across rebuilds
    // Only the latest is referenced by ILT
    candidates.push_back(0x140001200);  // Build 1 (zombie)
    candidates.push_back(0x140003400);  // Build 2 (zombie)
    candidates.push_back(0x140005600);  // Build 3 (current)

    iltTargets.insert(0x140005600);

    int result = pickLiveSymbolCandidate(candidates, iltTargets);
    EXPECT_EQ(result, 2);
}

TEST_F(IltDisambiguationTest, DebuggerBugScenario) {
    // Scenario from real bug: stale "inner" record redirects breakpoint
    // to mid-instruction, causing target corruption
    uint64_t outerFunction = 0x140001000;
    uint64_t staleInner = 0x140001005;  // Zombie, mid-instruction in current build
    uint64_t currentInner = 0x140001010; // Actual nested function location

    candidates.push_back(staleInner);
    candidates.push_back(currentInner);

    // Only current inner function is in ILT
    iltTargets.insert(currentInner);
    iltTargets.insert(outerFunction);

    int result = pickLiveSymbolCandidate(candidates, iltTargets);
    EXPECT_EQ(result, 1);  // Correctly picks current inner, not stale
}

// Performance consideration tests

TEST_F(IltDisambiguationTest, FastPathForSingleCandidate) {
    // Single candidate should return immediately without checking ILT
    candidates.push_back(0x140001000);

    // Large ILT set that would be slow to search
    for (uint64_t addr = 0; addr < 0x100000000; addr += 0x1000) {
        iltTargets.insert(addr);
    }

    // Should still be fast (no ILT lookup needed)
    int result = pickLiveSymbolCandidate(candidates, iltTargets);
    EXPECT_EQ(result, 0);
}
