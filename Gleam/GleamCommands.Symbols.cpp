/// @file GleamCommands.Symbols.cpp
/// @brief Symbol resolution, import/export table inspection, and x64 stack
///        frame unwinding.
///
/// @details
/// The file covers several logical areas:
///
/// @section sym_pe  PE header helpers
///   readPeDirectories() reads the optional header and data-directory table
///   from a live debuggee process.  rangeInImage() provides overflow-safe
///   bounds checks against the loader-reported image size (never the PE
///   header value, which the target can falsify).
///
/// @section sym_ilt  ILT thunk-target collection
///   iltThunkTargets() walks all executable sections looking for E9 rel32
///   thunks.  Under incremental linking every call goes through such a thunk,
///   so the set of thunk targets is exactly the set of live function bodies.
///   getIltTargets() (in GleamDebugger.cpp) adds a per-module cache on top.
///
/// @section sym_resolve  Symbol resolution with zombie-record elimination
///   resolveModuleSymbol() enumerates ALL PDB records for a name, then uses
///   pickLiveSymbolCandidate() to discard stale "zombie" records left by
///   incremental linking.  resolvePdbSymbol() does the same via an explicit
///   SymLoadModuleExW for modules not yet in the loader list (DLL load event).
///   Both populate mSymbolCache on success.
///
/// @section sym_modid  Module identity verification
///   verifyModuleIdentity() compares the CodeView GUID+Age+SizeOfImage of a
///   loaded module against its disk image.  Prevents symbol spoofing: loading
///   a symbol file proves nothing; a matching debug record does.
///
/// @section sym_frames  x64 stack frame unwinding
///   cmdFrames() uses RtlVirtualUnwind (the same unwinder dbghelp wraps)
///   run locally against a mirror of the remote stack + code/unwind data.
///   findRuntimeFunction() binary-searches a cached copy of the remote .pdata
///   and resolves indirect table entries (common in system DLLs).
///
/// @section sym_failapi  Fault-injection hooks
///   failapi* functions implement the "selftest failapi" test surface: each
///   replaces the matching mTestHook* slot, fires once (or persistently for
///   the "always" variant), and then disarms itself.

#include "GleamDebugger.h"
#include "RaiiUtils.h"
#include "Performance.h"
#include "PerfMonitor.h"
#include "Constants.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <string>
#include <unordered_set>
#include <vector>
#include <psapi.h>
#include <dbghelp.h>
#include <delayimp.h>

using namespace GleeBug;

#ifndef DBG_REPLY_LATER
#define DBG_REPLY_LATER ((NTSTATUS)0x40010001L)
#endif // DBG_REPLY_LATER

// Forward declaration for ILT cache (implemented below in anonymous namespace)
std::unordered_set<uint64_t> iltThunkTargets(GleeBug::Process* process, uint64_t base);

namespace
{
    // Fault-injection hooks for "selftest failapi" (engine side:
    // GleeBug::Debugger::mTestHook*). An armed hook makes the matching Win32
    // API fail ONCE with ERROR_ACCESS_DENIED (so the injected failure is
    // recognizable in the error output) and then disarms itself: a failure
    // must never leak into the rest of the session, and a following resume
    // retry must be able to succeed. Session init also clears all hooks, so
    // an armed-but-never-fired hook cannot pollute a restarted session.
    BOOL failapiWait(LPDEBUG_EVENT, DWORD)
    {
        GleeBug::Debugger::mTestHookWaitForDebugEvent = nullptr;
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }

    // Fail only the next "normal" continue; DBG_REPLY_LATER passes through.
    BOOL failapiContinueNormal(DWORD dwProcessId, DWORD dwThreadId, DWORD dwContinueStatus)
    {
        if(dwContinueStatus == (DWORD)DBG_REPLY_LATER)
            return ContinueDebugEvent(dwProcessId, dwThreadId, dwContinueStatus);
        GleeBug::Debugger::mTestHookContinueDebugEvent = nullptr;
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }

    // Fail only the next DBG_REPLY_LATER continue; normal ones pass through.
    BOOL failapiContinueReplyLater(DWORD dwProcessId, DWORD dwThreadId, DWORD dwContinueStatus)
    {
        if(dwContinueStatus != (DWORD)DBG_REPLY_LATER)
            return ContinueDebugEvent(dwProcessId, dwThreadId, dwContinueStatus);
        GleeBug::Debugger::mTestHookContinueDebugEvent = nullptr;
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }

    DWORD failapiResume(HANDLE)
    {
        GleeBug::Debugger::mTestHookResumeThread = nullptr;
        SetLastError(ERROR_ACCESS_DENIED);
        return (DWORD)-1;
    }

    // Persistent variant ("selftest failapi resume always"): fails EVERY
    // resume until "selftest failapi off" disarms it. Used to prove the
    // detach-refusal path for permanently failing restores.
    DWORD failapiResumeAlways(HANDLE)
    {
        SetLastError(ERROR_ACCESS_DENIED);
        return (DWORD)-1;
    }
}

namespace
{
    // Decide which record for a symbol is the CURRENT function body. Under
    // incremental linking the PDB can keep a stale (zombie) record: the old
    // body is still mapped and still carries the name, but no ILT thunk
    // references it anymore. A single record is all the information there
    // is (accepted); with several, only a body an ILT thunk still points at
    // can be used - and only when EXACTLY ONE such body exists. Returns the
    // candidate index, or -1 when no reliable identification is possible
    // (the caller must refuse rather than write a breakpoint to a guess).
    int pickLiveSymbolCandidate(const std::vector<uint64_t> & candidates,
                                const std::unordered_set<uint64_t> & iltTargets)
    {
        if(candidates.size() == 1)
            return 0;
        int found = -1, live = 0;
        for(size_t i = 0; i < candidates.size(); i++)
        {
            if(iltTargets.count(candidates[i]) != 0)
            {
                found = (int)i;
                live++;
            }
        }
        return live == 1 ? found : -1;
    }
} // anonymous namespace

// Collect the ILT thunk targets of a module loaded in the debuggee.
// With incremental linking every call goes through an "E9 rel32" thunk,
// so the thunk targets are exactly the function bodies of the linker's
// current layout.
// NOTE: This function is now wrapped by GleamDebugger::getIltTargets() which
// provides caching. Direct calls should be avoided in hot paths.
std::unordered_set<uint64_t> iltThunkTargets(GleeBug::Process* process, uint64_t base)
{
    std::unordered_set<uint64_t> targets;
    uint8_t hdr[0x1000];
        if(!process->MemReadSafe(base, hdr, sizeof(hdr)))
            return targets;
        auto dos = (const IMAGE_DOS_HEADER*)hdr;
        if(dos->e_magic != IMAGE_DOS_SIGNATURE ||
           (uint64_t)dos->e_lfanew + sizeof(IMAGE_NT_HEADERS64) > sizeof(hdr))
            return targets;
        auto nt = (const IMAGE_NT_HEADERS64*)(hdr + dos->e_lfanew);
        if(nt->Signature != IMAGE_NT_SIGNATURE)
            return targets;
        auto sec = (const IMAGE_SECTION_HEADER*)((const uint8_t*)&nt->OptionalHeader +
                                                 nt->FileHeader.SizeOfOptionalHeader);
        if((const uint8_t*)(sec + nt->FileHeader.NumberOfSections) > hdr + sizeof(hdr))
            return targets;
        const uint64_t imageEnd = base + nt->OptionalHeader.SizeOfImage;
        std::vector<uint8_t> buf;
        for(int i = 0; i < nt->FileHeader.NumberOfSections; i++)
        {
            if(!(sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE))
                continue;
            const uint32_t size = sec[i].Misc.VirtualSize;
            if(!size || size > 32 * 1024 * 1024)
                continue;
            buf.resize(size);
            if(!process->MemReadSafe(base + sec[i].VirtualAddress, buf.data(), size))
                continue;
            const uint64_t secBase = base + sec[i].VirtualAddress;
            for(uint32_t off = 0; off + 5 <= size; off++)
            {
                if(buf[off] != 0xE9)
                    continue;
                int32_t rel;
                memcpy(&rel, buf.data() + off + 1, sizeof(rel));
                const uint64_t target = secBase + off + 5 + (int64_t)rel;
                if(target >= base && target < imageEnd)
                    targets.insert(target);
            }
        }
        return targets;
    }

namespace
{
    struct ModuleInfo
    {
        uint64_t base = 0;
        uint32_t size = 0;
        std::string name;
    };

    // Find a loaded module by base address (hex string) or case-insensitive
    // name (with or without the .dll suffix).
    bool findModule(HANDLE hProcess, const std::string & nameOrBase, ModuleInfo & out)
    {
        uint64_t wantedBase = 0;
        bool byBase = parseHex(nameOrBase, wantedBase);

        HMODULE modules[1024];
        DWORD needed = 0;
        if(!EnumProcessModules(hProcess, modules, sizeof(modules), &needed))
            return false;
        DWORD count = (DWORD)(std::min)(needed / sizeof(HMODULE), sizeof(modules) / sizeof(HMODULE));
        for(DWORD i = 0; i < count; i++)
        {
            MODULEINFO mi;
            if(!GetModuleInformation(hProcess, modules[i], &mi, sizeof(mi)))
                continue;
            if(byBase)
            {
                if((uint64_t)(uintptr_t)mi.lpBaseOfDll == wantedBase)
                {
                    out.base = wantedBase;
                    out.size = mi.SizeOfImage;
                    return true;
                }
                continue;
            }
            char name[MAX_PATH] = "";
            GetModuleBaseNameA(hProcess, modules[i], name, sizeof(name));
            if(normalizeModuleName(name) == normalizeModuleName(nameOrBase))
            {
                out.base = (uint64_t)(uintptr_t)mi.lpBaseOfDll;
                out.size = mi.SizeOfImage;
                out.name = name;
                return true;
            }
        }
        return false;
    }

    // Read a NUL-terminated string from the debuggee. `cap` limits the total
    // number of bytes read; every chunk read is checked so we never go past it.
    // Returns (text, true) only when a NUL was found within the range.
    std::pair<std::string, bool> readCString(Process* process, uint64_t addr, size_t cap = 260)
    {
        std::string result;
        char buf[64];
        while(result.size() < cap)
        {
            size_t chunk = (std::min)(sizeof(buf), cap - result.size());
            if(chunk == 0 || !process->MemReadSafe(addr + result.size(), buf, chunk))
                return { result, false }; // truncated or unreadable
            size_t i = 0;
            for(; i < chunk && buf[i]; i++)
                result += buf[i];
            if(i < chunk) // hit the NUL
                return { result, true };
        }
        return { result, false }; // no NUL within the cap
    }

    template<typename T>
    bool readAt(Process* process, uint64_t addr, T & out)
    {
        return process->MemReadSafe(addr, &out, sizeof(out));
    }

    // Optional header for both PE32 and PE32+ (only the data directories are used).
    struct PeInfo
    {
        bool valid = false;
        bool pe64 = true;
        uint32_t entryPointRva = 0;
        uint32_t sizeOfImage = 0;
        IMAGE_DATA_DIRECTORY importDir{};
        IMAGE_DATA_DIRECTORY delayImportDir{};
        IMAGE_DATA_DIRECTORY exceptionDir{};
        IMAGE_DATA_DIRECTORY exportDir{};
        IMAGE_DATA_DIRECTORY debugDir{};
        // Added for "moduleinfo": TLS analysis, CFG inspection and the
        // header facts needed to tell a relocated image from a fixed one.
        IMAGE_DATA_DIRECTORY tlsDir{};
        IMAGE_DATA_DIRECTORY loadConfigDir{};
        IMAGE_DATA_DIRECTORY relocDir{};
        uint16_t machine = 0;
        uint16_t characteristics = 0;
        uint16_t subsystem = 0;
        uint16_t dllCharacteristics = 0;
        uint16_t sectionCount = 0;
        uint32_t timeDateStamp = 0;
        uint32_t sizeOfHeaders = 0;
        uint32_t sectionAlignment = 0;
        uint64_t preferredBase = 0;
        // Where the section table starts, so callers do not re-derive it.
        uint64_t sectionTableVa = 0;
    };

