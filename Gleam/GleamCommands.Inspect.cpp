// Inspection commands: registers, memory, disassembly, maps, modules,
// memory search, exception info, backtrace.

#include "GleamDebugger.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <psapi.h>

#include <GleeBug/Zydis/Zydis.h>

using namespace GleeBug;

#ifdef _WIN64
static const ZydisMachineMode kMachineMode = ZYDIS_MACHINE_MODE_LONG_64;
#else
static const ZydisMachineMode kMachineMode = ZYDIS_MACHINE_MODE_LONG_COMPAT_32;
#endif

// Register name -> enum mapping (x64 64-bit registers only).
bool GleamDebugger::registerByName(const std::string & name, RegId & reg)
{
    static const std::pair<const char*, RegId> table[] = {
        { "rax", RegId::RAX }, { "rbx", RegId::RBX }, { "rcx", RegId::RCX }, { "rdx", RegId::RDX },
        { "rsi", RegId::RSI }, { "rdi", RegId::RDI }, { "rbp", RegId::RBP }, { "rsp", RegId::RSP },
        { "rip", RegId::RIP },
        { "r8", RegId::R8 }, { "r9", RegId::R9 }, { "r10", RegId::R10 }, { "r11", RegId::R11 },
        { "r12", RegId::R12 }, { "r13", RegId::R13 }, { "r14", RegId::R14 }, { "r15", RegId::R15 },
    };
    for(const auto & entry : table)
    {
        if(_stricmp(name.c_str(), entry.first) == 0)
        {
            reg = entry.second;
            return true;
        }
    }
    return false;
}

void GleamDebugger::cmdRegs()
{
    Registers r(currentThread()->hThread);
    printf("RAX=%016llX RBX=%016llX RCX=%016llX RDX=%016llX\n", r.Rax(), r.Rbx(), r.Rcx(), r.Rdx());
    printf("RSI=%016llX RDI=%016llX RBP=%016llX RSP=%016llX\n", r.Rsi(), r.Rdi(), r.Rbp(), r.Rsp());
    printf("R8 =%016llX R9 =%016llX R10=%016llX R11=%016llX\n", r.R8(), r.R9(), r.R10(), r.R11());
    printf("R12=%016llX R13=%016llX R14=%016llX R15=%016llX\n", r.R12(), r.R13(), r.R14(), r.R15());
    printf("RIP=%016llX EFLAGS=%08X\n", r.Rip(), r.Eflags());
    fflush(stdout);
}

void GleamDebugger::cmdRead(uint64_t addr, uint64_t size)
{
    if(size == 0 || size > 0x10000)
    {
        printf("invalid size (1..65536)\n");
        fflush(stdout);
        return;
    }
    std::vector<uint8_t> buf((size_t)size);
    if(!mProcess->MemReadSafe(addr, buf.data(), size))
    {
        printf("read failed at 0x%llX\n", addr);
        fflush(stdout);
        return;
    }
    for(uint64_t i = 0; i < size; i += 16)
    {
        printf("%016llX  ", addr + i);
        for(uint64_t j = i; j < i + 16 && j < size; j++)
            printf("%02X ", buf[(size_t)j]);
        printf("\n");
    }
    fflush(stdout);
}

void GleamDebugger::cmdWrite(uint64_t addr, const std::vector<uint8_t> & bytes)
{
    if(bytes.empty())
    {
        printf("nothing to write\n");
        fflush(stdout);
        return;
    }
    if(mProcess->MemWriteSafe(addr, bytes.data(), bytes.size()))
        printf("wrote %zu bytes at 0x%llX\n", bytes.size(), addr);
    else
        printf("write failed at 0x%llX\n", addr);
    fflush(stdout);
}

void GleamDebugger::cmdThreads()
{
    for(const auto & kv : mProcess->threads)
    {
        printf("thread %u (0x%X)%s%s\n",
               kv.first,
               kv.first,
               kv.first == mDebugEvent.dwThreadId ? " [event]" : "",
               kv.first == mSelectedThreadId ? " [selected]" : "");
    }
    fflush(stdout);
}

