// Symbol cache implementation for GleamDebugger

#include "GleamDebugger.h"
#include "Performance.h"

// Lookup symbol in cache, returns 0 if not found
uint64_t GleamDebugger::getCachedSymbol(const std::string& modSym)
{
    auto it = mSymbolCache.find(modSym);
    if(it != mSymbolCache.end())
        return it->second;  // Cache hit
    return 0;  // Cache miss
}

// Store resolved symbol in cache
void GleamDebugger::cacheSymbol(const std::string& modSym, uint64_t addr)
{
    if(addr != 0)  // Only cache successful resolutions
        mSymbolCache[modSym] = addr;
}
