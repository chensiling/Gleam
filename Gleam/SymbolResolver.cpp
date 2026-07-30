#include "SymbolResolver.h"
#include "Exception.h"
#include <algorithm>
#include <cctype>
#include <cstdio>

#ifdef _WIN32
#include <Windows.h>
#include <DbgHelp.h>
#include <psapi.h>
#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "psapi.lib")
#endif

namespace Gleam {

// ============================================================================
// SymbolSpec parsing
// ============================================================================

SymbolSpec SymbolSpec::parse(const std::string& spec) {
    SymbolSpec result;

    size_t bangPos = spec.find('!');
    if (bangPos != std::string::npos) {
        result.module = spec.substr(0, bangPos);
        result.symbol = spec.substr(bangPos + 1);
    } else {
        result.symbol = spec;
    }

    return result;
}

std::string SymbolSpec::format() const {
    if (module.empty()) {
        return symbol;
    }
    return module + "!" + symbol;
}

// ============================================================================
// SymbolResolver implementation
// ============================================================================

SymbolResolver::SymbolResolver(GleeBug::Process* process)
    : mProcess(process)
    , mCacheHits(0)
    , mCacheMisses(0)
    , mIltCacheHits(0)
{
}

SymbolLookupResult SymbolResolver::resolve(const std::string& spec) {
    SymbolSpec parsed = SymbolSpec::parse(spec);

    // Check cache first
    std::string cacheKey = makeCacheKey(parsed.module, parsed.symbol);
    uint64_t cached = getCached(cacheKey);
    if (cached != 0) {
        mCacheHits++;
        SymbolLookupResult result;
        result.status = SymbolStatus::Found;
        result.candidates.push_back({cached, parsed.module, parsed.symbol, false});
        return result;
    }

    mCacheMisses++;

    // Perform actual resolution
    if (parsed.isModuleQualified()) {
        return resolveModuleSym(parsed.module, parsed.symbol);
    } else {
        return resolveAnySym(parsed.symbol);
    }
}

SymbolLookupResult SymbolResolver::resolveByBase(uint64_t moduleBase,
                                                  const std::string& symbolName) {
    // Check cache with base-keyed format
    std::string cacheKey = makePdbCacheKey(moduleBase, symbolName);
    uint64_t cached = getCached(cacheKey);
    if (cached != 0) {
        mCacheHits++;
        SymbolLookupResult result;
        result.status = SymbolStatus::Found;
        result.candidates.push_back({cached, "", symbolName, false});
        return result;
    }

    mCacheMisses++;

    // This is a simplified implementation - actual resolution would call
    // into dbghelp SymEnumSymbols with the module base
    SymbolLookupResult result;
    result.status = SymbolStatus::NotFound;
    result.errorMessage = "PDB symbol resolution not implemented in this build";
    return result;
}

Result<std::string> SymbolResolver::addressToSymbol(uint64_t address) {
    if (address == 0) {
        return Error(ErrorCategory::Symbol, "Invalid address: 0");
    }

    // This would use SymFromAddr in the full implementation
    char buf[64];
    snprintf(buf, sizeof(buf), "0x%llX", (unsigned long long)address);
    return std::string(buf);
}

void SymbolResolver::invalidateSymbolCache() {
    mSymbolCache.clear();
}

void SymbolResolver::invalidateIltCache(uint64_t moduleBase) {
    mIltCache.erase(moduleBase);
}

void SymbolResolver::invalidateAllCaches() {
    mSymbolCache.clear();
    mIltCache.clear();
}

void SymbolResolver::warmIltCache(uint64_t moduleBase) {
    // Force cache population
    getIltTargets(moduleBase);
}

const std::unordered_set<uint64_t>* SymbolResolver::getIltTargets(uint64_t moduleBase) {
    auto it = mIltCache.find(moduleBase);
    if (it != mIltCache.end()) {
        mIltCacheHits++;
        return &it->second;
    }

    // Not in cache - would scan module for E9 thunks in full implementation
    // For now, return empty set
    auto inserted = mIltCache.emplace(moduleBase, std::unordered_set<uint64_t>());
    return &inserted.first->second;
}

int SymbolResolver::pickLiveCandidate(const std::vector<uint64_t>& candidates,
                                      const std::unordered_set<uint64_t>& iltTargets) {
    if (candidates.empty()) {
        return -1;
    }

    if (candidates.size() == 1) {
        return 0;  // Only one candidate - no disambiguation needed
    }

    // Find candidates that are ILT thunk targets (live code)
    std::vector<int> liveIndices;
    for (size_t i = 0; i < candidates.size(); i++) {
        if (iltTargets.count(candidates[i]) > 0) {
            liveIndices.push_back(static_cast<int>(i));
        }
    }

    // Disambiguation succeeds only if exactly one candidate is an ILT target
    if (liveIndices.size() == 1) {
        return liveIndices[0];
    }

    // Failed: either no ILT targets (all zombies?) or multiple targets (ambiguous)
    return -1;
}

SymbolResolver::Stats SymbolResolver::getStats() const {
    return {mCacheHits, mCacheMisses, mIltCacheHits};
}

void SymbolResolver::resetStats() {
    mCacheHits = 0;
    mCacheMisses = 0;
    mIltCacheHits = 0;
}

// ============================================================================
// Internal helpers
// ============================================================================

SymbolLookupResult SymbolResolver::resolveModuleSym(const std::string& module,
                                                     const std::string& symbol) {
    // Simplified stub - full implementation would:
    // 1. Find module base by name
    // 2. Call SymEnumSymbols
    // 3. Disambiguate with ILT cache
    // 4. Cache result

    SymbolLookupResult result;
    result.status = SymbolStatus::NotFound;
    result.errorMessage = "Module-qualified symbol resolution requires GleeBug integration";
    return result;
}

SymbolLookupResult SymbolResolver::resolveAnySym(const std::string& symbol) {
    // Simplified stub - full implementation would search all loaded modules

    SymbolLookupResult result;
    result.status = SymbolStatus::NotFound;
    result.errorMessage = "Unqualified symbol resolution requires GleeBug integration";
    return result;
}

std::string SymbolResolver::makeCacheKey(const std::string& module,
                                         const std::string& symbol) const {
    if (module.empty()) {
        return symbol;
    }
    return module + "!" + symbol;
}

std::string SymbolResolver::makePdbCacheKey(uint64_t base,
                                            const std::string& symbol) const {
    char buf[128];
    snprintf(buf, sizeof(buf), "%llX:%s", (unsigned long long)base, symbol.c_str());
    return std::string(buf);
}

uint64_t SymbolResolver::getCached(const std::string& key) const {
    auto it = mSymbolCache.find(key);
    if (it != mSymbolCache.end()) {
        return it->second;
    }
    return 0;
}

void SymbolResolver::putCache(const std::string& key, uint64_t address) {
    if (address != 0) {
        mSymbolCache[key] = address;
    }
}

} // namespace Gleam