void GleamDebugger::cmdDisasm(uint64_t addr, uint64_t count)
{
    for(uint64_t i = 0; i < count; i++)
    {
        uint8_t data[16];
        if(!mProcess->MemReadSafe(addr, data, sizeof(data)))
        {
            printf("read failed at 0x%llX\n", addr);
            break;
        }
        ZydisDisassembledInstruction instruction;
        if(!ZYAN_SUCCESS(ZydisDisassembleIntel(kMachineMode, addr, data, sizeof(data), &instruction)))
        {
            printf("%016llX  <invalid>\n", addr);
            break;
        }
        printf("%016llX  %s\n", addr, instruction.text);
        addr += instruction.info.length;
    }
    fflush(stdout);
}

static const char* protectText(DWORD protect)
{
    switch(protect & 0xFF)
    {
    case PAGE_NOACCESS: return "---";
    case PAGE_READONLY: return "r--";
    case PAGE_READWRITE: return "rw-";
    case PAGE_WRITECOPY: return "cow";
    case PAGE_EXECUTE: return "--x";
    case PAGE_EXECUTE_READ: return "r-x";
    case PAGE_EXECUTE_READWRITE: return "rwx";
    case PAGE_EXECUTE_WRITECOPY: return "cox";
    default: return "???";
    }
}

void GleamDebugger::cmdMaps()
{
    uint64_t addr = 0;
    size_t count = 0;
    for(;;)
    {
        MEMORY_BASIC_INFORMATION mbi;
        if(!VirtualQueryEx(mProcess->hProcess, (LPCVOID)addr, &mbi, sizeof(mbi)))
            break;
        uint64_t base = (uint64_t)mbi.BaseAddress;
        uint64_t size = (uint64_t)mbi.RegionSize;
        if(mbi.State == MEM_COMMIT)
        {
            const char* typeText =
                mbi.Type == MEM_IMAGE ? "image" :
                mbi.Type == MEM_MAPPED ? "mapped" : "private";
            printf("%016llX-%016llX  %-10s %s %s%s\n",
                   base,
                   base + size,
                   typeText,
                   protectText(mbi.Protect),
                   (mbi.Protect & PAGE_GUARD) ? " guard" : "",
                   (mbi.Protect & PAGE_NOCACHE) ? " nocache" : "");
            count++;
        }
        uint64_t next = base + size;
        if(next <= addr) // overflow or zero-size region
            break;
        addr = next;
    }
    printf("%zu committed regions\n", count);
    fflush(stdout);
}

void GleamDebugger::cmdModules()
{
    HMODULE modules[1024];
    DWORD needed = 0;
    if(!EnumProcessModules(mProcess->hProcess, modules, sizeof(modules), &needed))
    {
        printf("EnumProcessModules failed (%lu)\n", GetLastError());
        fflush(stdout);
        return;
    }
    DWORD count = (DWORD)(std::min)(needed / sizeof(HMODULE), sizeof(modules) / sizeof(HMODULE));
    for(DWORD i = 0; i < count; i++)
    {
        MODULEINFO mi;
        char name[MAX_PATH] = "";
        if(!GetModuleInformation(mProcess->hProcess, modules[i], &mi, sizeof(mi)))
            continue;
        GetModuleBaseNameA(mProcess->hProcess, modules[i], name, sizeof(name));
        printf("%016llX  %08lX  %s\n",
               (unsigned long long)(uintptr_t)mi.lpBaseOfDll,
               (unsigned long)mi.SizeOfImage,
               name);
    }
    fflush(stdout);
}

void GleamDebugger::cmdFind(uint64_t addr, uint64_t size, const std::string & pattern)
{
    auto found = mProcess->MemFindPattern(addr, (size_t)size, pattern);
    if(found)
        printf("found at 0x%llX\n", (unsigned long long)found);
    else
        printf("not found\n");
    fflush(stdout);
}

void GleamDebugger::cmdFindString(uint64_t addr, uint64_t size, const std::string & text, bool utf16)
{
    if(text.empty())
    {
        printf("empty string\n");
        fflush(stdout);
        return;
    }
    std::vector<uint8_t> bytes;
    bytes.reserve(text.size() * (utf16 ? 2 : 1));
    for(char c : text)
    {
        bytes.push_back((uint8_t)c);
        if(utf16)
            bytes.push_back(0);
    }
    auto found = mProcess->MemFindPattern(addr, (size_t)size, bytes.data(), bytes.size());
    if(found)
        printf("found at 0x%llX\n", (unsigned long long)found);
    else
        printf("not found\n");
    fflush(stdout);
}

