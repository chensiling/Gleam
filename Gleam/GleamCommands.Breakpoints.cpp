// Breakpoint commands: software, hardware, memory breakpoints and ignore counts.

#include "GleamDebugger.h"

#include <cstdio>

using namespace GleeBug;

const char* GleamDebugger::hwTypeText(HardwareType type)
{
    switch(type)
    {
    case HardwareType::Execute: return "x";
    case HardwareType::Write: return "w";
    case HardwareType::Access: return "rw";
    default: return "?";
    }
}

const char* GleamDebugger::memTypeText(MemoryType type)
{
    switch(type)
    {
    case MemoryType::Access: return "a";
    case MemoryType::Read: return "r";
    case MemoryType::Write: return "w";
    case MemoryType::Execute: return "x";
    default: return "?";
    }
}

void GleamDebugger::cmdBreakpointList()
{
    if(mProcess->breakpoints.empty())
    {
        printf("no breakpoints\n");
        fflush(stdout);
        return;
    }
    for(const auto & kv : mProcess->breakpoints)
    {
        const auto & info = kv.second;
        char detail[64] = "";
        switch(info.type)
        {
        case BreakpointType::Software:
            strcpy_s(detail, "int3");
            break;
        case BreakpointType::Hardware:
            sprintf_s(detail, "dr%d %s size=%d",
                      (int)info.internal.hardware.slot,
                      hwTypeText(info.internal.hardware.type),
                      (int)info.internal.hardware.size);
            break;
        case BreakpointType::Memory:
            sprintf_s(detail, "%s size=0x%llX",
                      memTypeText(info.internal.memory.type),
                      (unsigned long long)info.internal.memory.size);
            break;
        }
        const char* typeText =
            info.type == BreakpointType::Software ? "software" :
            info.type == BreakpointType::Hardware ? "hardware" : "memory";
        auto ignore = mIgnoreHits.find(info.address);
        printf("0x%llX  %-8s %-16s%s%s\n",
               (unsigned long long)info.address,
               typeText,
               detail,
               info.singleshoot ? " once" : "",
               ignore != mIgnoreHits.end() && ignore->second > 0 ? " (ignoring)" : "");
    }
    fflush(stdout);
}

GleamDebugger::CmdResult GleamDebugger::tryBreakpointCommand(const std::vector<std::string> & args)
{
    const std::string & cmd = args[0];
    uint64_t a = 0, b = 0;

    if(cmd == "bp" && (args.size() == 2 || args.size() == 3) && parseHex(args[1], a))
    {
        bool once = args.size() == 3 && args[2] == "once";
        if(args.size() == 3 && !once)
        {
            printf("usage: bp <hexaddr> [once]\n");
        }
        else if(mProcess->SetBreakpoint(a, once))
        {
            printf("%sbreakpoint set at 0x%llX\n", once ? "one-shot " : "", a);
        }
        else
            printf("failed to set breakpoint at 0x%llX\n", a);
        fflush(stdout);
        return CmdResult::Handled;
    }
    if(cmd == "rbp" && args.size() == 2 && parseHex(args[1], a))
    {
        printf(mProcess->DeleteBreakpoint(a) ? "breakpoint removed at 0x%llX\n" : "failed to remove breakpoint at 0x%llX\n", a);
        fflush(stdout);
        return CmdResult::Handled;
    }
    if(cmd == "hbp" && args.size() >= 2 && args.size() <= 4 && parseHex(args[1], a))
    {
        HardwareType type = HardwareType::Execute;
        HardwareSize size = HardwareSize::SizeByte;
        bool badArgs = false;
        if(args.size() >= 3)
        {
            if(args[2] == "x") type = HardwareType::Execute;
            else if(args[2] == "w") type = HardwareType::Write;
            else if(args[2] == "rw" || args[2] == "a") type = HardwareType::Access;
            else badArgs = true;
        }
        if(args.size() == 4 && !badArgs)
        {
            switch(atoi(args[3].c_str()))
            {
            case 1: size = HardwareSize::SizeByte; break;
            case 2: size = HardwareSize::SizeWord; break;
            case 4: size = HardwareSize::SizeDword; break;
#ifdef _WIN64
            case 8: size = HardwareSize::SizeQword; break;
#endif
            default: badArgs = true; break;
            }
        }
        if(type == HardwareType::Execute && size != HardwareSize::SizeByte)
        {
            printf("execute breakpoints require size 1\n");
            badArgs = true;
        }
        if(badArgs)
        {
            printf("usage: hbp <hexaddr> [x|w|rw] [1|2|4|8]\n");
        }
        else
        {
            HardwareSlot slot;
            if(!mProcess->GetFreeHardwareBreakpointSlot(slot))
                printf("no free hardware breakpoint slot (4 max)\n");
            else if(mProcess->SetHardwareBreakpoint(a, slot, type, size))
                printf("hardware breakpoint set at 0x%llX (%s)\n", a, hwTypeText(type));
            else
                printf("failed to set hardware breakpoint at 0x%llX\n", a);
        }
        fflush(stdout);
        return CmdResult::Handled;
    }
    if(cmd == "hbpd" && args.size() == 2 && parseHex(args[1], a))
    {
        printf(mProcess->DeleteHardwareBreakpoint(a) ? "hardware breakpoint removed at 0x%llX\n" : "failed to remove hardware breakpoint at 0x%llX\n", a);
        fflush(stdout);
        return CmdResult::Handled;
    }
    if(cmd == "mbp" && (args.size() == 3 || args.size() == 4) && parseHex(args[1], a) && parseHex(args[2], b))
    {
        MemoryType type = MemoryType::Access;
        bool badArgs = false;
        if(args.size() == 4)
        {
            if(args[3] == "a") type = MemoryType::Access;
            else if(args[3] == "r") type = MemoryType::Read;
            else if(args[3] == "w") type = MemoryType::Write;
            else if(args[3] == "x") type = MemoryType::Execute;
            else badArgs = true;
        }
        if(badArgs || b == 0)
            printf("usage: mbp <hexaddr> <hexsize> [a|r|w|x]\n");
        else if(mProcess->SetMemoryBreakpoint(a, b, type, false))
            printf("memory breakpoint set at 0x%llX size 0x%llX (%s)\n", a, b, memTypeText(type));
        else
            printf("failed to set memory breakpoint at 0x%llX\n", a);
        fflush(stdout);
        return CmdResult::Handled;
    }
    if(cmd == "mbpd" && args.size() == 2 && parseHex(args[1], a))
    {
        printf(mProcess->DeleteMemoryBreakpoint(a) ? "memory breakpoint removed at 0x%llX\n" : "failed to remove memory breakpoint at 0x%llX\n", a);
        fflush(stdout);
        return CmdResult::Handled;
    }
    if(cmd == "bl")
    {
        cmdBreakpointList();
        return CmdResult::Handled;
    }
    if(cmd == "ignore" && args.size() == 3 && parseHex(args[1], a))
    {
        uint32_t count = (uint32_t)strtoul(args[2].c_str(), nullptr, 10);
        mIgnoreHits[a] = count;
        printf("will ignore the next %u hits of the breakpoint at 0x%llX\n", count, a);
        fflush(stdout);
        return CmdResult::Handled;
    }
    return CmdResult::NotMine;
}