    PeInfo readPeDirectories(Process* process, uint64_t base)
    {
        PeInfo info;
        IMAGE_DOS_HEADER dos;
        if(!readAt(process, base, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE)
            return info;
        uint32_t signature = 0;
        if(!readAt(process, base + dos.e_lfanew, signature) || signature != IMAGE_NT_SIGNATURE)
            return info;
        IMAGE_FILE_HEADER fileHeader;
        if(!readAt(process, base + dos.e_lfanew + sizeof(signature), fileHeader))
            return info;
        uint16_t magic = 0;
        uint64_t optAddr = base + dos.e_lfanew + sizeof(signature) + sizeof(fileHeader);
        if(!readAt(process, optAddr, magic))
            return info;
        info.machine = fileHeader.Machine;
        info.characteristics = fileHeader.Characteristics;
        info.sectionCount = fileHeader.NumberOfSections;
        info.timeDateStamp = fileHeader.TimeDateStamp;
        // The section table follows the optional header, whose real length is
        // SizeOfOptionalHeader - never sizeof() of our own struct.
        info.sectionTableVa = optAddr + fileHeader.SizeOfOptionalHeader;
        if(magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        {
            IMAGE_OPTIONAL_HEADER64 opt;
            if(!readAt(process, optAddr, opt))
                return info;
            info.pe64 = true;
            info.entryPointRva = opt.AddressOfEntryPoint;
            info.sizeOfImage = opt.SizeOfImage;
            info.importDir = opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
            info.delayImportDir = opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
            info.exceptionDir = opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
            info.exportDir = opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
            info.debugDir = opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
            info.tlsDir = opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
            info.loadConfigDir = opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG];
            info.relocDir = opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
            info.subsystem = opt.Subsystem;
            info.dllCharacteristics = opt.DllCharacteristics;
            info.sizeOfHeaders = opt.SizeOfHeaders;
            info.sectionAlignment = opt.SectionAlignment;
            info.preferredBase = opt.ImageBase;
        }
        else if(magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC)
        {
            IMAGE_OPTIONAL_HEADER32 opt;
            if(!readAt(process, optAddr, opt))
                return info;
            info.pe64 = false;
            info.entryPointRva = opt.AddressOfEntryPoint;
            info.sizeOfImage = opt.SizeOfImage;
            info.importDir = opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
            info.delayImportDir = opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
            info.exceptionDir = opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
            info.exportDir = opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
            info.debugDir = opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
            info.tlsDir = opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
            info.loadConfigDir = opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG];
            info.relocDir = opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
            info.subsystem = opt.Subsystem;
            info.dllCharacteristics = opt.DllCharacteristics;
            info.sizeOfHeaders = opt.SizeOfHeaders;
            info.sectionAlignment = opt.SectionAlignment;
            info.preferredBase = opt.ImageBase;
        }
        else
            return info;
        info.valid = true;
        return info;
    }

    // Overflow-safe range check: is [rva, rva+size) fully inside the image?
    bool rangeInImage(uint64_t rva, uint64_t size, uint64_t imageSize)
    {
        if(imageSize == 0 || rva > imageSize)
            return false;
        return size <= imageSize - rva;
    }

    // Find the module whose [base, base+size) contains addr.
    bool moduleOf(HANDLE hProcess, uint64_t addr, ModuleInfo & out)
    {
        HMODULE modules[1024];
        DWORD needed = 0;
        if(!EnumProcessModules(hProcess, modules, sizeof(modules), &needed))
            return false;
        DWORD count = (DWORD)(std::min)(needed / sizeof(HMODULE), sizeof(modules) / sizeof(HMODULE));
        for(DWORD i = 0; i < count; i++)
        {
            MODULEINFO mi;
            if(!GetModuleInformation(hProcess, modules[i], &mi, sizeof(mi)))
                continue;
            uint64_t base = (uint64_t)(uintptr_t)mi.lpBaseOfDll;
            if(addr >= base && addr - base < mi.SizeOfImage)
            {
                out.base = base;
                out.size = mi.SizeOfImage;
                char name[MAX_PATH] = "";
                GetModuleBaseNameA(hProcess, modules[i], name, sizeof(name));
                out.name = name;
                return true;
            }
        }
        return false;
    }

    // Case-insensitive wildcard match (* and ?).
    bool wildcardMatch(const char* pattern, const char* text)
    {
        if(!*pattern)
            return !*text;
        if(*pattern == '*')
        {
            while(*text)
            {
                if(wildcardMatch(pattern + 1, text))
                    return true;
                text++;
            }
            return wildcardMatch(pattern + 1, text);
        }
        if(!*text)
            return false;
        if(*pattern != '?' && tolower((unsigned char)*pattern) != tolower((unsigned char)*text))
            return false;
        return wildcardMatch(pattern + 1, text + 1);
    }

    struct EnumCtx
    {
        const std::string* filter;
        size_t shown;
    };

    BOOL CALLBACK enumSymbolCb(PSYMBOL_INFO sym, ULONG, PVOID context)
    {
        auto ctx = (EnumCtx*)context;
        if(ctx->filter && !ctx->filter->empty() && !wildcardMatch(ctx->filter->c_str(), sym->Name))
            return TRUE;
        printf("  0x%016llX  %s\n", (unsigned long long)sym->Address, sym->Name);
        ctx->shown++;
        return TRUE;
    }
}

// ReadMemoryProc for StackWalk64 on the remote target.
static BOOL CALLBACK stackReadMemory(HANDLE hProcess, DWORD64 base, PVOID buffer, DWORD size, LPDWORD bytesRead)
{
    // ReadProcessMemory reports through SIZE_T (8 bytes on x64); converting
    // LPDWORD to SIZE_T* directly would write 8 bytes into a 4-byte out
    // parameter. Receive into a local first, then convert with a bound.
    SIZE_T read = 0;
    BOOL ok = ReadProcessMemory(hProcess, (LPCVOID)base, buffer, size, &read);
    if(bytesRead)
        *bytesRead = (DWORD)(std::min)(read, (SIZE_T)size);
    return ok;
}

// Unwind one frame with dbghelp StackWalk64 (uses .pdata, so it works for
// FPO/optimized x64 code). Distinguishes a real caller from a confirmed
// leaf function (verified remote .pdata has no record for the ORIGINAL PC)
// and from infrastructure failure.
std::pair<GleamDebugger::UnwindStatus, uint64_t> GleamDebugger::stackWalkReturn(HANDLE hThread)
{
    if(!mProcess || !ensureSymSession())
        return { UnwindStatus::Failed, 0 };
    CONTEXT context{};
    context.ContextFlags = CONTEXT_FULL;
    if(!GetThreadContext(hThread, &context))
        return { UnwindStatus::Failed, 0 };
    // StackWalk64 rewrites the context to the CALLER's frame; the leaf check
    // must use the ORIGINAL rip.
    const uint64_t originalRip = context.Rip;
    STACKFRAME64 frame{};
    frame.AddrPC.Offset = context.Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = context.Rbp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = context.Rsp;
    frame.AddrStack.Mode = AddrModeFlat;
    if(StackWalk64(IMAGE_FILE_MACHINE_AMD64,
                   mProcess->hProcess, hThread, &frame, &context,
                   stackReadMemory, SymFunctionTableAccess64, SymGetModuleBase64, nullptr) &&
       frame.AddrReturn.Offset)
        return { UnwindStatus::Success, frame.AddrReturn.Offset };

    // The walk found no caller. Verify the remote .pdata directly: a
    // RUNTIME_FUNCTION entry covering the original PC means the function is
    // NOT a leaf (the unwind itself failed); no such entry means a genuine
    // leaf and [rsp] holds the return address (x64 ABI).
    switch(checkUnwindRecord(originalRip))
    {
    case PdataCheck::HasRecord:
        return { UnwindStatus::Failed, 0 }; // has unwind info, yet unwinding failed
    case PdataCheck::NoRecord:
        return { UnwindStatus::Leaf, 0 };
    default:
        return { UnwindStatus::Failed, 0 }; // cannot verify; set no breakpoint
    }
}

// Verify a remote module's .pdata for a RUNTIME_FUNCTION covering rva.
GleamDebugger::PdataCheck GleamDebugger::checkUnwindRecord(uint64_t rip)
{
    ModuleInfo mod;
    if(!moduleOf(mProcess->hProcess, rip, mod))
        return PdataCheck::Unknown;
    auto pe = readPeDirectories(mProcess, mod.base);
    if(!pe.valid)
        return PdataCheck::Unknown;
    const uint64_t rva = rip - mod.base;
    if(!rangeInImage(pe.exceptionDir.VirtualAddress, pe.exceptionDir.Size, mod.size))
        return PdataCheck::Unknown;
    if(pe.exceptionDir.Size == 0)
        return PdataCheck::NoRecord;
    const uint32_t count = pe.exceptionDir.Size / (uint32_t)sizeof(RUNTIME_FUNCTION);
    for(uint32_t i = 0; i < count; i++)
    {
        RUNTIME_FUNCTION rf;
        if(!mProcess->MemReadSafe(mod.base + pe.exceptionDir.VirtualAddress + (uint64_t)i * sizeof(rf), &rf, sizeof(rf)))
            return PdataCheck::Unknown;
        if(rva >= rf.BeginAddress && rva < rf.EndAddress)
            return PdataCheck::HasRecord;
    }
    return PdataCheck::NoRecord;
}

uint64_t GleamDebugger::moduleEntryPoint(uint64_t base)
{
    if(!mProcess)
        return 0;
    auto pe = readPeDirectories(mProcess, base);
    if(!pe.valid || !pe.entryPointRva)
        return 0;
    return base + pe.entryPointRva;
}

std::string GleamDebugger::symNameByAddr(uint64_t addr)
{
    if(!addr || !ensureSymSession())
        return std::string();
    char buf[sizeof(SYMBOL_INFO) + MAX_SYM_NAME];
    memset(buf, 0, sizeof(buf));
    auto si = (SYMBOL_INFO*)buf;
    si->SizeOfStruct = sizeof(SYMBOL_INFO);
    si->MaxNameLen = MAX_SYM_NAME;
    uint64_t disp = 0;
    if(SymFromAddr(mProcess->hProcess, addr, &disp, si))
        return std::string(si->Name);
    return std::string();
}

bool GleamDebugger::parseAddress(const std::string & s, uint64_t & out, std::string & err)
{
    if(!evalExpression(s, out, err))
    {
        mAddrError = err;
        return false;
    }
    mAddrError.clear();
    return true;
}

bool GleamDebugger::parseAddress(const std::string & s, uint64_t & out)
{
    std::string err;
    return parseAddress(s, out, err);
}

// Print the reason of the last failed parseAddress, if any. Called by the
// central "unknown command" path and by usage printers.
void GleamDebugger::printAddrError()
{
    if(!mAddrError.empty())
    {
        printf("error: %s\n", mAddrError.c_str());
        mAddrError.clear();
        fflush(stdout);
    }
}

bool GleamDebugger::moduleBaseByName(const std::string & name, uint64_t & base)
{
    if(!mProcess)
        return false;
    ModuleInfo mod;
    if(!findModule(mProcess->hProcess, name, mod))
        return false;
    base = mod.base;
    return true;
}

bool GleamDebugger::moduleInfoOf(const std::string & name, uint64_t & base, uint32_t & size)
{
    if(!mProcess)
        return false;
    ModuleInfo mod;
    if(!findModule(mProcess->hProcess, name, mod))
        return false;
    base = mod.base;
    size = mod.size;
    return true;
}

uint32_t GleamDebugger::moduleImageSize(uint64_t base)
{
    if(!mProcess)
        return 0;
    auto pe = readPeDirectories(mProcess, base);
    return pe.valid ? pe.sizeOfImage : 0;
}