void GleamDebugger::cmdStackScan(uint64_t count)
{
    // x64dbg-style stack view: scan qwords from rsp, annotate values that
    // point into executable committed memory (likely return addresses).
    if(count == 0 || count > 0x1000)
        count = 32;
    Registers r(currentThread()->hThread);
    uint64_t rsp = r.Gsp();
    ensureSymSession();
    for(uint64_t i = 0; i < count; i++)
    {
        uint64_t value = 0;
        if(!mProcess->MemReadSafe(rsp + i * sizeof(value), &value, sizeof(value)))
            break;
        MEMORY_BASIC_INFORMATION mbi;
        if(!VirtualQueryEx(mProcess->hProcess, (LPCVOID)value, &mbi, sizeof(mbi)))
            continue;
        if(mbi.State != MEM_COMMIT)
            continue;
        const DWORD exec = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        if(!(mbi.Protect & exec))
            continue;
        auto name = symNameByAddr(value);
        printf("rsp+0x%02llX  0x%016llX  %s\n",
               (unsigned long long)(i * sizeof(value)),
               value,
               name.empty() ? "" : name.c_str());
    }
    fflush(stdout);
}

void GleamDebugger::cmdPatch(uint64_t addr, const std::vector<uint8_t> & bytes)
{
    if(bytes.empty())
    {
        printf("nothing to patch\n");
        fflush(stdout);
        return;
    }
    std::vector<uint8_t> original(bytes.size());
    if(!mProcess->MemReadSafe(addr, original.data(), original.size()))
    {
        printf("read failed at 0x%llX (cannot record original bytes)\n", addr);
        fflush(stdout);
        return;
    }
    if(!mProcess->MemWriteSafe(addr, bytes.data(), bytes.size()))
    {
        printf("write failed at 0x%llX\n", addr);
        fflush(stdout);
        return;
    }
    mPatches[addr] = original;
    printf("patched 0x%llX (%zu bytes)\n", addr, bytes.size());
    fflush(stdout);
}

void GleamDebugger::cmdPatchList()
{
    if(mPatches.empty())
    {
        printf("no patches\n");
        fflush(stdout);
        return;
    }
    for(const auto & kv : mPatches)
    {
        printf("0x%llX  %zu bytes, original:", kv.first, kv.second.size());
        for(auto b : kv.second)
            printf(" %02X", b);
        printf("\n");
    }
    fflush(stdout);
}

void GleamDebugger::cmdRestore(uint64_t addr)
{
    auto it = mPatches.find(addr);
    if(it == mPatches.end())
    {
        printf("no patch recorded at 0x%llX\n", addr);
        fflush(stdout);
        return;
    }
    if(mProcess->MemWriteSafe(addr, it->second.data(), it->second.size()))
    {
        printf("restored 0x%llX\n", addr);
        mPatches.erase(it);
    }
    else
        printf("restore failed at 0x%llX\n", addr);
    fflush(stdout);
}

void GleamDebugger::cmdExceptionInfo()
{
    if(!mLastExceptionValid)
    {
        printf("no exception recorded\n");
        fflush(stdout);
        return;
    }
    printf("code=0x%08lX address=0x%p flags=0x%lX %s\n",
           mLastException.ExceptionCode,
           mLastException.ExceptionAddress,
           mLastException.ExceptionFlags,
           mLastExceptionFirstChance ? "(first chance)" : "(second chance)");
    for(DWORD i = 0; i < mLastException.NumberParameters; i++)
        printf("  parameter[%lu] = 0x%p\n", i, (void*)mLastException.ExceptionInformation[i]);
    fflush(stdout);
}

void GleamDebugger::cmdBacktrace()
{
    // Naive rbp-chain walk; functions without frame pointers are invisible.
    Registers r(currentThread()->hThread);
    uint64_t rbp = r.Gbp();
    printf("#0  0x%016llX (rip)\n", r.Gip());
    for(int frame = 1; frame <= 32; frame++)
    {
        uint64_t callerRbp = 0, retAddr = 0;
        if(!mProcess->MemReadSafe(rbp, &callerRbp, sizeof(callerRbp)) ||
           !mProcess->MemReadSafe(rbp + sizeof(rbp), &retAddr, sizeof(retAddr)))
            break;
        if(callerRbp <= rbp) // chain must grow upwards
            break;
        printf("#%d  0x%016llX (rbp=0x%llX)\n", frame, retAddr, rbp);
        rbp = callerRbp;
    }
    fflush(stdout);
}

