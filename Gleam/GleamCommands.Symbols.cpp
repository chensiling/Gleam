// Symbol commands: import table enumeration (descriptor walk + dbghelp
// reverse resolution) and export enumeration (dbghelp SymEnumSymbols).

#include "GleamDebugger.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <psapi.h>
#include <dbghelp.h>
#include <delayimp.h>

using namespace GleeBug;

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

bool GleamDebugger::resolveModuleSymbol(const std::string & modSym, uint64_t & out)
{
    if(!mProcess || !ensureSymSession())
        return false;
    char buf[sizeof(SYMBOL_INFO) + MAX_SYM_NAME];
    memset(buf, 0, sizeof(buf));
    auto si = (SYMBOL_INFO*)buf;
    si->SizeOfStruct = sizeof(SYMBOL_INFO);
    si->MaxNameLen = MAX_SYM_NAME;
    if(SymFromName(mProcess->hProcess, modSym.c_str(), si))
    {
        out = si->Address;
        return true;
    }
    return false;
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
    if(mSymInitialized && mProcess)
    {
        SymCleanup(mProcess->hProcess);
        mSymInitialized = false;
    }
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
// Resolve a PDB-only symbol by explicitly loading the module's symbols
// from its file on disk. The invade-based dbghelp session depends on the
// loader list, which is not yet linked during the DLL load event - this
// path has no such dependency. Returns 0 when unresolvable.
uint64_t GleamDebugger::resolvePdbSymbol(uint64_t moduleBase, const wchar_t* imagePath, const std::string & symbol)
{
    if(!imagePath || !*imagePath || !ensureSymSession())
        return 0;
    if(!mSymLoadedBases.count(moduleBase))
    {
        if(!SymLoadModuleExW(mProcess->hProcess, NULL, imagePath, NULL,
                             moduleBase, 0 /* size from image */, NULL, 0))
            return 0;
        mSymLoadedBases.insert(moduleBase);
    }
    char buf[sizeof(SYMBOL_INFO) + MAX_SYM_NAME];
    memset(buf, 0, sizeof(buf));
    auto si = (SYMBOL_INFO*)buf;
    si->SizeOfStruct = sizeof(SYMBOL_INFO);
    si->MaxNameLen = MAX_SYM_NAME;
    // Wide path -> UTF-8 for the module name in "module!symbol".
    char narrow[MAX_PATH * 2] = "";
    WideCharToMultiByte(CP_UTF8, 0, imagePath, -1, narrow, sizeof(narrow), nullptr, nullptr);
    std::string modSym = normalizeModuleName(narrow) + "!" + symbol;
    if(SymFromName(mProcess->hProcess, modSym.c_str(), si))
        return si->Address;
    return 0;
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
    if(cmd == "exports" && (args.size() == 2 || args.size() == 3))
    {
        cmdExports(args[1], args.size() == 3 ? args[2] : std::string());
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