GleamDebugger::SymbolResult GleamDebugger::resolveModuleSymbol(const std::string & modSym, uint64_t & out)
{
    PERF_RECORD(Gleam::PerfEvent::SymbolResolve, modSym);

    // Check cache first for successful resolutions
    uint64_t cached = getCachedSymbol(modSym);
    if(cached != 0)
    {
        PERF_CACHE_HIT();
        out = cached;
        return SymbolResult::Found;
    }

    // Cache miss - perform actual resolution
    if(!mProcess || !ensureSymSession())
        return SymbolResult::NotFound;
    // Enumerate ALL records for the name: under incremental linking the PDB
    // can keep a stale (zombie) record whose address no live code uses, and
    // SymFromName's pick between the records is not reliable. A breakpoint
    // written to such an address may never fire - or land mid-instruction
    // in a current function and corrupt the target (observed for real: a
    // stale "inner" record redirected an int3 into "sub rsp,imm").
    std::vector<uint64_t> candidates;
    uint64_t modBase = 0;
    {
        struct Ctx
        {
            std::vector<uint64_t>* addrs;
            uint64_t* modBase;
        } ctx{ &candidates, &modBase };
        auto cb = [](PSYMBOL_INFO si, ULONG, PVOID userCtx) -> BOOL
        {
            auto c = (Ctx*)userCtx;
            c->addrs->push_back(si->Address);
            if(!*c->modBase)
                *c->modBase = si->ModBase;
            return TRUE;
        };
        if(!SymEnumSymbols(mProcess->hProcess, 0, modSym.c_str(), cb, &ctx))
            return SymbolResult::NotFound;
    }
    if(candidates.empty())
        return SymbolResult::NotFound;
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());

    int pick = 0;
    if(candidates.size() > 1)
    {
        // Disambiguate through the module's ILT: only a body a thunk still
        // points at can be the current function.
        auto ilt = iltThunkTargets(mProcess, modBase);
        pick = pickLiveSymbolCandidate(candidates, ilt);
        if(pick < 0)
        {
            printf("error: ambiguous symbol '%s' (%zu records, no unique live body):",
                   modSym.c_str(), candidates.size());
            for(auto a : candidates)
                printf(" 0x%llX", (unsigned long long)a);
            printf(" - refusing to use it\n");
            fflush(stdout);
            return SymbolResult::Ambiguous;
        }
    }
    out = candidates[pick];

    // Cache the successful resolution
    cacheSymbol(modSym, out);

    return SymbolResult::Found;
}

bool GleamDebugger::ensureSymSession()
{
    if(mSymInitialized)
    {
        // Keep dbghelp's module list in sync with runtime DLL loads/unloads.
        SymRefreshModuleList(mProcess->hProcess);
        return true;
    }
    if(!mProcess)
        return false;
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    // Invade: dbghelp does not see the debuggee's modules unless it invades
    // (or each module is registered with SymLoadModuleEx). Deferred loads
    // keep this lazy.
    mSymInitialized = SymInitialize(mProcess->hProcess, nullptr, TRUE) != 0;
    return mSymInitialized;
}

void GleamDebugger::closeSymSession()
{
    if(!mProcess)
    {
        mSymLoadedBases.clear();
        mSymInitialized = false;
        return;
    }
    // Explicit SymLoadModuleEx loads pair with SymUnloadModule64 before the
    // session goes down; the tracking set must never outlive the process
    // (a new process may reuse the same DLL base).
    for(uint64_t base : mSymLoadedBases)
    {
        if(mSymInitialized)
            SymUnloadModule64(mProcess->hProcess, base);
    }
    mSymLoadedBases.clear();
    if(mSymInitialized)
    {
        SymCleanup(mProcess->hProcess);
        mSymInitialized = false;
    }
}

// ---- Module layout (P1: moduleinfo / sections) ----

namespace
{
    const char* machineText(uint16_t m)
    {
        switch(m)
        {
        case IMAGE_FILE_MACHINE_AMD64: return "x64";
        case IMAGE_FILE_MACHINE_I386:  return "x86";
        case IMAGE_FILE_MACHINE_ARM64: return "arm64";
        case IMAGE_FILE_MACHINE_ARMNT: return "arm";
        default: return "unknown";
        }
    }

    const char* subsystemText(uint16_t s)
    {
        switch(s)
        {
        case IMAGE_SUBSYSTEM_WINDOWS_GUI: return "gui";
        case IMAGE_SUBSYSTEM_WINDOWS_CUI: return "console";
        case IMAGE_SUBSYSTEM_NATIVE:      return "native";
        default: return "other";
        }
    }

    // The DllCharacteristics bits a reverse engineer acts on: they decide
    // whether addresses are stable (ASLR), whether the stack is executable
    // (DEP) and whether indirect calls are checked (CFG).
    std::string dllCharsText(uint16_t c)
    {
        struct Bit { uint16_t mask; const char* name; };
        static const Bit bits[] = {
            { IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE, "DYNAMIC_BASE" },
            { IMAGE_DLLCHARACTERISTICS_FORCE_INTEGRITY, "FORCE_INTEGRITY" },
            { IMAGE_DLLCHARACTERISTICS_NX_COMPAT, "NX_COMPAT" },
            { IMAGE_DLLCHARACTERISTICS_NO_ISOLATION, "NO_ISOLATION" },
            { IMAGE_DLLCHARACTERISTICS_NO_SEH, "NO_SEH" },
            { IMAGE_DLLCHARACTERISTICS_NO_BIND, "NO_BIND" },
            { IMAGE_DLLCHARACTERISTICS_APPCONTAINER, "APPCONTAINER" },
            { IMAGE_DLLCHARACTERISTICS_WDM_DRIVER, "WDM_DRIVER" },
            { IMAGE_DLLCHARACTERISTICS_GUARD_CF, "GUARD_CF" },
            { IMAGE_DLLCHARACTERISTICS_TERMINAL_SERVER_AWARE, "TS_AWARE" },
            { 0x0040 /* HIGH_ENTROPY_VA */, "HIGH_ENTROPY_VA" },
        };
        std::string out;
        for(const auto & b : bits)
        {
            if(!(c & b.mask))
                continue;
            if(!out.empty())
                out += ' ';
            out += b.name;
        }
        return out;
    }

    // "rwx"-style protection from the section characteristics.
    std::string sectionProt(uint32_t c)
    {
        std::string s;
        s += (c & IMAGE_SCN_MEM_READ) ? 'r' : '-';
        s += (c & IMAGE_SCN_MEM_WRITE) ? 'w' : '-';
        s += (c & IMAGE_SCN_MEM_EXECUTE) ? 'x' : '-';
        if(c & IMAGE_SCN_MEM_SHARED)
            s += 's';
        return s;
    }

    // Read the section table of a mapped image. Bounded by the header size so
    // a bogus NumberOfSections cannot drive an unbounded read.
    bool readSections(Process* process, uint64_t base, const PeInfo & pe,
                      std::vector<IMAGE_SECTION_HEADER> & out)
    {
        if(!pe.valid || !pe.sectionCount || pe.sectionCount > 96)
            return false;
        const uint64_t tableEnd = pe.sectionTableVa - base +
                                  (uint64_t)pe.sectionCount * sizeof(IMAGE_SECTION_HEADER);
        // The whole table must live inside the mapped headers.
        if(pe.sizeOfHeaders && tableEnd > pe.sizeOfHeaders)
            return false;
        out.resize(pe.sectionCount);
        return process->MemReadSafe(pe.sectionTableVa, out.data(),
                                    out.size() * sizeof(IMAGE_SECTION_HEADER));
    }

    // Section names are 8 bytes and NOT necessarily NUL-terminated.
    std::string sectionName(const IMAGE_SECTION_HEADER & s)
    {
        char buf[9] = "";
        memcpy(buf, s.Name, 8);
        buf[8] = '\0';
        return buf;
    }
}

void GleamDebugger::cmdSections(const std::string & moduleName)
{
    ModuleInfo mod;
    if(!findModule(mProcess->hProcess, moduleName, mod))
    {
        printf("module not found: %s\n", moduleName.c_str());
        fflush(stdout);
        return;
    }
    auto pe = readPeDirectories(mProcess, mod.base);
    std::vector<IMAGE_SECTION_HEADER> secs;
    if(!readSections(mProcess, mod.base, pe, secs))
    {
        printf("cannot read the section table of %s\n", moduleName.c_str());
        fflush(stdout);
        return;
    }
    printf("sections base=0x%llX count=%zu\n", (unsigned long long)mod.base, secs.size());
    printf("  %-8s %-18s %-10s %-10s %-4s %s\n",
           "name", "address", "vsize", "rawsize", "prot", "characteristics");
    for(const auto & s : secs)
    {
        printf("  %-8s 0x%016llX 0x%08lX 0x%08lX %-4s 0x%08lX\n",
               sectionName(s).c_str(),
               (unsigned long long)(mod.base + s.VirtualAddress),
               (unsigned long)s.Misc.VirtualSize,
               (unsigned long)s.SizeOfRawData,
               sectionProt(s.Characteristics).c_str(),
               (unsigned long)s.Characteristics);
    }
    fflush(stdout);
}

