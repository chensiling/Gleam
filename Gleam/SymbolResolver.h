// Unified symbol resolution interface

#ifndef GLEAM_SYMBOL_RESOLVER_H
#define GLEAM_SYMBOL_RESOLVER_H

#include "Error.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <cstdint>

namespace GleeBug {
    class Process;
}

namespace Gleam {

// Parsed symbol specification: "module!symbol" or "symbol"
struct SymbolSpec {
    std::string module;    // Empty means search all modules
    std::string symbol;
    bool isModuleQualified() const { return !module.empty(); }

    static SymbolSpec parse(const std::string& spec);
    std::string format() const;
};

// A single resolved symbol location
struct ResolvedSymbol {
    uint64_t address;
    std::string module;
    std::string name;
    bool isIltTarget;   // Whether this is a live ILT target (not zombie)
};

// Result of a symbol lookup (may have 0, 1, or multiple candidates)
enum class SymbolStatus {
    Found,        // Exactly one result
    Ambiguous,    // Multiple candidates, user must pick
    NotFound,     // No candidates found
    Error         // Internal error during resolution
};

struct SymbolLookupResult {
    SymbolStatus status;
    std::vector<ResolvedSymbol> candidates;
    std::string errorMessage;

    bool isFound()    const { return status == SymbolStatus::Found; }
    bool isAmbiguous()const { return status == SymbolStatus::Ambiguous; }
    bool isNotFound() const { return status == SymbolStatus::NotFound; }

    // Convenience: address of the single resolved symbol
    uint64_t address() const {
        return (status == SymbolStatus::Found && !candidates.empty())
               ? candidates[0].address : 0;
    }
};

// Symbol resolver: wraps dbghelp + ILT cache + symbol cache
class SymbolResolver {
private:
    GleeBug::Process*  mProcess;

    // Symbol address cache: "module!symbol" or "HEXBASE:symbol" → address
    std::unordered_map<std::string, uint64_t> mSymbolCache;

    // ILT target cache: module base → set of live target addresses
    std::unordered_map<uint64_t, std::unordered_set<uint64_t>> mIltCache;

    // Statistics
    mutable uint32_t mCacheHits;
    mutable uint32_t mCacheMisses;
    mutable uint32_t mIltCacheHits;

public:
    explicit SymbolResolver(GleeBug::Process* process);

    // -------------------------------------------------------
    // Primary API
    // -------------------------------------------------------

    // Resolve "module!symbol" or "symbol" to an address.
    // Uses cache, ILT disambiguation, and dbghelp under the hood.
    SymbolLookupResult resolve(const std::string& spec);

    // Resolve a raw PDB symbol name within a known module base.
    SymbolLookupResult resolveByBase(uint64_t moduleBase, const std::string& symbolName);

    // Reverse: address → "module!symbol"
    Result<std::string> addressToSymbol(uint64_t address);

    // -------------------------------------------------------
    // Cache management
    // -------------------------------------------------------

    void invalidateSymbolCache();
    void invalidateIltCache(uint64_t moduleBase);  // On module unload
    void invalidateAllCaches();

    // Pre-warm ILT cache for a module (call on module load)
    void warmIltCache(uint64_t moduleBase);

    // -------------------------------------------------------
    // ILT helpers (public for testability)
    // -------------------------------------------------------

    // Returns the set of live ILT targets for a module (cached).
    const std::unordered_set<uint64_t>* getIltTargets(uint64_t moduleBase);

    // Pick the single live candidate from a list, using ILT targets.
    // Returns index into candidates, or -1 if ambiguous / none found.
    static int pickLiveCandidate(const std::vector<uint64_t>& candidates,
                                 const std::unordered_set<uint64_t>& iltTargets);

    // -------------------------------------------------------
    // Statistics
    // -------------------------------------------------------

    struct Stats {
        uint32_t cacheHits;
        uint32_t cacheMisses;
        uint32_t iltCacheHits;
        double hitRatio() const {
            uint32_t total = cacheHits + cacheMisses;
            return total ? static_cast<double>(cacheHits) / total : 0.0;
        }
    };
    Stats getStats() const;
    void resetStats();

    GleeBug::Process* process() const { return mProcess; }

private:
    // Internal helpers
    SymbolLookupResult resolveModuleSym(const std::string& module,
                                        const std::string& symbol);
    SymbolLookupResult resolveAnySym(const std::string& symbol);

    std::string makeCacheKey(const std::string& module, const std::string& symbol) const;
    std::string makePdbCacheKey(uint64_t base, const std::string& symbol) const;

    uint64_t getCached(const std::string& key) const;
    void putCache(const std::string& key, uint64_t address);
};

} // namespace Gleam

#endif // GLEAM_SYMBOL_RESOLVER_H
