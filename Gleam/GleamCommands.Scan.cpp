/**
 * @file GleamCommands.Scan.cpp
 * @brief Code scanning: `xref` (references to an address) and `findasm`
 *        (instructions matching text).
 *
 * Both share one linear-disassembly sweep over the debuggee's committed
 * executable regions. Each block is read with 15 extra bytes so an instruction
 * straddling a block boundary still decodes completely.
 *
 * @note Instruction targets are computed from **raw bytes** (E8/E9 rel32,
 *       FF /2 and /4 rip-relative) rather than from decoder mnemonics. The
 *       discipline dates from a vendored-Zydis enum mismatch; it is kept even
 *       though the amalgamated v4.1.1 build fixed that, because it makes the
 *       scanners independent of decoder-internal state.
 */

#include "GleamDebugger.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <GleeBug/Zydis/Zydis.h>

using namespace GleeBug;

namespace
{
#ifdef _WIN64
    const ZydisMachineMode kScanMode = ZYDIS_MACHINE_MODE_LONG_64;
#else
    const ZydisMachineMode kScanMode = ZYDIS_MACHINE_MODE_LONG_COMPAT_32;
#endif

    const DWORD kExecProtect = PAGE_EXECUTE | PAGE_EXECUTE_READ |
                               PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    const size_t kChunkSize = 0x100000; // 1 MB per read

    // Invoke cb(base, size) for every committed executable region.
    template<typename F>
    void forEachExecRegion(HANDLE hProcess, F cb)
    {
        uint64_t addr = 0;
        for(;;)
        {
            MEMORY_BASIC_INFORMATION mbi;
            if(!VirtualQueryEx(hProcess, (LPCVOID)addr, &mbi, sizeof(mbi)))
                break;
            uint64_t base = (uint64_t)mbi.BaseAddress;
            uint64_t size = (uint64_t)mbi.RegionSize;
            if(mbi.State == MEM_COMMIT && (mbi.Protect & kExecProtect))
                cb(base, size);
            uint64_t next = base + size;
            if(next <= addr)
                break;
            addr = next;
        }
    }

    // Decode one instruction at addr from buf (up to avail bytes, max 16).
    bool decodeAt(const uint8_t* buf, size_t avail, uint64_t addr, ZydisDisassembledInstruction & out)
    {
        if(avail > 16)
            avail = 16;
        if(avail == 0)
            return false;
        return ZYAN_SUCCESS(ZydisDisassembleIntel(kScanMode, addr, buf, avail, &out));
    }

    // If the instruction transfers control to a computed target, return it.
    enum class TargetKind { None, Direct, Indirect };
    TargetKind transferTarget(const uint8_t* buf, uint64_t addr, uint8_t len, uint64_t & target)
    {
        // E8 call rel32 / E9 jmp rel32
        if((buf[0] == 0xE8 || buf[0] == 0xE9) && len == 5)
        {
            int32_t rel;
            memcpy(&rel, buf + 1, 4);
            target = addr + len + (int64_t)rel;
            return TargetKind::Direct;
        }
        // EB jmp rel8 (2 bytes)
        if(buf[0] == 0xEB && len == 2)
        {
            target = addr + len + (int8_t)buf[1];
            return TargetKind::Direct;
        }
        // FF /2 call [rip+disp32] / FF /4 jmp [rip+disp32]
        if(buf[0] == 0xFF && len == 6)
        {
            uint8_t modrm = buf[1];
            uint8_t mod = modrm >> 6, reg = (modrm >> 3) & 7, rm = modrm & 7;
            if(mod == 0 && rm == 5 && (reg == 2 || reg == 4))
            {
                int32_t disp;
                memcpy(&disp, buf + 2, 4);
                target = addr + len + (int64_t)disp;
                return TargetKind::Indirect;
            }
        }
        return TargetKind::None;
    }

    // Lowercase, single-spaced normalization for instruction text matching.
    std::string normalizeText(const char* text)
    {
        std::string out;
        bool lastSpace = true;
        for(const char* p = text; *p; p++)
        {
            char c = *p;
            if(c == ' ' || c == '\t')
            {
                if(!lastSpace)
                    out += ' ';
                lastSpace = true;
            }
            else
            {
                out += (char)tolower((unsigned char)c);
                lastSpace = false;
            }
        }
        while(!out.empty() && out.back() == ' ')
            out.pop_back();
        return out;
    }
}

