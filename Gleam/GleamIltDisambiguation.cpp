// ILT-based symbol disambiguation
//
// Given multiple candidate addresses for a symbol (common under incremental
// linking where the PDB retains stale "zombie" records), determine which
// candidate is the live function body by checking the module's Incremental
// Link Table (ILT).
//
// Returns the index of the unique live candidate, or -1 if disambiguation fails
// (no candidates are ILT targets, or multiple candidates are ILT targets).

#include "GleamDebugger.h"
#include <vector>
#include <unordered_set>
#include <algorithm>

// Pick the live symbol candidate from multiple addresses using ILT disambiguation.
// Returns index into candidates vector, or -1 if disambiguation fails.
int pickLiveSymbolCandidate(const std::vector<uint64_t>& candidates,
                            const std::unordered_set<uint64_t>& iltTargets)
{
    if (candidates.empty())
        return -1;

    if (candidates.size() == 1)
        return 0;  // Only one candidate - no disambiguation needed

    // Find candidates that are ILT thunk targets (live code)
    std::vector<int> liveIndices;
    for (size_t i = 0; i < candidates.size(); i++)
    {
        if (iltTargets.count(candidates[i]) > 0)
            liveIndices.push_back((int)i);
    }

    // Disambiguation succeeds only if exactly one candidate is an ILT target
    if (liveIndices.size() == 1)
        return liveIndices[0];

    // Failed: either no ILT targets (all zombies?) or multiple targets (ambiguous)
    return -1;
}