void GleamDebugger::cmdModuleInfo(const std::string & moduleName)
{
    ModuleInfo mod;
    if(!findModule(mProcess->hProcess, moduleName, mod))
    {
        printf("module not found: %s\n", moduleName.c_str());
        fflush(stdout);
        return;
    }
    const std::string norm = normalizeModuleName(mod.name.empty() ? moduleName : mod.name);
    printf("moduleinfo base=0x%llX size=0x%lX\n",
           (unsigned long long)mod.base, (unsigned long)mod.size);
    std::wstring path;
    if(imagePathOf(mod.base, norm, path))
    {
        char narrow[MAX_PATH * 2] = "";
        WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, narrow, sizeof(narrow), nullptr, nullptr);
        printf("  path: %s\n", narrow);
    }
    else
        printf("  path: (unknown)\n");

    auto pe = readPeDirectories(mProcess, mod.base);
    if(!pe.valid)
    {
        printf("  headers: unreadable or not a PE image\n");
        printSymbolStatus(mod.base);
        fflush(stdout);
        return;
    }
    printf("  machine: %s (0x%04X)  subsystem: %s  timestamp: 0x%08lX\n",
           machineText(pe.machine), pe.machine, subsystemText(pe.subsystem),
           (unsigned long)pe.timeDateStamp);
    // A relocated image is the reason a saved absolute address goes stale, so
    // state the delta rather than leaving it to be computed.
    const int64_t slide = (int64_t)(mod.base - pe.preferredBase);
    printf("  preferred-base: 0x%llX  relocated: %s",
           (unsigned long long)pe.preferredBase, slide ? "yes" : "no");
    if(slide)
        printf(" (slide %s0x%llX)", slide < 0 ? "-" : "+",
               (unsigned long long)(slide < 0 ? -slide : slide));
    printf("\n");
    printf("  image-size: 0x%lX  sections: %u  section-alignment: 0x%lX\n",
           (unsigned long)pe.sizeOfImage, pe.sectionCount,
           (unsigned long)pe.sectionAlignment);
    // OEP: the entry-breakpoint target. An RVA of 0 means "no entry point",
    // which is legal for a resource-only DLL - do not print a bogus address.
    if(pe.entryPointRva)
        printf("  oep: 0x%llX (rva 0x%lX)\n",
               (unsigned long long)(mod.base + pe.entryPointRva),
               (unsigned long)pe.entryPointRva);
    else
        printf("  oep: none (rva 0)\n");
    const std::string chars = dllCharsText(pe.dllCharacteristics);
    printf("  dll-characteristics: 0x%04X%s%s\n", pe.dllCharacteristics,
           chars.empty() ? "" : " ", chars.c_str());

    // Exception directory: the .pdata unwind records "frames" walks.
    if(pe.exceptionDir.VirtualAddress && pe.exceptionDir.Size)
        printf("  exception(.pdata): 0x%llX size=0x%lX entries=%lu\n",
               (unsigned long long)(mod.base + pe.exceptionDir.VirtualAddress),
               (unsigned long)pe.exceptionDir.Size,
               (unsigned long)(pe.exceptionDir.Size / sizeof(RUNTIME_FUNCTION)));
    else
        printf("  exception(.pdata): none (leaf-only or non-x64 unwind)\n");
    if(pe.relocDir.VirtualAddress && pe.relocDir.Size)
        printf("  relocations: 0x%llX size=0x%lX\n",
               (unsigned long long)(mod.base + pe.relocDir.VirtualAddress),
               (unsigned long)pe.relocDir.Size);

    // TLS: the callbacks run before the entry point, so they are where
    // initialisation (and anti-debug) hides.
    if(pe.tlsDir.VirtualAddress && pe.tlsDir.Size &&
       rangeInImage(pe.tlsDir.VirtualAddress, pe.tlsDir.Size, pe.sizeOfImage))
    {
        printf("  tls: 0x%llX size=0x%lX\n",
               (unsigned long long)(mod.base + pe.tlsDir.VirtualAddress),
               (unsigned long)pe.tlsDir.Size);
        // AddressOfCallBacks is a VA, already relocated by the loader in the
        // mapped copy, pointing at a NULL-terminated array of VAs.
        uint64_t callbackArray = 0, indexVa = 0;
        bool haveDir = false;
        if(pe.pe64)
        {
            IMAGE_TLS_DIRECTORY64 tls{};
            if(readAt(mProcess, mod.base + pe.tlsDir.VirtualAddress, tls))
            {
                callbackArray = tls.AddressOfCallBacks;
                indexVa = tls.AddressOfIndex;
                haveDir = true;
                printf("    raw-data: 0x%llX..0x%llX  zero-fill: 0x%lX\n",
                       (unsigned long long)tls.StartAddressOfRawData,
                       (unsigned long long)tls.EndAddressOfRawData,
                       (unsigned long)tls.SizeOfZeroFill);
            }
        }
        else
        {
            IMAGE_TLS_DIRECTORY32 tls{};
            if(readAt(mProcess, mod.base + pe.tlsDir.VirtualAddress, tls))
            {
                callbackArray = tls.AddressOfCallBacks;
                indexVa = tls.AddressOfIndex;
                haveDir = true;
            }
        }
        if(haveDir)
        {
            printf("    index-at: 0x%llX\n", (unsigned long long)indexVa);
            if(!callbackArray)
                printf("    callbacks: none\n");
            else
            {
                // Bounded walk: the array is NUL-terminated, but a corrupt or
                // hostile image must not spin here.
                uint32_t n = 0;
                bool readFailed = false;
                for(; n < Gleam::Limits::TLS_CALLBACK_MAX; n++)
                {
                    uint64_t cb = 0;
                    if(pe.pe64)
                    {
                        if(!readAt(mProcess, callbackArray + n * 8, cb))
                        {
                            readFailed = true;
                            break;
                        }
                    }
                    else
                    {
                        uint32_t cb32 = 0;
                        if(!readAt(mProcess, callbackArray + n * 4, cb32))
                        {
                            readFailed = true;
                            break;
                        }
                        cb = cb32;
                    }
                    if(!cb)
                        break;
                    std::string sym = symNameByAddr(cb);
                    printf("    callback[%u]: 0x%llX%s%s\n", n, (unsigned long long)cb,
                           sym.empty() ? "" : " ", sym.c_str());
                }
                if(readFailed)
                    printf("    callbacks: truncated (array at 0x%llX unreadable at index %u)\n",
                           (unsigned long long)callbackArray, n);
                else if(n == 0)
                    printf("    callbacks: none (empty array)\n");
                else
                    printf("    callbacks: %u\n", n);
            }
        }
    }
    else
        printf("  tls: none\n");

    // Load Config / CFG. The directory Size field decides which fields exist:
    // the struct grew across SDK versions, so anything beyond the declared
    // size is not present in THIS image and must not be read as if it were.
    if(pe.loadConfigDir.VirtualAddress && pe.loadConfigDir.Size &&
       rangeInImage(pe.loadConfigDir.VirtualAddress, pe.loadConfigDir.Size, pe.sizeOfImage))
    {
        printf("  load-config: 0x%llX size=0x%lX\n",
               (unsigned long long)(mod.base + pe.loadConfigDir.VirtualAddress),
               (unsigned long)pe.loadConfigDir.Size);
        if(pe.pe64)
        {
            IMAGE_LOAD_CONFIG_DIRECTORY64 lc{};
            const size_t want = (std::min)((size_t)pe.loadConfigDir.Size, sizeof(lc));
            if(mProcess->MemReadSafe(mod.base + pe.loadConfigDir.VirtualAddress, &lc, want))
            {
                // Offset of the last byte each field needs, compared against
                // the size the image actually declares.
                const uint32_t declared = pe.loadConfigDir.Size;
                auto has = [declared](size_t endOffset) { return declared >= endOffset; };
                if(has(offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, SecurityCookie) + 8) &&
                   lc.SecurityCookie)
                    printf("    security-cookie: 0x%llX\n",
                           (unsigned long long)lc.SecurityCookie);
                if(has(offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, SEHandlerTable) + 8) &&
                   lc.SEHandlerTable)
                    printf("    safeseh-table: 0x%llX count=%llu\n",
                           (unsigned long long)lc.SEHandlerTable,
                           (unsigned long long)lc.SEHandlerCount);
                if(has(offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, GuardCFCheckFunctionPointer) + 8) &&
                   lc.GuardCFCheckFunctionPointer)
                    printf("    guard-check-fptr: 0x%llX\n",
                           (unsigned long long)lc.GuardCFCheckFunctionPointer);
                if(has(offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, GuardFlags) + 4))
                    printf("    guard-cf-table: 0x%llX count=%llu flags=0x%08lX\n",
                           (unsigned long long)lc.GuardCFFunctionTable,
                           (unsigned long long)lc.GuardCFFunctionCount,
                           (unsigned long)lc.GuardFlags);
            }
            else
                printf("    (unreadable)\n");
        }
    }
    else
        printf("  load-config: none\n");

    printSymbolStatus(mod.base);
    fflush(stdout);
}

// ---- Symbol control (P1: sympath / symload / symreload) ----

// Image path of a loaded module. The path learned from the DLL load event's
// file handle is preferred over the loader list: it is the authoritative
// identity (see cbLoadDllEvent), and it is available for modules whose loader
// record is not linked yet.
bool GleamDebugger::imagePathOf(uint64_t base, const std::string & normalizedName,
                                std::wstring & out)
{
    if(!normalizedName.empty())
    {
        auto found = mModulePaths.find(normalizedName);
        if(found != mModulePaths.end() && !found->second.empty())
        {
            out = found->second;
            return true;
        }
    }
    if(!mProcess)
        return false;
    wchar_t wpath[MAX_PATH * 2] = L"";
    if(GetModuleFileNameExW(mProcess->hProcess, (HMODULE)base, wpath, ARRAYSIZE(wpath)) && *wpath)
    {
        out = wpath;
        return true;
    }
    return false;
}

// Report what dbghelp actually has for a module. SymType is the answer to
// "why does module!symbol not resolve": SymPdb/SymDia mean real symbols,
// SymExport means dbghelp fell back to the export table (function names only,
// no statics and no private symbols), SymNone means nothing at all.
void GleamDebugger::printSymbolStatus(uint64_t base)
{
    if(!ensureSymSession())
    {
        printf("  symbols: unavailable (no dbghelp session)\n");
        return;
    }
    IMAGEHLP_MODULE64 mi{};
    mi.SizeOfStruct = sizeof(mi);
    // NOTE: with SYMOPT_DEFERRED_LOADS this call forces the pending load, so
    // it is deliberately NOT used in list-all commands like "modules".
    if(!SymGetModuleInfo64(mProcess->hProcess, (DWORD64)base, &mi))
    {
        printf("  symbols: none (module not known to dbghelp, error %lu)\n", GetLastError());
        return;
    }
    const char* kind = "unknown";
    switch(mi.SymType)
    {
    case SymNone:    kind = "none"; break;
    case SymCoff:    kind = "coff"; break;
    case SymCv:      kind = "codeview"; break;
    case SymPdb:     kind = "pdb"; break;
    case SymExport:  kind = "export-only"; break;
    case SymDeferred:kind = "deferred"; break;
    case SymSym:     kind = "sym"; break;
    case SymDia:     kind = "dia"; break;
    case SymVirtual: kind = "virtual"; break;
    default: break;
    }
    printf("  symbols: %s%s\n", kind,
           mSymLoadedBases.count(base) ? " (explicitly loaded)" : "");
    if(mi.LoadedPdbName[0])
        printf("  pdb: %s\n", mi.LoadedPdbName);
    else if(mi.LoadedImageName[0])
        printf("  symbol-image: %s\n", mi.LoadedImageName);
    if(mi.SymType == SymPdb || mi.SymType == SymDia)
    {
        // PdbSig70 (a GUID) is the modern identity; the old 32-bit PdbSig is 0
        // for every PDB 7.0 file, which is all of them in practice. This is the
        // same GUID+Age pair verifyModuleIdentity() compares.
        printf("  pdb-guid: %08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X age=%lu unmatched=%d\n",
               mi.PdbSig70.Data1, mi.PdbSig70.Data2, mi.PdbSig70.Data3,
               mi.PdbSig70.Data4[0], mi.PdbSig70.Data4[1], mi.PdbSig70.Data4[2],
               mi.PdbSig70.Data4[3], mi.PdbSig70.Data4[4], mi.PdbSig70.Data4[5],
               mi.PdbSig70.Data4[6], mi.PdbSig70.Data4[7],
               mi.PdbAge, mi.PdbUnmatched ? 1 : 0);
    }
}

void GleamDebugger::cmdSymPath(const std::string & path, bool set)
{
    if(!ensureSymSession())
    {
        printf("symbol session unavailable\n");
        fflush(stdout);
        return;
    }
    if(!set)
    {
        std::vector<char> buf(4096, 0);
        if(SymGetSearchPath(mProcess->hProcess, buf.data(), (DWORD)buf.size() - 1))
            printf("sympath %s\n", buf.data());
        else
            printf("sympath unavailable (error %lu)\n", GetLastError());
        fflush(stdout);
        return;
    }
    if(!SymSetSearchPath(mProcess->hProcess, path.c_str()))
    {
        printf("sympath failed (error %lu)\n", GetLastError());
        fflush(stdout);
        return;
    }
    // The new path governs FUTURE loads only; modules dbghelp already resolved
    // keep their symbols. Cached resolutions came from the old configuration.
    clearSymbolCache();
    printf("sympath set: %s\n", path.c_str());
    printf("note: applies to modules loaded from now on; use 'symreload' to "
           "re-resolve modules already loaded\n");
    fflush(stdout);
}

void GleamDebugger::cmdSymLoad(const std::string & moduleName)
{
    if(!ensureSymSession())
    {
        printf("symbol session unavailable\n");
        fflush(stdout);
        return;
    }
    ModuleInfo mod;
    if(!findModule(mProcess->hProcess, moduleName, mod))
    {
        printf("module not found: %s\n", moduleName.c_str());
        fflush(stdout);
        return;
    }
    const std::string norm = normalizeModuleName(mod.name.empty() ? moduleName : mod.name);
    std::wstring path;
    if(!imagePathOf(mod.base, norm, path))
    {
        printf("symload failed: cannot determine the image path of %s\n", moduleName.c_str());
        fflush(stdout);
        return;
    }
    // Unload first, unconditionally. dbghelp refuses to load over a module it
    // already knows, and after the invaded SymInitialize it knows most of them
    // - so without this, symload would fail on exactly the modules the user is
    // most likely to name. Not being loaded is not an error here.
    SymUnloadModule64(mProcess->hProcess, (DWORD64)mod.base);
    mSymLoadedBases.erase(mod.base);
    SetLastError(ERROR_SUCCESS);
    if(!SymLoadModuleExW(mProcess->hProcess, NULL, path.c_str(), NULL,
                         mod.base, 0 /* size from image */, NULL, 0))
    {
        // Documented quirk: a zero return with ERROR_SUCCESS means "already
        // loaded", which is a no-op success rather than a failure.
        const DWORD err = GetLastError();
        if(err != ERROR_SUCCESS)
        {
            printf("symload failed for %s (error %lu)\n", moduleName.c_str(), err);
            fflush(stdout);
            return;
        }
        printf("symload base=0x%llX %s (already loaded)\n",
               (unsigned long long)mod.base, moduleName.c_str());
        clearSymbolCache();
        printSymbolStatus(mod.base);
        fflush(stdout);
        return;
    }
    mSymLoadedBases.insert(mod.base);
    clearSymbolCache(); // previously-failed lookups must be retried
    printf("symload base=0x%llX %s\n", (unsigned long long)mod.base, moduleName.c_str());
    printSymbolStatus(mod.base);
    fflush(stdout);
}

