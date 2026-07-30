/**
 * @file SymbolResolver.h
 * @brief Unified symbol resolution interface with caching and ILT disambiguation
 *
 * Provides a high-level API for resolving symbol names to addresses and vice versa.
 * Features:
 * - Module-qualified symbols (kernel32!CreateFileW) and unqualified (CreateFileW)
 * - Two-tier caching: symbol cache and ILT (Incremental Link Table) cache
 * - ILT-based disambiguation for incremental linking zombie symbols
 * - Performance statistics tracking
 *
 * @see GleamCommands.Symbols.cpp for integration with dbghelp
 */

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

/**
 * @brief Parsed symbol specification
 *
 * Represents a symbol in "module!symbol" or "symbol" format.
 * The module part is optional; when absent, all loaded modules are searched.
 *
 * @example
 * @code
 * auto spec1 = SymbolSpec::parse("kernel32!CreateFileW");
 * // spec1.module = "kernel32", spec1.symbol = "CreateFileW"
 *
 * auto spec2 = SymbolSpec::parse("CreateFileW");
 * // spec2.module = "", spec2.symbol = "CreateFileW"
 * @endcode
 */
struct SymbolSpec {
    std::string module;    ///< Module name (empty = search all modules)
    std::string symbol;    ///< Symbol name

    /**
     * @brief Check if this symbol has a module qualifier
     * @return true if module is specified
     */
    bool isModuleQualified() const { return !module.empty(); }

    /**
     * @brief Parse a symbol specification string
     * @param spec String in format "module!symbol" or "symbol"
     * @return Parsed SymbolSpec
     */
    static SymbolSpec parse(const std::string& spec);

    /**
     * @brief Format back to string
     * @return "module!symbol" if qualified, otherwise "symbol"
     */
    std::string format() const;
};

/**
 * @brief A single resolved symbol location
 *
 * Represents one candidate address for a symbol. In cases of incremental
 * linking, a single symbol name may resolve to multiple addresses (zombie
 * symbols). The isIltTarget flag indicates whether this is the live version.
 */
struct ResolvedSymbol {
    uint64_t address;       ///< Resolved address
    std::string module;     ///< Module name where found
    std::string name;       ///< Symbol name
    bool isIltTarget;       ///< true if this is a live ILT target (not zombie)
};

/**
 * @brief Status of a symbol lookup operation
 */
enum class SymbolStatus {
    Found,        ///< Exactly one result found
    Ambiguous,    ///< Multiple candidates, user must disambiguate
    NotFound,     ///< No candidates found
    Error         ///< Internal error during resolution
};

/**
 * @brief Result of a symbol lookup operation
 *
 * Contains the resolution status and candidate addresses. For Found status,
 * candidates contains exactly one entry. For Ambiguous, it contains multiple
 * entries. For NotFound/Error, candidates is empty.
 *
 * @example Handling resolution results:
 * @code
 * auto result = resolver.resolve("kernel32!CreateFileW");
 * if (result.isFound()) {
 *     printf("Resolved to: 0x%llX\n", result.address());
 * } else if (result.isAmbiguous()) {
 *     printf("Ambiguous: %zu candidates\n", result.candidates.size());
 *     for (const auto& sym : result.candidates) {
 *         printf("  0x%llX in %s%s\n", sym.address, sym.module.c_str(),
 *                sym.isIltTarget ? " (LIVE)" : "");
 *     }
 * } else {
 *     printf("Not found: %s\n", result.errorMessage.c_str());
 * }
 * @endcode
 */
struct SymbolLookupResult {
    SymbolStatus status;                      ///< Resolution status
    std::vector<ResolvedSymbol> candidates;   ///< Resolved addresses
    std::string errorMessage;                 ///< Error details (if status=Error/NotFound)

    /** @brief Check if exactly one symbol was found */
    bool isFound()    const { return status == SymbolStatus::Found; }

    /** @brief Check if multiple candidates were found (disambiguation needed) */
    bool isAmbiguous()const { return status == SymbolStatus::Ambiguous; }

    /** @brief Check if no symbol was found */
    bool isNotFound() const { return status == SymbolStatus::NotFound; }

