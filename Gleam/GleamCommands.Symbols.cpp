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

    // Read a NUL-terminated string from the debuggee (capped).
    std::string readCString(Process* process, uint64_t addr, size_t cap = 260)
    {
        std::string result;
        char buf[64];
        while(result.size() < cap)
        {
            size_t chunk = (std::min)(sizeof(buf), cap - result.size());
            if(!process->MemReadSafe(addr + result.size(), buf, chunk))
                break;
            size_t i = 0;
            for(; i < chunk && buf[i]; i++)
                result += buf[i];
            if(i < chunk) // hit the NUL
                break;
            if(chunk == 0)
                break;
        }
        return result;
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
            info.importDir = opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
            info.delayImportDir = opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
        }
        else if(magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC)
        {
            IMAGE_OPTIONAL_HEADER32 opt;
            if(!readAt(process, optAddr, opt))
                return info;
            info.pe64 = false;
            info.importDir = opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
            info.delayImportDir = opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
        }
        else
            return info;
        info.valid = true;
        return info;
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

bool GleamDebugger::ensureSymSession()
{
    if(mSymInitialized)
        return true;
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
        mod.base = (uint64_t)mProcess->createProcessInfo.lpBaseOfImage;
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

    // Bound imports.
    if(pe.importDir.VirtualAddress)
    {
        for(uint32_t i = 0;; i++)
        {
            IMAGE_IMPORT_DESCRIPTOR desc;
            if(!readAt(mProcess, mod.base + pe.importDir.VirtualAddress + i * sizeof(desc), desc))
                break;
            if(!desc.Name && !desc.FirstThunk)
                break;
            auto dllName = readCString(mProcess, mod.base + desc.Name);
            printf("%s:\n", dllName.c_str());
            groups++;
            for(uint32_t t = 0;; t++)
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

    // Delay-loaded imports.
    if(pe.delayImportDir.VirtualAddress)
    {
        for(uint32_t i = 0;; i++)
        {
            ImgDelayDescr desc{};
            if(!readAt(mProcess, mod.base + pe.delayImportDir.VirtualAddress + i * sizeof(desc), desc))
                break;
            if(!desc.rvaDLLName)
                break;
            // dlattrRva: when set, the fields are RVAs; otherwise absolute VAs.
            const bool rva = (desc.grAttrs & 1) != 0;
            uint64_t nameAddr = rva ? mod.base + (uint64_t)desc.rvaDLLName : (uint64_t)desc.rvaDLLName;
            uint64_t iatAddr = rva ? mod.base + (uint64_t)desc.rvaIAT : (uint64_t)desc.rvaIAT;
            auto dllName = readCString(mProcess, nameAddr);
            printf("%s (delay):\n", dllName.c_str());
            groups++;
            for(uint32_t t = 0;; t++)
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

GleamDebugger::CmdResult GleamDebugger::trySymbolCommand(const std::vector<std::string> & args)
{
    const std::string & cmd = args[0];

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
    return CmdResult::NotMine;
}