void GleamDebugger::cmdSymReload(const std::string & moduleName)
{
    if(!mProcess)
    {
        printf("no process\n");
        fflush(stdout);
        return;
    }
    if(moduleName.empty())
    {
        // Whole-session rebuild: tear the dbghelp session down and re-invade.
        // This is the recovery path when the initial invade raced the loader
        // or the search path has since changed.
        closeSymSession();
        clearSymbolCache();
        if(!ensureSymSession())
        {
            printf("symreload failed: cannot re-create the symbol session\n");
            fflush(stdout);
            return;
        }
        printf("symreload: symbol session rebuilt\n");
        fflush(stdout);
        return;
    }
    if(!ensureSymSession())
    {
        printf("symbol session unavailable\n");
        fflush(stdout);
        return;
    }
    ModuleInfo mod;
    if(!findModule(mProcess->hProcess, moduleName, mod))
    {
        printf("module not found: %s\n", moduleName.c_str());
        fflush(stdout);
        return;
    }
    SymUnloadModule64(mProcess->hProcess, (DWORD64)mod.base);
    mSymLoadedBases.erase(mod.base);
    clearSymbolCache();
    const std::string norm = normalizeModuleName(mod.name.empty() ? moduleName : mod.name);
    std::wstring path;
    bool loaded = false;
    if(imagePathOf(mod.base, norm, path))
    {
        loaded = SymLoadModuleExW(mProcess->hProcess, NULL, path.c_str(), NULL,
                                  mod.base, 0, NULL, 0) != 0;
        if(loaded)
            mSymLoadedBases.insert(mod.base);
    }
    if(!loaded)
    {
        // Fall back to dbghelp's own view: without an explicit load the module
        // is still reachable through the invaded session's refresh.
        SymRefreshModuleList(mProcess->hProcess);
        printf("symreload base=0x%llX %s (via module-list refresh)\n",
               (unsigned long long)mod.base, moduleName.c_str());
    }
    else
        printf("symreload base=0x%llX %s\n", (unsigned long long)mod.base, moduleName.c_str());
    printSymbolStatus(mod.base);
    fflush(stdout);
}

void GleamDebugger::cmdImports(const std::string & moduleName)
{
    ModuleInfo mod;
    if(moduleName.empty())
    {
        // Find the loader record whose base matches the main image base from
        // the debug event - never just the first enumerated module. On any
        // mismatch or failure we do NOT fall back to the (target-modifiable)
        // PE header value.
        HMODULE modules[1024];
        DWORD needed = 0;
        bool found = false;
        if(EnumProcessModules(mProcess->hProcess, modules, sizeof(modules), &needed))
        {
            DWORD count = (DWORD)(std::min)(needed / sizeof(HMODULE), sizeof(modules) / sizeof(HMODULE));
            for(DWORD i = 0; i < count && !found; i++)
            {
                MODULEINFO mi{};
                if(!GetModuleInformation(mProcess->hProcess, modules[i], &mi, sizeof(mi)))
                    continue;
                if((uint64_t)(uintptr_t)mi.lpBaseOfDll == (uint64_t)mProcess->createProcessInfo.lpBaseOfImage)
                {
                    mod.base = (uint64_t)(uintptr_t)mi.lpBaseOfDll;
                    mod.size = mi.SizeOfImage;
                    found = true;
                }
            }
        }
        if(!found || !mod.size)
        {
            printf("cannot determine a trusted image range for the main module\n");
            fflush(stdout);
            return;
        }
        mod.name = "(main module)";
    }
    else if(!findModule(mProcess->hProcess, moduleName, mod))
    {
        printf("module not found: %s (try 'modules')\n", moduleName.c_str());
        fflush(stdout);
        return;
    }

    auto pe = readPeDirectories(mProcess, mod.base);
    if(!pe.valid)
    {
        printf("failed to read PE headers at 0x%llX\n", mod.base);
        fflush(stdout);
        return;
    }
    // Every read below is bounded by the loader-reported image range.
    const uint64_t imageSize = mod.size;
    if(!imageSize)
    {
        printf("cannot determine a trusted image size for 0x%llX\n", mod.base);
        fflush(stdout);
        return;
    }
    if(!ensureSymSession())
    {
        printf("dbghelp SymInitialize failed\n");
        fflush(stdout);
        return;
    }

    char symBuf[sizeof(SYMBOL_INFO) + MAX_SYM_NAME];
    auto resolve = [this, &symBuf](uint64_t addr, uint64_t & disp) -> std::string
    {
        disp = 0;
        if(!addr)
            return std::string("(unbound)");
        memset(symBuf, 0, sizeof(symBuf));
        auto si = (SYMBOL_INFO*)symBuf;
        si->SizeOfStruct = sizeof(SYMBOL_INFO);
        si->MaxNameLen = MAX_SYM_NAME;
        if(SymFromAddr(mProcess->hProcess, addr, &disp, si))
            return std::string(si->Name);
        return std::string("?");
    };

    const size_t thunkSize = pe.pe64 ? 8 : 4;
    size_t groups = 0, slots = 0;

    // Bound imports. The directory RVA/Size must lie inside the image; the
    // walk is bounded by the directory size and each thunk walk by the image.
    if(pe.importDir.VirtualAddress &&
       rangeInImage(pe.importDir.VirtualAddress, pe.importDir.Size, imageSize))
    {
        uint32_t maxDesc = pe.importDir.Size
            ? pe.importDir.Size / (uint32_t)sizeof(IMAGE_IMPORT_DESCRIPTOR) : 0;
        if(maxDesc > 4096)
            maxDesc = 4096;
        for(uint32_t i = 0; i < maxDesc; i++)
        {
            if(!rangeInImage(pe.importDir.VirtualAddress + (uint64_t)i * sizeof(IMAGE_IMPORT_DESCRIPTOR),
                             sizeof(IMAGE_IMPORT_DESCRIPTOR), imageSize))
                break;
            IMAGE_IMPORT_DESCRIPTOR desc;
            if(!readAt(mProcess, mod.base + pe.importDir.VirtualAddress + i * sizeof(desc), desc))
                break;
            if(!desc.Name && !desc.FirstThunk)
                break;
            if(!rangeInImage(desc.Name, 1, imageSize) || !rangeInImage(desc.FirstThunk, 1, imageSize))
                continue; // bogus RVA in a malformed descriptor
            auto dllName = readCString(mProcess, mod.base + desc.Name,
                                       (size_t)(std::min)((uint64_t)260, imageSize - desc.Name));
            if(!dllName.second)
            {
                printf("(invalid dll name at rva 0x%llX, skipped)\n", (unsigned long long)desc.Name);
                continue; // unterminated or truncated: not a valid import entry
            }
            printf("%s:\n", dllName.first.c_str());
            groups++;
            // Thunk walk bounded by complete elements inside the image.
            uint64_t maxT = 0;
            if(desc.FirstThunk < imageSize)
                maxT = (std::min)((uint64_t)65536, (imageSize - desc.FirstThunk) / thunkSize);
            for(uint32_t t = 0; t < maxT; t++)
            {
                uint64_t slotAddr = mod.base + desc.FirstThunk + t * thunkSize;
                uint64_t value = 0;
                if(pe.pe64)
                {
                    if(!readAt(mProcess, slotAddr, value))
                        break;
                }
                else
                {
                    uint32_t v32 = 0;
                    if(!readAt(mProcess, slotAddr, v32))
                        break;
                    value = v32;
                }
                if(!value)
                    break;
                uint64_t disp = 0;
                auto name = resolve(value, disp);
                printf("  0x%016llX  %s%s\n",
                       slotAddr,
                       name.c_str(),
                       disp ? " (hooked?)" : "");
                slots++;
            }
        }
    }

    // Delay-loaded imports (same bounding rules).
    if(pe.delayImportDir.VirtualAddress &&
       rangeInImage(pe.delayImportDir.VirtualAddress, pe.delayImportDir.Size, imageSize))
    {
        uint32_t maxDesc = pe.delayImportDir.Size
            ? pe.delayImportDir.Size / (uint32_t)sizeof(ImgDelayDescr) : 0;
        if(maxDesc > 4096)
            maxDesc = 4096;
        for(uint32_t i = 0; i < maxDesc; i++)
        {
            if(!rangeInImage(pe.delayImportDir.VirtualAddress + (uint64_t)i * sizeof(ImgDelayDescr),
                             sizeof(ImgDelayDescr), imageSize))
                break;
            ImgDelayDescr desc{};
            if(!readAt(mProcess, mod.base + pe.delayImportDir.VirtualAddress + i * sizeof(desc), desc))
                break;
            if(!desc.rvaDLLName)
                break;
            // dlattrRva: when set, the fields are RVAs; otherwise absolute VAs.
            const bool rva = (desc.grAttrs & 1) != 0;
            uint64_t nameAddr = rva ? mod.base + (uint64_t)desc.rvaDLLName : (uint64_t)desc.rvaDLLName;
            uint64_t iatAddr = rva ? mod.base + (uint64_t)desc.rvaIAT : (uint64_t)desc.rvaIAT;
            if(!rangeInImage(nameAddr - mod.base, 1, imageSize) || !rangeInImage(iatAddr - mod.base, 1, imageSize))
                continue;
            auto dllName = readCString(mProcess, nameAddr,
                                       (size_t)(std::min)((uint64_t)260, imageSize - (nameAddr - mod.base)));
            if(!dllName.second)
            {
                printf("(invalid dll name at 0x%llX, skipped)\n", (unsigned long long)nameAddr);
                continue;
            }
            printf("%s (delay):\n", dllName.first.c_str());
            groups++;
            // Thunk walk bounded by complete elements inside the image.
            uint64_t iatRva = iatAddr - mod.base;
            uint64_t maxT = (std::min)((uint64_t)65536, (imageSize - iatRva) / thunkSize);
            for(uint32_t t = 0; t < maxT; t++)
            {
                uint64_t slotAddr = iatAddr + t * thunkSize;
                uint64_t value = 0;
                if(pe.pe64)
                {
                    if(!readAt(mProcess, slotAddr, value))
                        break;
                }
                else
                {
                    uint32_t v32 = 0;
                    if(!readAt(mProcess, slotAddr, v32))
                        break;
                    value = v32;
                }
                if(!value)
                    break;
                uint64_t disp = 0;
                auto name = resolve(value, disp);
                printf("  0x%016llX  %s%s\n",
                       slotAddr,
                       name.c_str(),
                       disp ? " (hooked?)" : "");
                slots++;
            }
        }
    }

    printf("%zu DLLs, %zu imported functions\n", groups, slots);
    fflush(stdout);
}

void GleamDebugger::cmdExports(const std::string & moduleName, const std::string & filter)
{
    ModuleInfo mod;
    if(!findModule(mProcess->hProcess, moduleName, mod))
    {
        printf("module not found: %s (try 'modules')\n", moduleName.c_str());
        fflush(stdout);
        return;
    }
    if(!ensureSymSession())
    {
        printf("dbghelp SymInitialize failed\n");
        fflush(stdout);
        return;
    }
    printf("%s exports (base 0x%llX):\n", mod.name.c_str(), mod.base);
    EnumCtx ctx{ &filter, 0 };
    if(!SymEnumSymbols(mProcess->hProcess, mod.base, "*", enumSymbolCb, &ctx))
        printf("(enumeration failed or no symbols)\n");
    printf("%zu symbols\n", ctx.shown);
    fflush(stdout);
}

void GleamDebugger::cmdSym(uint64_t addr)
{
    if(!ensureSymSession())
    {
        printf("dbghelp SymInitialize failed\n");
        fflush(stdout);
        return;
    }
    char buf[sizeof(SYMBOL_INFO) + MAX_SYM_NAME];
    memset(buf, 0, sizeof(buf));
    auto si = (SYMBOL_INFO*)buf;
    si->SizeOfStruct = sizeof(SYMBOL_INFO);
    si->MaxNameLen = MAX_SYM_NAME;
    uint64_t disp = 0;
    if(SymFromAddr(mProcess->hProcess, addr, &disp, si))
        printf("0x%llX  %s+0x%llX\n", addr, si->Name, disp);
    else
        printf("no symbol for 0x%llX\n", addr);
    fflush(stdout);
}