    /**
     * @brief Get the resolved address (convenience for Found status)
     * @return Address of the first candidate, or 0 if not Found
     */
    uint64_t address() const {
        return (status == SymbolStatus::Found && !candidates.empty())
               ? candidates[0].address : 0;
    }
};

/**
 * @brief Symbol resolver with caching and ILT disambiguation
 *
 * SymbolResolver provides a high-level interface for symbol resolution
 * with the following features:
 *
 * **Two-tier caching:**
 * - Symbol cache: Maps "module!symbol" -> address for fast repeated lookups
 * - ILT cache: Maps module base -> set of live ILT thunk targets
 *
 * **ILT disambiguation:**
 * When incremental linking is enabled, dbghelp may return multiple addresses
 * for a single symbol (the live version and zombie copies). SymbolResolver
 * uses ILT (Incremental Link Table) analysis to identify the live version:
 * only the live function body is referenced by ILT thunks.
 *
 * **Performance tracking:**
 * Tracks cache hit/miss statistics via getStats().
 *
 * @example Basic usage:
 * @code
 * GleeBug::Process* proc = debugger->GetProcessById(pid);
 * SymbolResolver resolver(proc);
 *
 * // Resolve a symbol
 * auto result = resolver.resolve("kernel32!CreateFileW");
 * if (result.isFound()) {
 *     printf("Address: 0x%llX\n", result.address());
 * }
 *
 * // Reverse lookup
 * auto nameResult = resolver.addressToSymbol(0x7FFF12345678);
 * if (nameResult.isOk()) {
 *     printf("Symbol: %s\n", nameResult.value().c_str());
 * }
 * @endcode
 *
 * @example Cache warming on module load:
 * @code
 * void onModuleLoad(uint64_t moduleBase) {
 *     resolver.warmIltCache(moduleBase);  // Pre-scan ILT for fast lookups
 * }
 * @endcode
 */
class SymbolResolver {
private:
    GleeBug::Process*  mProcess;   ///< Target process for memory reads

    /// Symbol address cache: "module!symbol" or "HEXBASE:symbol" -> address
    std::unordered_map<std::string, uint64_t> mSymbolCache;

    /// ILT target cache: module base -> set of live target addresses
    std::unordered_map<uint64_t, std::unordered_set<uint64_t>> mIltCache;

    // Statistics (mutable for const methods)
    mutable uint32_t mCacheHits;      ///< Symbol cache hits
    mutable uint32_t mCacheMisses;    ///< Symbol cache misses
    mutable uint32_t mIltCacheHits;   ///< ILT cache hits

public:
    /**
     * @brief Construct a SymbolResolver for a process
     * @param process Target process (must remain valid for lifetime of resolver)
     */
    explicit SymbolResolver(GleeBug::Process* process);

    // -------------------------------------------------------
    // Primary API
    // -------------------------------------------------------

    /**
     * @brief Resolve a symbol specification to an address
     *
     * Accepts "module!symbol" or "symbol" format. Uses symbol cache,
     * ILT disambiguation, and dbghelp lookups under the hood.
     *
     * @param spec Symbol specification (e.g., "kernel32!CreateFileW" or "CreateFileW")
     * @return SymbolLookupResult with status and candidates
     *
     * @note Increments cache hit/miss statistics
     */
    SymbolLookupResult resolve(const std::string& spec);

    /**
     * @brief Resolve a raw PDB symbol name within a known module
     *
     * For PDB-only symbols (not in exports), when you already know
     * the module base. Bypasses module name lookup.
     *
     * @param moduleBase Base address of the loaded module
     * @param symbolName Symbol name to resolve
     * @return SymbolLookupResult with status and candidates
     */
    SymbolLookupResult resolveByBase(uint64_t moduleBase, const std::string& symbolName);

    /**
     * @brief Reverse lookup: address -> "module!symbol"
     *
     * Uses dbghelp SymFromAddr to find the symbol name for an address.
     *
     * @param address Address to look up
     * @return Result<string> containing "module!symbol" or error
     */
    Result<std::string> addressToSymbol(uint64_t address);

    // -------------------------------------------------------
    // Cache management
    // -------------------------------------------------------

