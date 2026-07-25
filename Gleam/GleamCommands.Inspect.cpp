// Inspection commands: registers, memory, disassembly, maps, modules,
// memory search, exception info, backtrace.

#include "GleamDebugger.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <fstream>
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
    const CONTEXT* ctx = r.GetContext();
    printf("DR0=%016llX DR1=%016llX DR2=%016llX DR3=%016llX\n",
           (unsigned long long)ctx->Dr0, (unsigned long long)ctx->Dr1,
           (unsigned long long)ctx->Dr2, (unsigned long long)ctx->Dr3);
    printf("DR6=%016llX DR7=%016llX MXCSR=%08X\n",
           (unsigned long long)ctx->Dr6, (unsigned long long)ctx->Dr7, ctx->FltSave.MxCsr);
    for(int i = 0; i < 16; i++)
    {
        const auto & xmm = ctx->FltSave.XmmRegisters[i];
        printf("XMM%-2d=%016llX%016llX\n", i,
               (unsigned long long)xmm.High, (unsigned long long)xmm.Low);
    }
    fflush(stdout);
}

// Write one register: GPR (engine enum), eflags, dr0-dr7 (dr4->dr6, dr5->dr7
// aliases like x64dbg), mxcsr, xmm0-xmm15 (32 hex chars, high half first).
// The write lands via the Registers RAII destructor (SetThreadContext).
bool GleamDebugger::setRegisterExtended(const std::string & name, const std::string & valueText)
{
    Registers r(currentThread()->hThread);
    CONTEXT* ctx = r.GetContext();
    uint64_t v = 0;

    RegId reg;
    if(registerByName(name, reg))
    {
        if(!parseHex(valueText, v))
            return false;
        r.Set(reg, v);
        printf("%s = 0x%llX\n", name.c_str(), v);
        return true;
    }
    if(_stricmp(name.c_str(), "eflags") == 0)
    {
        if(!parseHex(valueText, v) || v > 0xFFFFFFFF)
            return false;
        ctx->EFlags = (DWORD)v;
        printf("eflags = 0x%08X\n", (DWORD)v);
        return true;
    }
    if(_stricmp(name.c_str(), "mxcsr") == 0)
    {
        if(!parseHex(valueText, v) || v > 0xFFFFFFFF)
            return false;
        ctx->FltSave.MxCsr = (DWORD)v;
        printf("mxcsr = 0x%08X\n", (DWORD)v);
        return true;
    }
    if(name.size() == 3 && (name[0] == 'd' || name[0] == 'D') &&
       (name[1] == 'r' || name[1] == 'R') && name[2] >= '0' && name[2] <= '7')
    {
        if(!parseHex(valueText, v))
            return false;
        int n = name[2] - '0';
        if(n == 4) n = 6;      // DR4 aliases DR6
        else if(n == 5) n = 7; // DR5 aliases DR7
        switch(n)
        {
        case 0: ctx->Dr0 = v; break;
        case 1: ctx->Dr1 = v; break;
        case 2: ctx->Dr2 = v; break;
        case 3: ctx->Dr3 = v; break;
        case 6: ctx->Dr6 = v; break;
        case 7: ctx->Dr7 = v; break;
        }
        printf("%s = 0x%llX\n", name.c_str(), v);
        return true;
    }
    if(name.size() >= 4 && name.size() <= 5 && _strnicmp(name.c_str(), "xmm", 3) == 0)
    {
        char* end = nullptr;
        long idx = strtol(name.c_str() + 3, &end, 10);
        if(!end || *end != '\0' || end == name.c_str() + 3 || idx < 0 || idx > 15)
            return false;
        if(valueText.size() != 32)
            return false;
        uint64_t hi = 0, lo = 0;
        if(!parseHex(valueText.substr(0, 16), hi) || !parseHex(valueText.substr(16), lo))
            return false;
        ctx->FltSave.XmmRegisters[idx].High = hi;
        ctx->FltSave.XmmRegisters[idx].Low = lo;
        printf("xmm%ld = %016llX%016llX\n", idx, hi, lo);
        return true;
    }
    return false;
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

void GleamDebugger::cmdReadTyped(const char* type, uint64_t addr)
{
    int width = 8;
    if(!strcmp(type, "u8")) width = 1;
    else if(!strcmp(type, "u16")) width = 2;
    else if(!strcmp(type, "u32")) width = 4;
    uint64_t value = 0;
    if(!mProcess->MemReadSafe(addr, &value, width))
    {
        printf("read failed at 0x%llX\n", addr);
        fflush(stdout);
        return;
    }
    switch(width)
    {
    case 1: printf("= 0x%02llX\n", value & 0xFF); break;
    case 2: printf("= 0x%04llX\n", value & 0xFFFF); break;
    case 4: printf("= 0x%08llX\n", value & 0xFFFFFFFF); break;
    default: printf("= 0x%llX\n", value); break;
    }
    fflush(stdout);
}

void GleamDebugger::cmdReadString(uint64_t addr, uint64_t maxLen, bool utf16)
{
    std::string text;
    bool terminated = false;
    bool firstChunk = true;
    while(text.size() < maxLen && !terminated)
    {
        const size_t unit = utf16 ? 2 : 1;
        const size_t chunk = (std::min)((size_t)64, ((size_t)maxLen - text.size()) * unit);
        char buf[64];
        if(!mProcess->MemReadSafe(addr + text.size() * unit, buf, chunk))
        {
            if(firstChunk)
            {
                printf("cannot read string at 0x%llX\n", addr);
                fflush(stdout);
                return;
            }
            break; // truncated by an unreadable page
        }
        firstChunk = false;
        for(size_t i = 0; i < chunk; i += unit)
        {
            if(utf16)
            {
                uint16_t wc = *(uint16_t*)(buf + i);
                if(!wc) { terminated = true; break; }
                // Best-effort UTF-8 (BMP only; enough for typical strings).
                if(wc < 0x80)
                    text += (char)wc;
                else if(wc < 0x800)
                {
                    text += (char)(0xC0 | (wc >> 6));
                    text += (char)(0x80 | (wc & 0x3F));
                }
                else
                {
                    text += (char)(0xE0 | (wc >> 12));
                    text += (char)(0x80 | ((wc >> 6) & 0x3F));
                    text += (char)(0x80 | (wc & 0x3F));
                }
            }
            else
            {
                if(!buf[i]) { terminated = true; break; }
                text += buf[i];
            }
        }
    }
    printf("string at 0x%llX = \"%s\"%s\n", addr, text.c_str(),
           terminated ? "" : " (no NUL within limit)");
    fflush(stdout);
}

void GleamDebugger::cmdSaveMem(uint64_t addr, uint64_t size, const std::string & file)
{
    if(size == 0 || size > 0x10000000)
    {
        printf("invalid size (1..268435456)\n");
        fflush(stdout);
        return;
    }
    std::ofstream out(file, std::ios::binary);
    if(!out)
    {
        printf("cannot open %s for writing\n", file.c_str());
        fflush(stdout);
        return;
    }
    // Page-granular export: a failing page is zero-filled and counted, so
    // offsets in the file always match the address space layout.
    const uint64_t kPage = 0x1000;
    std::vector<uint8_t> buf(kPage);
    uint64_t holes = 0, done = 0;
    while(done < size)
    {
        const size_t n = (size_t)(std::min)(kPage, size - done);
        if(!mProcess->MemReadSafe(addr + done, buf.data(), n))
        {
            memset(buf.data(), 0, n);
            holes++;
        }
        out.write((const char*)buf.data(), n);
        done += n;
    }
    out.close();
    if(!out)
        printf("write error while saving %s\n", file.c_str());
    else
        printf("saved 0x%llX bytes to %s holes=%llu\n",
               (unsigned long long)size, file.c_str(), (unsigned long long)holes);
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

std::string GleamDebugger::disasmOne(uint64_t addr)
{
    uint8_t data[16];
    if(!mProcess->MemReadSafe(addr, data, sizeof(data)))
        return std::string();
    ZydisDisassembledInstruction instruction;
    if(!ZYAN_SUCCESS(ZydisDisassembleIntel(kMachineMode, addr, data, sizeof(data), &instruction)))
        return std::string();
    return std::string(instruction.text);
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
    if(size == 0 || size > 0x10000000)
    {
        printf("invalid size (1..256MB)\n");
        fflush(stdout);
        return;
    }
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
    if(size == 0 || size > 0x10000000)
    {
        printf("invalid size (1..256MB)\n");
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
    // Original bytes of the whole new range.
    std::vector<uint8_t> fresh(bytes.size());
    if(!mProcess->MemReadSafe(addr, fresh.data(), fresh.size()))
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

    // Merge the new range into any overlapping records. Existing records
    // hold the EARLIER original bytes and always win in their range; fresh
    // bytes only fill the parts no previous record covers. `merged` carries
    // the running union so bridging several old records keeps every byte.
    uint64_t start = addr, end = addr + bytes.size();
    std::vector<uint8_t> merged(fresh);
    for(auto it = mPatches.begin(); it != mPatches.end();)
    {
        uint64_t s = it->first, e = s + it->second.size();
        if(e <= start || s >= end)
        {
            ++it;
            continue;
        }
        uint64_t ns = (std::min)(start, s), ne = (std::max)(end, e);
        std::vector<uint8_t> u(ne - ns);
        // Current union covers its own range...
        memcpy(u.data() + (start - ns), merged.data(), merged.size());
        // ...and the earlier record's original bytes win in its range.
        memcpy(u.data() + (s - ns), it->second.data(), it->second.size());
        start = ns;
        end = ne;
        merged = std::move(u);
        it = mPatches.erase(it);
    }
    mPatches[start] = std::move(merged);
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
    if(cmd == "setreg" && args.size() == 3)
    {
        if(!setRegisterExtended(args[1], args[2]))
        {
            printf("unknown register or bad value '%s %s'\n", args[1].c_str(), args[2].c_str());
            fflush(stdout);
        }
        return CmdResult::Handled;
    }
    if(cmd == "read" && args.size() >= 3 &&
       (args[1] == "u8" || args[1] == "u16" || args[1] == "u32" ||
        args[1] == "u64" || args[1] == "ptr"))
    {
        if(args.size() == 3 && parseAddress(args[2], a))
            cmdReadTyped(args[1].c_str(), a);
        else
        {
            printf("usage: read u8|u16|u32|u64|ptr <addr>\n");
            fflush(stdout);
        }
        return CmdResult::Handled;
    }
    if(cmd == "read" && args.size() >= 3 && (args[1] == "ansi" || args[1] == "utf16"))
    {
        uint64_t maxLen = 256;
        bool ok = parseAddress(args[2], a);
        if(ok && args.size() == 4)
            ok = parseHex(args[3], maxLen) && maxLen > 0 && maxLen <= 0x10000;
        if(ok && args.size() <= 4)
            cmdReadString(a, maxLen, args[1] == "utf16");
        else
        {
            printf("usage: read ansi|utf16 <addr> [hexmaxlen]\n");
            fflush(stdout);
        }
        return CmdResult::Handled;
    }
    if(cmd == "savemem" && args.size() == 4 && parseAddress(args[1], a) && parseHex(args[2], b))
    {
        cmdSaveMem(a, b, args[3]);
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
    if(cmd == "eval" && args.size() >= 2)
    {
        // Join without separators so "eval 1 + 2" works as well as "eval 1+2".
        std::string expr;
        for(size_t i = 1; i < args.size(); i++)
            expr += args[i];
        uint64_t value = 0;
        std::string err;
        if(evalExpression(expr, value, err))
            printf("= 0x%llX\n", (unsigned long long)value);
        else
            printf("error: %s\n", err.c_str());
        fflush(stdout);
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