// ---- frames: real stack frame enumeration (RtlVirtualUnwind + own .pdata) ----

// StackWalk64 on x64 proved unusable here (empirically: it fills AddrReturn
// but leaves AddrPC/AddrStack and the context stale). RtlVirtualUnwind -
// the unwinder dbghelp itself wraps - updates the full context correctly,
// but runs locally, so cmdFrames mirrors the remote stack and the remote
// code/unwind-data span into our address space. ntdll_x64.lib exports it.
extern "C" NTSYSAPI PEXCEPTION_ROUTINE NTAPI RtlVirtualUnwind(
    ULONG HandlerType,
    ULONG64 ImageBase,
    ULONG64 ControlPc,
    PRUNTIME_FUNCTION FunctionEntry,
    PCONTEXT ContextRecord,
    PVOID* HandlerData,
    PULONG64 EstablisherFrame,
    PKNONVOLATILE_CONTEXT_POINTERS ContextPointers);

// SEH-wrapped RtlVirtualUnwind: a bad remote unwind record must never kill
// the debugger. No C++ objects here so __try is allowed (C2712).
static uint32_t virtualUnwindSeh(uint64_t fakeBase, uint64_t controlPc,
                                 RUNTIME_FUNCTION* rf, CONTEXT* ctx)
{
    PVOID handlerData = nullptr;
    uint64_t establisherFrame = 0;
    uint32_t code = 0;
    __try
    {
        RtlVirtualUnwind(0 /* UNW_FLAG_NHANDLER */, fakeBase, controlPc, rf, ctx,
                         &handlerData, &establisherFrame, nullptr);
    }
    __except(code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
    {
    }
    return code;
}

uint64_t GleamDebugger::moduleBaseOf(uint64_t addr)
{
    if(!mProcess)
        return 0;
    ModuleInfo mod;
    return moduleOf(mProcess->hProcess, addr, mod) ? mod.base : 0;
}

// The DLL's own name from its export directory. Works during the DLL load
// event, when the loader-list based APIs (EnumProcessModules,
// GetModuleFileNameEx, dbghelp refresh) are still blind.
std::string GleamDebugger::dllNameFromBase(uint64_t base)
{
    if(!mProcess)
        return std::string();
    auto pe = readPeDirectories(mProcess, base);
    if(!pe.valid || !pe.exportDir.VirtualAddress)
        return std::string();
    IMAGE_EXPORT_DIRECTORY exp;
    if(!readAt(mProcess, base + pe.exportDir.VirtualAddress, exp))
        return std::string();
    if(!rangeInImage(exp.Name, 1, pe.sizeOfImage))
        return std::string();
    auto nm = readCString(mProcess, base + exp.Name, 260);
    return nm.second ? nm.first : std::string();
}

// Resolve an exported function by walking the export table directly (same
// reason as above: no loader-list or dbghelp dependency). Forwarded exports
// return 0 (unresolved).
namespace
{
    struct CodeViewId
    {
        bool valid = false;
        GUID guid{};
        uint32_t age = 0;
        uint32_t sizeOfImage = 0;
    };

    // Read the RSDS CodeView record from a module loaded in the debuggee.
    CodeViewId codeViewFromModule(Process* process, uint64_t base)
    {
        CodeViewId id;
        auto pe = readPeDirectories(process, base);
        if(!pe.valid || !pe.debugDir.VirtualAddress ||
           !rangeInImage(pe.debugDir.VirtualAddress, pe.debugDir.Size, pe.sizeOfImage))
            return id;
        IMAGE_DEBUG_DIRECTORY dir;
        const uint32_t count = pe.debugDir.Size / (uint32_t)sizeof(dir);
        for(uint32_t i = 0; i < count; i++)
        {
            if(!process->MemReadSafe(base + pe.debugDir.VirtualAddress + (uint64_t)i * sizeof(dir),
                                     &dir, sizeof(dir)))
                return id;
            if(dir.Type != IMAGE_DEBUG_TYPE_CODEVIEW || dir.SizeOfData < 24 ||
               !rangeInImage(dir.AddressOfRawData, 24, pe.sizeOfImage))
                continue;
            struct
            {
                char sig[4];
                GUID guid;
                uint32_t age;
            } rsds;
            if(!process->MemReadSafe(base + dir.AddressOfRawData, &rsds, sizeof(rsds)) ||
               memcmp(rsds.sig, "RSDS", 4) != 0)
                continue;
            id.valid = true;
            id.guid = rsds.guid;
            id.age = rsds.age;
            id.sizeOfImage = pe.sizeOfImage;
            return id;
        }
        return id;
    }

    // Read the RSDS CodeView record from a PE file on disk.
    // RAII: Uses Gleam::UniqueHandle and Gleam::MappedView for automatic cleanup
    CodeViewId codeViewFromFile(const wchar_t* path)
    {
        CodeViewId id;
        // GetModuleFileNameExW returns native "\??\" paths; CreateFileW
        // needs them gone (plain "C:\") or in extended form ("\\?\").
        if(path[0] == L'\\' && path[1] == L'?' && path[2] == L'?' && path[3] == L'\\')
            path += 4;

        Gleam::UniqueHandle hFile(CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                                               OPEN_EXISTING, 0, nullptr));
        if(!hFile)
            return id;

        Gleam::UniqueHandle hMap(CreateFileMappingW(hFile.get(), nullptr, PAGE_READONLY, 0, 0, nullptr));
        if(!hMap)
            return id;

        Gleam::MappedView mappedView(MapViewOfFile(hMap.get(), FILE_MAP_READ, 0, 0, 0));
        if(!mappedView)
            return id;

        const uint8_t* view = mappedView.as_bytes();
        const uint64_t fileSize = GetFileSize(hFile.get(), nullptr);
        auto dos = (const IMAGE_DOS_HEADER*)view;
        if(fileSize >= sizeof(IMAGE_DOS_HEADER) && dos->e_magic == IMAGE_DOS_SIGNATURE &&
           (uint64_t)dos->e_lfanew + sizeof(IMAGE_NT_HEADERS64) <= fileSize)
        {
            auto nt = (const IMAGE_NT_HEADERS64*)(view + dos->e_lfanew);
            if(nt->Signature == IMAGE_NT_SIGNATURE &&
               nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC)
            {
                // The debug directory's VirtualAddress is an RVA; map it
                // to a file offset through the section table.
                auto sec = (const IMAGE_SECTION_HEADER*)(
                    (const uint8_t*)&nt->OptionalHeader + nt->FileHeader.SizeOfOptionalHeader);
                auto rvaToOffset = [&](uint32_t rva) -> uint32_t
                {
                    for(int i = 0; i < nt->FileHeader.NumberOfSections; i++)
                        if(rva >= sec[i].VirtualAddress &&
                           rva < sec[i].VirtualAddress + sec[i].Misc.VirtualSize)
                            return sec[i].PointerToRawData + (rva - sec[i].VirtualAddress);
                    return 0;
                };
                const auto & dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
                const uint32_t dirOff = rvaToOffset(dd.VirtualAddress);
                const uint32_t count = dd.Size / (uint32_t)sizeof(IMAGE_DEBUG_DIRECTORY);
                for(uint32_t i = 0; i < count && !id.valid; i++)
                {
                    if(!dirOff || (uint64_t)dirOff + (i + 1) * sizeof(IMAGE_DEBUG_DIRECTORY) > fileSize)
                        break;
                    auto dir = (const IMAGE_DEBUG_DIRECTORY*)(view + dirOff + i * sizeof(IMAGE_DEBUG_DIRECTORY));
                    if(dir->Type != IMAGE_DEBUG_TYPE_CODEVIEW || dir->SizeOfData < 24 ||
                       (uint64_t)dir->PointerToRawData + 24 > fileSize)
                        continue;
                    const uint8_t* rsds = view + dir->PointerToRawData;
                    if(memcmp(rsds, "RSDS", 4) == 0)
                    {
                        id.valid = true;
                        memcpy(&id.guid, rsds + 4, sizeof(GUID));
                        memcpy(&id.age, rsds + 20, sizeof(uint32_t));
                        id.sizeOfImage = nt->OptionalHeader.SizeOfImage;
                    }
                }
            }
        }
        // Resources automatically cleaned up by RAII destructors
        return id;
    }
}

// Prove "this loaded module IS that file on disk" by comparing their
// CodeView GUID+Age (and SizeOfImage). Never use symbol resolution as
// proof - loading the file's symbols can be arranged for any image.
bool GleamDebugger::verifyModuleIdentity(uint64_t moduleBase, const wchar_t* imagePath)
{
    auto remote = codeViewFromModule(mProcess, moduleBase);
    auto file = codeViewFromFile(imagePath);
    if(!remote.valid || !file.valid)
        return false;
    return remote.age == file.age &&
           remote.sizeOfImage == file.sizeOfImage &&
           memcmp(&remote.guid, &file.guid, sizeof(GUID)) == 0;
}

// Resolve a PDB-only symbol by explicitly loading the module's symbols
// from its file on disk. The invade-based dbghelp session depends on the
// loader list, which is not yet linked during the DLL load event - this
// path has no such dependency. Now uses the same ILT-based disambiguation
// as resolveModuleSymbol.
GleamDebugger::SymbolResult GleamDebugger::resolvePdbSymbol(uint64_t moduleBase, const wchar_t* imagePath,
                                                              const std::string & symbol, uint64_t & out)
{
    if(!imagePath || !*imagePath || !ensureSymSession())
        return SymbolResult::NotFound;
    if(!mSymLoadedBases.count(moduleBase))
    {
        if(!SymLoadModuleExW(mProcess->hProcess, NULL, imagePath, NULL,
                             moduleBase, 0 /* size from image */, NULL, 0))
            return SymbolResult::NotFound;
        mSymLoadedBases.insert(moduleBase);
    }

    // Enumerate all records (same logic as resolveModuleSymbol).
    std::vector<uint64_t> candidates;
    {
        struct Ctx { std::vector<uint64_t>* addrs; } ctx{ &candidates };
        auto cb = [](PSYMBOL_INFO si, ULONG, PVOID userCtx) -> BOOL
        {
            auto c = (Ctx*)userCtx;
            c->addrs->push_back(si->Address);
            return TRUE;
        };
        // Wide path -> UTF-8 for the module name in "module!symbol".
        char narrow[MAX_PATH * 2] = "";
        WideCharToMultiByte(CP_UTF8, 0, imagePath, -1, narrow, sizeof(narrow), nullptr, nullptr);
        std::string modSym = normalizeModuleName(narrow) + "!" + symbol;
        if(!SymEnumSymbols(mProcess->hProcess, 0, modSym.c_str(), cb, &ctx))
            return SymbolResult::NotFound;
    }
    if(candidates.empty())
        return SymbolResult::NotFound;
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());

    int pick = 0;
    if(candidates.size() > 1)
    {
        // Same ILT-based disambiguation.
        auto ilt = iltThunkTargets(mProcess, moduleBase);
        pick = pickLiveSymbolCandidate(candidates, ilt);
        if(pick < 0)
        {
            // Wide path -> UTF-8 for error message.
            char narrow[MAX_PATH * 2] = "";
            WideCharToMultiByte(CP_UTF8, 0, imagePath, -1, narrow, sizeof(narrow), nullptr, nullptr);
            std::string modName = normalizeModuleName(narrow);
            printf("error: ambiguous symbol '%s!%s' (%zu records, no unique live body):",
                   modName.c_str(), symbol.c_str(), candidates.size());
            for(auto a : candidates)
                printf(" 0x%llX", (unsigned long long)a);
            printf(" - refusing to use it\n");
            fflush(stdout);
            return SymbolResult::Ambiguous;
        }
    }
    out = candidates[pick];

    // Cache the successful resolution - need to reconstruct cacheKey here
    char keyBuf[256];
    snprintf(keyBuf, sizeof(keyBuf), "%llX:%s", (unsigned long long)moduleBase, symbol.c_str());
    cacheSymbol(std::string(keyBuf), out);

    return SymbolResult::Found;
}