void GleamDebugger::cmdXref(uint64_t target)
{
    size_t hits = 0;
    forEachExecRegion(mProcess->hProcess, [&](uint64_t base, uint64_t size)
    {
        for(uint64_t off = 0; off < size; off += kChunkSize)
        {
            size_t chunk = (size_t)(std::min)((uint64_t)kChunkSize, size - off);
            // Read up to 15 bytes beyond the chunk so instructions crossing
            // the boundary decode fully; instruction STARTS beyond the chunk
            // belong to the next chunk.
            size_t avail = (size_t)(std::min)(chunk + 15, size - off);
            std::vector<uint8_t> buf(avail);
            if(!mProcess->MemReadSafe(base + off, buf.data(), avail))
                continue;
            size_t i = 0;
            while(i < chunk)
            {
                uint64_t ia = base + off + i;
                ZydisDisassembledInstruction insn;
                if(!decodeAt(buf.data() + i, avail - i, ia, insn) || insn.info.length == 0)
                {
                    i++;
                    continue;
                }
                uint8_t len = insn.info.length;
                uint64_t t = 0;
                auto kind = transferTarget(buf.data() + i, ia, len, t);
                if(kind == TargetKind::Direct && t == target)
                {
                    printf("0x%016llX  %s\n", ia, insn.text);
                    hits++;
                }
                else if(kind == TargetKind::Indirect)
                {
                    uint64_t pointed = 0;
                    if(mProcess->MemReadSafe(t, &pointed, sizeof(pointed)) && pointed == target)
                    {
                        printf("0x%016llX  %s  ; via [0x%llX]\n", ia, insn.text, t);
                        hits++;
                    }
                }
                i += len;
            }
        }
    });
    printf("%zu references to 0x%llX\n", hits, target);
    fflush(stdout);
}

void GleamDebugger::cmdFindAsm(const std::string & text)
{
    auto needle = normalizeText(text.c_str());
    if(needle.empty())
    {
        printf("usage: findasm <instruction text>\n");
        fflush(stdout);
        return;
    }
    size_t hits = 0;
    bool capped = false;
    forEachExecRegion(mProcess->hProcess, [&](uint64_t base, uint64_t size)
    {
        for(uint64_t off = 0; off < size && !capped; off += kChunkSize)
        {
            size_t chunk = (size_t)(std::min)((uint64_t)kChunkSize, size - off);
            size_t avail = (size_t)(std::min)(chunk + 15, size - off);
            std::vector<uint8_t> buf(avail);
            if(!mProcess->MemReadSafe(base + off, buf.data(), avail))
                continue;
            size_t i = 0;
            while(i < chunk)
            {
                uint64_t ia = base + off + i;
                ZydisDisassembledInstruction insn;
                if(!decodeAt(buf.data() + i, avail - i, ia, insn) || insn.info.length == 0)
                {
                    i++;
                    continue;
                }
                auto hay = normalizeText(insn.text);
                if(hay.find(needle) != std::string::npos)
                {
                    printf("0x%016llX  %s\n", ia, insn.text);
                    if(++hits >= 200)
                    {
                        printf("(stopped at 200 matches)\n");
                        capped = true;
                        break;
                    }
                }
                i += insn.info.length;
            }
        }
    });
    printf("%zu matches\n", hits);
    fflush(stdout);
}

GleamDebugger::CmdResult GleamDebugger::tryScanCommand(const std::vector<std::string> & args)
{
    const std::string & cmd = args[0];
    uint64_t a = 0;

    if(cmd == "xref" && args.size() == 2 && parseAddress(args[1], a))
    {
        cmdXref(a);
        return CmdResult::Handled;
    }
    if(cmd == "findasm" && args.size() >= 2)
    {
        std::string text;
        for(size_t i = 1; i < args.size(); i++)
        {
            if(!text.empty())
                text += ' ';
            text += args[i];
        }
        cmdFindAsm(text);
        return CmdResult::Handled;
    }
    return CmdResult::NotMine;
}