GleamDebugger::CmdResult GleamDebugger::tryInspectCommand(const std::vector<std::string> & args)
{
    const std::string & cmd = args[0];
    uint64_t a = 0, b = 0;

    if(cmd == "regs")
    {
        cmdRegs();
        return CmdResult::Handled;
    }
    if(cmd == "setreg" && args.size() == 3 && parseHex(args[2], a))
    {
        RegId reg;
        if(registerByName(args[1], reg))
        {
            Registers r(currentThread()->hThread);
            r.Set(reg, a);
            printf("%s = 0x%llX\n", args[1].c_str(), a);
        }
        else
            printf("unknown register '%s'\n", args[1].c_str());
        fflush(stdout);
        return CmdResult::Handled;
    }
    if(cmd == "read" && args.size() == 3 && parseAddress(args[1], a) && parseHex(args[2], b))
    {
        cmdRead(a, b);
        return CmdResult::Handled;
    }
    if(cmd == "write" && args.size() >= 3 && parseAddress(args[1], a))
    {
        std::vector<uint8_t> bytes;
        bool ok = true;
        for(size_t i = 2; i < args.size() && ok; i++)
        {
            uint64_t byte = 0;
            ok = parseHex(args[i], byte) && byte <= 0xFF;
            if(ok)
                bytes.push_back((uint8_t)byte);
        }
        if(ok)
            cmdWrite(a, bytes);
        else
        {
            printf("invalid byte value\n");
            fflush(stdout);
        }
        return CmdResult::Handled;
    }
    if(cmd == "disasm" && args.size() <= 3)
    {
        uint64_t addr = 0, count = 8;
        bool ok = true;
        if(args.size() >= 2)
            ok = parseAddress(args[1], addr);
        else
        {
            Registers r(currentThread()->hThread);
            addr = r.Gip();
        }
        if(ok && args.size() == 3)
            ok = parseHex(args[2], count);
        if(ok && count > 0 && count <= 0x1000)
            cmdDisasm(addr, count);
        else
        {
            printf("usage: disasm [hexaddr] [count]\n");
            fflush(stdout);
        }
        return CmdResult::Handled;
    }
    if(cmd == "maps")
    {
        cmdMaps();
        return CmdResult::Handled;
    }
    if(cmd == "modules")
    {
        cmdModules();
        return CmdResult::Handled;
    }
    if(cmd == "find" && args.size() >= 4 && parseAddress(args[1], a) && parseHex(args[2], b))
    {
        if(args[3] == "ascii" || args[3] == "utf16")
        {
            std::string text;
            for(size_t i = 4; i < args.size(); i++)
            {
                if(!text.empty())
                    text += ' ';
                text += args[i];
            }
            cmdFindString(a, b, text, args[3] == "utf16");
        }
        else
        {
            std::string pattern;
            for(size_t i = 3; i < args.size(); i++)
            {
                if(!pattern.empty())
                    pattern += ' ';
                pattern += args[i];
            }
            cmdFind(a, b, pattern);
        }
        return CmdResult::Handled;
    }
    if(cmd == "stackscan" && args.size() <= 2)
    {
        uint64_t count = 32;
        if(args.size() == 2 && !parseHex(args[1], count))
        {
            printf("usage: stackscan [count]\n");
            fflush(stdout);
            return CmdResult::Handled;
        }
        cmdStackScan(count);
        return CmdResult::Handled;
    }
    if(cmd == "patch" && args.size() >= 3 && parseAddress(args[1], a))
    {
        std::vector<uint8_t> bytes;
        bool ok = true;
        for(size_t i = 2; i < args.size() && ok; i++)
        {
            uint64_t byte = 0;
            ok = parseHex(args[i], byte) && byte <= 0xFF;
            if(ok)
                bytes.push_back((uint8_t)byte);
        }
        if(ok)
            cmdPatch(a, bytes);
        else
        {
            printf("invalid byte value\n");
            fflush(stdout);
        }
        return CmdResult::Handled;
    }
    if(cmd == "patches")
    {
        cmdPatchList();
        return CmdResult::Handled;
    }
    if(cmd == "restore" && args.size() == 2 && parseAddress(args[1], a))
    {
        cmdRestore(a);
        return CmdResult::Handled;
    }
    if(cmd == "exinfo")
    {
        cmdExceptionInfo();
        return CmdResult::Handled;
    }
    if(cmd == "bt")
    {
        cmdBacktrace();
        return CmdResult::Handled;
    }
    if(cmd == "threads")
    {
        cmdThreads();
        return CmdResult::Handled;
    }
    return CmdResult::NotMine;
}