uint64_t GleamDebugger::findExportByName(uint64_t base, const std::string & name)
{
    if(!mProcess)
        return 0;
    auto pe = readPeDirectories(mProcess, base);
    if(!pe.valid || !rangeInImage(pe.exportDir.VirtualAddress, pe.exportDir.Size, pe.sizeOfImage) ||
       !pe.exportDir.VirtualAddress)
        return 0;
    IMAGE_EXPORT_DIRECTORY exp;
    if(!readAt(mProcess, base + pe.exportDir.VirtualAddress, exp))
        return 0;
    const uint32_t count = exp.NumberOfNames > 0x100000u ? 0x100000u : exp.NumberOfNames;
    for(uint32_t i = 0; i < count; i++)
    {
        uint32_t nameRva = 0;
        if(!readAt(mProcess, base + exp.AddressOfNames + (uint64_t)i * sizeof(uint32_t), nameRva))
            break;
        if(!rangeInImage(nameRva, 1, pe.sizeOfImage))
            continue;
        auto nm = readCString(mProcess, base + nameRva, 260);
        if(!nm.second || nm.first != name)
            continue;
        uint16_t ord = 0;
        if(!readAt(mProcess, base + exp.AddressOfNameOrdinals + (uint64_t)i * sizeof(uint16_t), ord))
            return 0;
        uint32_t funcRva = 0;
        if(!readAt(mProcess, base + exp.AddressOfFunctions + (uint64_t)ord * sizeof(uint32_t), funcRva))
            return 0;
        if(funcRva >= pe.exportDir.VirtualAddress &&
           funcRva < pe.exportDir.VirtualAddress + pe.exportDir.Size)
            return 0; // forwarded export
        return base + funcRva;
    }
    return 0;
}

// Binary-search the remote module's .pdata for a RUNTIME_FUNCTION covering
// pc. Entries are read once per module and cached for the session.
// Indirect table entries (common in system DLLs: UnwindInfoAddress points
// back INTO .pdata at the real record) are resolved transparently.
RUNTIME_FUNCTION* GleamDebugger::findRuntimeFunction(uint64_t pc)
{
    if(!mProcess)
        return nullptr;
    ModuleInfo mod;
    if(!moduleOf(mProcess->hProcess, pc, mod))
        return nullptr;
    auto it = mPdataCache.find(mod.base);
    if(it == mPdataCache.end())
    {
        PdataCache cache;
        auto pe = readPeDirectories(mProcess, mod.base);
        if(pe.valid && pe.exceptionDir.Size &&
           rangeInImage(pe.exceptionDir.VirtualAddress, pe.exceptionDir.Size, mod.size))
        {
            cache.dirRva = pe.exceptionDir.VirtualAddress;
            cache.dirSize = pe.exceptionDir.Size;
            const uint32_t count = pe.exceptionDir.Size / (uint32_t)sizeof(RUNTIME_FUNCTION);
            cache.entries.resize(count);
            if(!mProcess->MemReadSafe(mod.base + pe.exceptionDir.VirtualAddress,
                                      cache.entries.data(), (uint64_t)count * sizeof(RUNTIME_FUNCTION)))
                cache.entries.clear();
        }
        it = mPdataCache.emplace(mod.base, std::move(cache)).first;
    }
    auto & cache = it->second;
    auto & entries = cache.entries;
    if(entries.empty())
        return nullptr;
    const uint32_t rva = (uint32_t)(pc - mod.base);
    size_t lo = 0, hi = entries.size();
    while(lo < hi)
    {
        size_t mid = lo + (hi - lo) / 2;
        if(rva < entries[mid].BeginAddress)
            hi = mid;
        else if(rva >= entries[mid].EndAddress)
            lo = mid + 1;
        else
        {
            RUNTIME_FUNCTION* found = &entries[mid];
            // Indirect entry: UnwindInfoAddress points into .pdata itself,
            // at the real record (system DLLs share unwind data this way).
            if(cache.dirSize && found->UnwindInfoAddress >= cache.dirRva &&
               found->UnwindInfoAddress < cache.dirRva + cache.dirSize)
            {
                const uint64_t off = found->UnwindInfoAddress - cache.dirRva;
                if(off % sizeof(RUNTIME_FUNCTION) == 0 &&
                   off / sizeof(RUNTIME_FUNCTION) < entries.size())
                    found = &entries[off / sizeof(RUNTIME_FUNCTION)];
            }
            return found;
        }
    }
    return nullptr;
}

void GleamDebugger::cmdFrames(uint32_t tid, uint64_t maxFrames)
{
    if(!mProcess)
        return;
    Thread* thread = nullptr;
    if(tid)
    {
        auto found = mProcess->threads.find(tid);
        if(found != mProcess->threads.end())
            thread = found->second.get();
        if(!thread)
        {
            printf("thread %u not found (try 'threads')\n", tid);
            fflush(stdout);
            return;
        }
    }
    else
        thread = currentThread();

    // All threads are already suspended while the debuggee sits in an event.
    CONTEXT context{};
    context.ContextFlags = CONTEXT_FULL;
    if(!GetThreadContext(thread->hThread, &context))
    {
        printf("GetThreadContext failed (%lu)\n", GetLastError());
        fflush(stdout);
        return;
    }

    // RtlVirtualUnwind (the unwinder dbghelp wraps) runs LOCALLY: it reads
    // the stack through the context and the code/unwind data through the
    // image base, all in OUR address space. Mirror both. The stack is frozen
    // while the debuggee sits in an event, so one mirror serves the walk.
    const uint64_t rsp0 = context.Rsp & ~0xFULL;
    MEMORY_BASIC_INFORMATION mbi{};
    if(!VirtualQueryEx(mProcess->hProcess, (LPCVOID)rsp0, &mbi, sizeof(mbi)))
    {
        printf("VirtualQueryEx failed for the stack (%lu)\n", GetLastError());
        fflush(stdout);
        return;
    }
    uint64_t regionEnd = (uint64_t)mbi.BaseAddress + mbi.RegionSize;
    if(regionEnd - rsp0 > 0x400000) // 4MB sanity cap
        regionEnd = rsp0 + 0x400000;
    std::vector<uint8_t> stackMirror(regionEnd - rsp0);
    if(!mProcess->MemReadSafe(rsp0, stackMirror.data(), stackMirror.size()))
    {
        printf("cannot read the stack of thread %u\n", thread->dwThreadId);
        fflush(stdout);
        return;
    }
    // Remote stack address -> mirror address (0 if outside).
    const uint64_t mirror0 = (uint64_t)stackMirror.data();
    const uint64_t mirrorSize = stackMirror.size();
    auto toLocal = [&](uint64_t remote) -> uint64_t
    {
        return remote >= rsp0 && remote < regionEnd ? mirror0 + (remote - rsp0) : 0;
    };
    // Mirror address -> remote stack address (0 if outside).
    auto toRemote = [&](uint64_t local) -> uint64_t
    {
        return local >= mirror0 && local < mirror0 + mirrorSize ? rsp0 + (local - mirror0) : 0;
    };

    ensureSymSession(); // best effort; frames degrade to module+offset
    const char* stopReason = nullptr;
    const char* source = "context"; // how the CURRENT rip was obtained
    uint64_t shown = 0;
    for(uint64_t n = 0; n < maxFrames; n++)
    {
        const uint64_t rip = context.Rip;
        const uint64_t rsp = context.Rsp;
        if(!rip)
            break; // clean bottom of the stack

        ModuleInfo mod;
        const std::string modname = moduleOf(mProcess->hProcess, rip, mod) ? mod.name : "?";
        std::string symField;
        if(mSymInitialized)
        {
            char buf[sizeof(SYMBOL_INFO) + MAX_SYM_NAME];
            memset(buf, 0, sizeof(buf));
            auto si = (SYMBOL_INFO*)buf;
            si->SizeOfStruct = sizeof(SYMBOL_INFO);
            si->MaxNameLen = MAX_SYM_NAME;
            uint64_t disp = 0;
            if(SymFromAddr(mProcess->hProcess, rip, &disp, si))
            {
                char tmp[MAX_SYM_NAME + 48];
                sprintf_s(tmp, " sym=%s+0x%llX", si->Name, (unsigned long long)disp);
                symField = tmp;
            }
        }
        printf("frame #%llu rip=0x%llX rsp=0x%llX module=%s%s source=%s\n",
               (unsigned long long)n, (unsigned long long)rip, (unsigned long long)rsp,
               modname.c_str(), symField.c_str(), source);
        fflush(stdout);
        shown++;

        const uint64_t base = moduleBaseOf(rip);
        RUNTIME_FUNCTION* rf = base ? findRuntimeFunction(rip) : nullptr;
        if(!rf)
        {
            // No unwind info: try the x64 ABI leaf rule, rip = [rsp],
            // rsp += 8. Honest labels: only a verified module without a
            // record is a "leaf"; anything else is "untrusted".
            source = base ? "leaf" : "untrusted";
            uint64_t ret = 0;
            if(!mProcess->MemReadSafe(rsp, &ret, sizeof(ret)))
            {
                stopReason = base ? "no unwind data and unreadable stack"
                                  : "pc not in any module (shellcode?) - cannot unwind";
                break;
            }
            context.Rip = ret;
            context.Rsp = rsp + 8;
        }
        else
        {
            source = "unwind";
            // Mirror the RVA span covering the function's code (prologue /
            // epilogue byte checks) and its UNWIND_INFO, then unwind with a
            // fake image base mapping the RVA span onto that buffer.
            // Chained unwind info (UNW_FLAG_CHAININFO, common in CRT and
            // system DLLs): each record's data ends with the next
            // RUNTIME_FUNCTION; walk the whole chain and mirror every piece
            // so RtlVirtualUnwind can follow it inside the buffer.
            struct SpanPiece { uint64_t rva; uint64_t size; };
            SpanPiece pieces[20];
            size_t pieceCount = 0;
            pieces[pieceCount++] = { rf->BeginAddress, rf->EndAddress - rf->BeginAddress };
            uint32_t uwRva = rf->UnwindInfoAddress;
            for(int chain = 0; ; chain++)
            {
                if(chain == 16)
                {
                    stopReason = "unwind chain too long";
                    break;
                }
                uint8_t hdr[4];
                if(!mProcess->MemReadSafe(base + uwRva, hdr, sizeof(hdr)))
                {
                    stopReason = "unwind info unreadable";
                    break;
                }
                // UNWIND_INFO: byte0 = version:3|flags:5, byte1 = prolog
                // size, byte2 = unwind code count, byte3 = frame reg/offset.
                const uint8_t uwFlags = hdr[0] >> 3;
                size_t uwSize = 4 + (size_t)hdr[2] * 2; // header + codes
                uwSize = (uwSize + 3) & ~(size_t)3;       // 4-byte aligned
                if(uwFlags & 3) // EHANDLER/UHANDLER: handler RVA follows
                    uwSize += 4;
                if(uwFlags & 4) // CHAININFO: a RUNTIME_FUNCTION follows
                {
                    if(pieceCount >= 20)
                    {
                        stopReason = "unwind chain too long";
                        break;
                    }
                    uwSize += sizeof(RUNTIME_FUNCTION);
                    pieces[pieceCount++] = { uwRva, uwSize };
                    RUNTIME_FUNCTION next{};
                    if(!mProcess->MemReadSafe(base + uwRva + uwSize - sizeof(RUNTIME_FUNCTION),
                                              &next, sizeof(next)) ||
                       !next.UnwindInfoAddress)
                    {
                        stopReason = "unwind chain broken";
                        break;
                    }
                    uwRva = next.UnwindInfoAddress;
                    continue;
                }
                pieces[pieceCount++] = { uwRva, uwSize };
                break; // end of the chain
            }
            if(stopReason)
                break;
            uint64_t spanStart = pieces[0].rva, spanEnd = 0;
            for(size_t i = 0; i < pieceCount; i++)
            {
                spanStart = (std::min)(spanStart, pieces[i].rva);
                spanEnd = (std::max)(spanEnd, pieces[i].rva + pieces[i].size);
            }
            if(spanEnd - spanStart > 0x2000000) // sanity cap (32MB)
            {
                stopReason = "unwind span too large";
                break;
            }
            std::vector<uint8_t> span(spanEnd - spanStart, 0);
            for(size_t i = 0; i < pieceCount && !stopReason; i++)
            {
                if(!mProcess->MemReadSafe(base + pieces[i].rva,
                                          span.data() + (pieces[i].rva - spanStart), pieces[i].size))
                    stopReason = "unwind data unreadable";
            }
            if(stopReason)
                break;
            RUNTIME_FUNCTION localRf = *rf;
            const uint64_t fakeBase = (uint64_t)span.data() - spanStart;

            // The context's stack pointers must point into the mirror. Rsp
            // is mandatory; Rbp only gets dereferenced when the function
            // uses a frame register, so map it when possible and pass it
            // through otherwise (a bad one is caught by the SEH wrapper).
            CONTEXT uw = context;
            uw.Rsp = toLocal(context.Rsp);
            if(!uw.Rsp)
            {
                stopReason = "stack pointer outside the mirrored region";
                break;
            }
            if(uint64_t localRbp = toLocal(context.Rbp))
                uw.Rbp = localRbp;
            const uint32_t sehCode = virtualUnwindSeh(fakeBase, rip - base + fakeBase,
                                                      &localRf, &uw);
            if(sehCode)
            {
                stopReason = "unwind faulted (corrupt unwind data?)";
                break;
            }
            // Copy the unwound registers back; pointers into the mirror
            // become remote addresses again, values stay as they are.
            context = uw;
            context.Rsp = toRemote(uw.Rsp);
            if(uw.Rbp == 0 || toRemote(uw.Rbp))
                context.Rbp = toRemote(uw.Rbp);
            if(!context.Rsp)
                break; // walked off the mirrored stack: treat as the bottom
        }

        // Loop guards against a corrupted stack.
        if(context.Rip == rip)
        {
            stopReason = "pc did not change (corrupt stack?)";
            break;
        }
        if(context.Rsp && context.Rsp < rsp)
        {
            stopReason = "stack pointer decreased (corrupt stack?)";
            break;
        }
    }
    if(stopReason)
        printf("walk stopped at frame #%llu: %s (use stackscan for heuristic candidates)\n",
               (unsigned long long)shown, stopReason);
    fflush(stdout);
}

