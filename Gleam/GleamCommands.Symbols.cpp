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
            std::string n(name);
            // strip .dll for comparison
            std::string stripped = n;
            if(stripped.size() > 4 && _stricmp(stripped.c_str() + stripped.size() - 4, ".dll") == 0)
                stripped.resize(stripped.size() - 4);
            std::string wanted = nameOrBase;
            if(wanted.size() > 4 && _stricmp(wanted.c_str() + wanted.size() - 4, ".dll") == 0)
                wanted.resize(wanted.size() - 4);
            if(_stricmp(n.c_str(), nameOrBase.c_str()) == 0 || _stricmp(stripped.c_str(), wanted.c_str()) == 0)
            {
                out.base = (uint64_t)(uintptr_t)mi.lpBaseOfDll;
                out.size = mi.SizeOfImage;
                out.name = n;
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
// leaf function (no unwind record present) and from infrastructure failure.
std::pair<GleamDebugger::UnwindStatus, uint64_t> GleamDebugger::stackWalkReturn(HANDLE hThread)
{
    if(!mProcess || !ensureSymSession())
        return { UnwindStatus::Failed, 0 };
    CONTEXT context{};
    context.ContextFlags = CONTEXT_FULL;
    if(!GetThreadContext(hThread, &context))
        return { UnwindStatus::Failed, 0 };
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
    // No caller from the walk. If an unwind record exists for the current
    // PC, the function is NOT a leaf - the unwind itself failed. Otherwise
    // it is a genuine leaf and [rsp] holds the return address (x64 ABI).
    if(SymFunctionTableAccess64(mProcess->hProcess, context.Rip))
        return { UnwindStatus::Failed, 0 };
    return { UnwindStatus::Leaf, 0 };
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

bool GleamDebugger::parseAddress(const std::string & s, uint64_t & out)
{
    if(parseHex(s, out))
        return true;
    // "module!symbol" form, resolved through the dbghelp session.
    if(s.find('!') == std::string::npos || !mProcess || !ensureSymSession())
        return false;
    char buf[sizeof(SYMBOL_INFO) + MAX_SYM_NAME];
    memset(buf, 0, sizeof(buf));
    auto si = (SYMBOL_INFO*)buf;
    si->SizeOfStruct = sizeof(SYMBOL_INFO);
    si->MaxNameLen = MAX_SYM_NAME;
    if(SymFromName(mProcess->hProcess, s.c_str(), si))
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
        // Loader-reported size for the main module: the record must match the
        // main image base from the debug event. On any mismatch or failure we
        // do NOT fall back to the (target-modifiable) PE header value.
        HMODULE first = nullptr;
        DWORD needed = 0;
        MODULEINFO mi{};
        if(EnumProcessModules(mProcess->hProcess, &first, sizeof(first), &needed) &&
           GetModuleInformation(mProcess->hProcess, first, &mi, sizeof(mi)) &&
           (uint64_t)(uintptr_t)mi.lpBaseOfDll == (uint64_t)mProcess->createProcessInfo.lpBaseOfImage)
        {
            mod.base = (uint64_t)(uintptr_t)mi.lpBaseOfDll;
            mod.size = mi.SizeOfImage;
        }
        else
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

GleamDebugger::CmdResult GleamDebugger::trySymbolCommand(const std::vector<std::string> & args)
{
    const std::string & cmd = args[0];
    uint64_t a = 0;

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