    /**
     * @brief Invalidate the entire symbol cache
     *
     * Call this when symbols may have changed (e.g., after a restart).
     */
    void invalidateSymbolCache();

    /**
     * @brief Invalidate ILT cache for a specific module
     *
     * Call this when a module is unloaded to free memory.
     *
     * @param moduleBase Base address of the unloaded module
     */
    void invalidateIltCache(uint64_t moduleBase);

    /**
     * @brief Invalidate all caches (symbol + ILT)
     *
     * Call this on restart or detach to clear all cached state.
     */
    void invalidateAllCaches();

    /**
     * @brief Pre-populate ILT cache for a module
     *
     * Scans the module's ILT immediately to avoid latency on first lookup.
     * Recommended to call on module load events.
     *
     * @param moduleBase Base address of the newly loaded module
     */
    void warmIltCache(uint64_t moduleBase);

    // -------------------------------------------------------
    // ILT helpers (public for testability)
    // -------------------------------------------------------

    /**
     * @brief Get the set of live ILT targets for a module
     *
     * Returns a cached set of addresses that are ILT thunk targets,
     * indicating they are live function bodies (not zombies).
     *
     * @param moduleBase Base address of the module
     * @return Pointer to the ILT target set (owned by cache), or nullptr on failure
     *
     * @note Increments ILT cache hit counter on subsequent calls
     */
    const std::unordered_set<uint64_t>* getIltTargets(uint64_t moduleBase);

    /**
     * @brief Pick the single live candidate using ILT analysis
     *
     * Given multiple candidate addresses for a symbol, determines which one
     * is the live version by checking ILT thunk targets.
     *
     * **Disambiguation rules:**
     * - If exactly 1 candidate: always return it (index 0)
     * - If exactly 1 candidate is an ILT target: return that one
     * - If 0 or >1 candidates are ILT targets: return -1 (ambiguous)
     *
     * @param candidates List of candidate addresses
     * @param iltTargets Set of addresses that are live ILT targets
     * @return Index into candidates array, or -1 if ambiguous/none found
     *
     * @note This is a static method for easy unit testing
     *
     * @example
     * @code
     * std::vector<uint64_t> candidates = {0x1000, 0x2000, 0x3000};
     * auto* iltTargets = resolver.getIltTargets(moduleBase);
     * int index = SymbolResolver::pickLiveCandidate(candidates, *iltTargets);
     * if (index >= 0) {
     *     uint64_t liveAddr = candidates[index];
     * }
     * @endcode
     */
    static int pickLiveCandidate(const std::vector<uint64_t>& candidates,
                                 const std::unordered_set<uint64_t>& iltTargets);

    // -------------------------------------------------------
    // Statistics
    // -------------------------------------------------------

    /**
     * @brief Performance statistics
     */
    struct Stats {
        uint32_t cacheHits;      ///< Symbol cache hits
        uint32_t cacheMisses;    ///< Symbol cache misses
        uint32_t iltCacheHits;   ///< ILT cache hits

        /**
         * @brief Calculate cache hit ratio
         * @return Hit ratio as a fraction (0.0 to 1.0)
         */
        double hitRatio() const {
            uint32_t total = cacheHits + cacheMisses;
            return total ? static_cast<double>(cacheHits) / total : 0.0;
        }
    };

    /**
     * @brief Get current performance statistics
     * @return Stats structure with cache hit/miss counts
     */
    Stats getStats() const;

    /**
     * @brief Reset all statistics to zero
     */
    void resetStats();

    /**
     * @brief Get the associated process
     * @return Pointer to the GleeBug::Process
     */
    GleeBug::Process* process() const { return mProcess; }

private:
    // Internal resolution helpers
    SymbolLookupResult resolveModuleSym(const std::string& module,
                                        const std::string& symbol);
    SymbolLookupResult resolveAnySym(const std::string& symbol);

    // Cache key generation
    std::string makeCacheKey(const std::string& module, const std::string& symbol) const;
    std::string makePdbCacheKey(uint64_t base, const std::string& symbol) const;

    // Cache access
    uint64_t getCached(const std::string& key) const;
    void putCache(const std::string& key, uint64_t address);
};

} // namespace Gleam

#endif // GLEAM_SYMBOL_RESOLVER_H