GleamDebugger::CmdResult GleamDebugger::trySymbolCommand(const std::vector<std::string> & args)
{
    const std::string & cmd = args[0];
    uint64_t a = 0;

    if(cmd == "frames" && args.size() <= 3)
    {
        uint32_t tid = 0;
        uint64_t maxFrames = 64;
        bool ok = true;
        if(args.size() >= 2)
        {
            // tid as printed by "threads" (decimal); 0x-prefixed hex works too
            char* end = nullptr;
            unsigned long t = strtoul(args[1].c_str(), &end, 0);
            ok = end && *end == '\0' && end != args[1].c_str();
            tid = (uint32_t)t;
        }
        if(ok && args.size() == 3)
            ok = parseHex(args[2], maxFrames);
        if(ok && maxFrames >= 1 && maxFrames <= 256)
            cmdFrames(tid, maxFrames);
        else
        {
            printf("usage: frames [tid] [maxframes]\n");
            fflush(stdout);
        }
        return CmdResult::Handled;
    }

    if(cmd == "imports" && args.size() <= 2)
    {
        cmdImports(args.size() == 2 ? args[1] : std::string());
        return CmdResult::Handled;
    }
    if(cmd == "moduleinfo" && args.size() == 2)
    {
        cmdModuleInfo(args[1]);
        return CmdResult::Handled;
    }
    if(cmd == "sections" && args.size() == 2)
    {
        cmdSections(args[1]);
        return CmdResult::Handled;
    }
    if(cmd == "sympath" && args.size() <= 2)
    {
        // A search path may contain spaces; the tokenizer split them, so
        // rejoin rather than silently using only the first fragment.
        cmdSymPath(args.size() == 2 ? args[1] : std::string(), args.size() == 2);
        return CmdResult::Handled;
    }
    if(cmd == "sympath" && args.size() > 2)
    {
        std::string joined = args[1];
        for(size_t i = 2; i < args.size(); i++)
            joined += ' ' + args[i];
        cmdSymPath(joined, true);
        return CmdResult::Handled;
    }
    if(cmd == "symload" && args.size() == 2)
    {
        cmdSymLoad(args[1]);
        return CmdResult::Handled;
    }
    if(cmd == "symreload" && args.size() <= 2)
    {
        cmdSymReload(args.size() == 2 ? args[1] : std::string());
        return CmdResult::Handled;
    }
    if(cmd == "exports" && (args.size() == 2 || args.size() == 3))
    {
        cmdExports(args[1], args.size() == 3 ? args[2] : std::string());
        return CmdResult::Handled;
    }
    if(cmd == "selftest" && (args.size() == 3 || args.size() == 4) && args[1] == "failapi")
    {
        // Arm an engine fault-injection hook (see GleeBug::Debugger::mTestHook*
        // and the failapi* functions at the top of this file). Used by the W16
        // gate scenarios to prove the loop's API-failure paths are handled.
        const std::string & which = args[2];
        if(which == "off")
        {
            // Disarm everything (pairs with the persistent "always" variant).
            mTestHookWaitForDebugEvent = nullptr;
            mTestHookContinueDebugEvent = nullptr;
            mTestHookResumeThread = nullptr;
            printf("selftest failapi off\n");
            fflush(stdout);
            return CmdResult::Handled;
        }
        if(which == "wait" && args.size() == 3)
            mTestHookWaitForDebugEvent = &failapiWait;
        else if(which == "continue" && args.size() == 3)
            mTestHookContinueDebugEvent = &failapiContinueNormal;
        else if(which == "replylater" && args.size() == 3)
            mTestHookContinueDebugEvent = &failapiContinueReplyLater;
        else if(which == "resume" && args.size() == 3)
            mTestHookResumeThread = &failapiResume;
        else if(which == "resume" && args.size() == 4 && args[3] == "always")
            mTestHookResumeThread = &failapiResumeAlways;
        else if(which == "terminate" && args.size() == 3)
            mFailNextTerminate = true;    // TerminateThread on the stub thread
        else if(which == "stubresume" && args.size() == 3)
            mFailNextStubResume = true;   // ResumeThread of a fresh stub thread
        else if(which == "vfree" && args.size() == 3)
            mFailNextVfree = true;        // VirtualFreeEx of the stub page
        else
        {
            printf("usage: selftest failapi wait|continue|replylater|resume [always]|terminate|stubresume|vfree|off\n");
            fflush(stdout);
            return CmdResult::Handled;
        }
        printf("selftest failapi armed %s%s\n", which.c_str(),
               args.size() == 4 ? " always" : "");
        fflush(stdout);
        return CmdResult::Handled;
    }
    if(cmd == "selftest")
    {
        // Unit boundaries for rangeInImage (no malformed-PE samples needed).
        int pass = 0, total = 0;
        auto T = [&](bool got, bool want) { total++; if(got == want) pass++; };
        T(rangeInImage(0, 1, 100), true);                    // start of image
        T(rangeInImage(99, 1, 100), true);                   // exactly to the end
        T(rangeInImage(99, 2, 100), false);                  // one byte past the end
        T(rangeInImage(100, 1, 100), false);                 // rva at the end
        T(rangeInImage(0, 0, 100), true);                    // zero-size range
        T(rangeInImage(50, 0xFFFFFFFFFFFFFFFFULL, 100), false); // size overflow
        T(rangeInImage(0xFFFFFFFFFFFFFFFFULL, 1, 100), false);  // rva overflow
        T(rangeInImage(92, 8, 100), true);                   // last complete element
        T(rangeInImage(96, 8, 100), false);                  // element past the end
        T(rangeInImage(97, 8, 100), false);                  // incomplete element
        T(rangeInImage(0, 100, 0), false);                   // zero-size image
        T(rangeInImage(60, 40, 100), true);                  // full tail range
        printf("selftest rangeInImage %d/%d ok\n", pass, total);
        // Exception disposition policy: full table-driven matrix (x64dbg
        // semantics; see decideExPolicy's rules comment).
        pass = total = 0;
        auto D = [](bool fc, bool hit, int bo, int hb, bool boe)
        {
            ExPolicyInput in{ fc, hit, bo, hb, boe };
            return decideExPolicy(in);
        };
        auto P = [](ExPolicyOutput o, bool pause, bool swallow)
        {
            return o.pause == pause && o.swallow == swallow;
        };
        // filter miss
        T(P(D(true, false, 2, 0, true), true, false), true);   // first + breakon on  -> pause/pass
        T(P(D(true, false, 2, 0, false), false, false), true); // first + breakon off -> silent pass
        T(P(D(false, false, 2, 0, true), true, true), true);   // second               -> pause/swallow
        T(P(D(false, false, 2, 0, false), true, true), true);  // second (breakon off) -> pause/swallow
        // filter never+pass
        T(P(D(true, true, 2, 0, true), false, false), true);   // first  -> no-pause pass
        T(P(D(false, true, 2, 0, true), false, false), true);  // second -> explicit pass (may die)
        // filter never+swallow
        T(P(D(true, true, 2, 1, true), false, true), true);    // first  -> no-pause swallow
        T(P(D(false, true, 2, 1, true), true, true), true);    // second -> pause/swallow
        // filter break=first, pass
        T(P(D(true, true, 0, 0, true), true, false), true);    // hit    -> pause/pass
        T(P(D(false, true, 0, 0, true), true, true), true);    // miss   -> pause/swallow (NOT silent pass)
        // filter break=first, swallow
        T(P(D(true, true, 0, 1, true), true, true), true);     // hit    -> pause/swallow
        T(P(D(false, true, 0, 1, true), true, true), true);    // miss   -> pause/swallow
        // filter break=second, pass
        T(P(D(false, true, 1, 0, true), true, true), true);    // hit    -> pause/swallow
        T(P(D(true, true, 1, 0, true), false, false), true);   // miss   -> no-pause pass
        // filter break=second, swallow
        T(P(D(false, true, 1, 1, true), true, true), true);    // hit    -> pause/swallow
        T(P(D(true, true, 1, 1, true), false, true), true);    // miss   -> no-pause swallow
        printf("selftest excpolicy %d/%d ok\n", pass, total);
        // Symbol disambiguation under incremental linking (zombie records):
        // the pure decision over candidate address sets.
        {
            std::unordered_set<uint64_t> ilt{ 0x2000, 0x3000 };
            int okc = 0;
            if(pickLiveSymbolCandidate({ 0x1000 }, ilt) == 0)
                okc++; // a single record is all the information there is
            if(pickLiveSymbolCandidate({ 0x1000, 0x2000 }, ilt) == 1)
                okc++; // zombie + live: pick the ILT-backed body
            if(pickLiveSymbolCandidate({ 0x1000, 0x4000 }, ilt) < 0)
                okc++; // no ILT-backed body: refuse
            if(pickLiveSymbolCandidate({ 0x2000, 0x3000 }, ilt) < 0)
                okc++; // two ILT-backed bodies: refuse (cannot disambiguate)
            printf("selftest symdis %d/4 ok\n", okc);
            fflush(stdout);
        }
        // Module identity check (needs Late.dll + NoExp.dll loaded: dll4).
        {
            uint64_t lateBase = 0, noexpBase = 0;
            if(moduleBaseByName("late", lateBase) && moduleBaseByName("noexp", noexpBase))
            {
                wchar_t wpath[MAX_PATH * 2] = L"";
                if(GetModuleFileNameExW(mProcess->hProcess, (HMODULE)lateBase, wpath, ARRAYSIZE(wpath)))
                {
                    int okc = 0;
                    if(verifyModuleIdentity(lateBase, wpath))
                        okc++; // the real module matches its file
                    if(!verifyModuleIdentity(noexpBase, wpath))
                        okc++; // a decoy must NOT match Late's file
                    printf("selftest modid %d/2 ok\n", okc);
                    fflush(stdout);
                }
            }
        }
        fflush(stdout);
        return CmdResult::Handled;
    }
    if(cmd == "sym" && args.size() == 2 && parseAddress(args[1], a))
    {
        cmdSym(a);
        return CmdResult::Handled;
    }
    return CmdResult::NotMine;
}
