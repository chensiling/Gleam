// ILT cache implementation for GleamDebugger

#include "GleamDebugger.h"
#include <unordered_set>

// Forward declaration of iltThunkTargets from GleamCommands.Symbols.cpp
std::unordered_set<uint64_t> iltThunkTargets(GleeBug::Process* process, uint64_t base);

// Get ILT targets for a module, using cache if available
const std::unordered_set<uint64_t>* GleamDebugger::getIltTargets(uint64_t moduleBase)
{
    if(!mProcess)
        return nullptr;

    // Check cache first
    auto it = mIltCache.find(moduleBase);
    if(it != mIltCache.end())
        return &it->second;

    // Not in cache - compute and store
    auto targets = iltThunkTargets(mProcess, moduleBase);
    auto inserted = mIltCache.emplace(moduleBase, std::move(targets));
    return &inserted.first->second;
}
