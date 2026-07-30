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
    if(mProcess->breakpoints.empty() && mLogicalBps.empty())
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
        char ruleText[48] = "";
        auto rule = mBpRules.find(info.address);
        if(rule != mBpRules.end())
        {
            if(rule->second.trace)
                strcpy_s(ruleText, " trace");
            else if(rule->second.condReg != RegId::Invalid)
                sprintf_s(ruleText, " cond(op%d)", rule->second.condOp);
        }
        printf("0x%llX  %-8s %-16s%s%s%s\n",
               (unsigned long long)info.address,
               typeText,
               detail,
               info.singleshoot ? " once" : "",
               ruleText,
               ignore != mIgnoreHits.end() && ignore->second > 0 ? " (ignoring)" : "");
    }
    for(const auto & lb : mLogicalBps)
    {
        std::string line = "logical module=" + lb.module + " ";
        if(!lb.symbol.empty())
            line += "symbol=" + lb.symbol;
        else
        {
            char rva[32];
            sprintf_s(rva, "rva=0x%llX", (unsigned long long)lb.rva);
            line += rva;
        }
        if(lb.once)
            line += " once";
        if(lb.boundAddr)
        {
            char bound[40];
            sprintf_s(bound, " bound=0x%llX", (unsigned long long)lb.boundAddr);
            line += bound;
        }
        else
            line += " pending";
        printf("%s\n", line.c_str());
    }
    fflush(stdout);
}

// Evaluate a register condition against the current thread's registers.
bool GleamDebugger::evalCondition(RegId reg, int op, uint64_t value)
{
    Registers r(mThread->hThread);
    auto v = r.Get(reg);
    switch(op)
    {
    case 0: return v == value;
    case 1: return v != value;
    case 2: return v < value;
    case 3: return v > value;
    default: return false;
    }
}

// Parse "<reg><op><hexval>" (op: == != < >).
bool GleamDebugger::parseCondition(const std::string & text, RegId & reg, int & op, uint64_t & value)
{
    size_t pos = text.find("==");
    op = 0;
    if(pos == std::string::npos) { pos = text.find("!="); op = 1; }
    if(pos == std::string::npos) { pos = text.find('<'); op = 2; }
    if(pos == std::string::npos) { pos = text.find('>'); op = 3; }
    if(pos == std::string::npos || pos == 0)
        return false;
    return registerByName(text.substr(0, pos), reg) &&
           parseHex(text.substr(pos + (op <= 1 ? 2 : 1)), value);
}

// Evaluate the hit-time rule for a breakpoint. Returns true when the hit
// should pause normally; false when it should auto-continue (condition not
// met, tracepoint logged, or a resume-type "do" command ran).
bool GleamDebugger::evalBpRule(const BreakpointInfo & info, const BpRule* rule)
{
    if(!rule)
        return true;

    if(rule->condReg != RegId::Invalid && !evalCondition(rule->condReg, rule->condOp, rule->condValue))
        return false;

    if(rule->trace)
    {
        Registers r(mThread->hThread);
        printf("trace address=0x%llX rip=0x%llX tid=%u\n",
               (unsigned long long)info.address,
               (unsigned long long)r.Gip(),
               mDebugEvent.dwThreadId);
        fflush(stdout);
        return false;
    }

    // "bp <addr> do <command>": run the command in the suspended context.
    // Resume-type commands (g/step/...) mean: don't pause.
    if(!rule->command.empty())
        return !executeCommand(rule->command);

    return true;
}

// Parse "module!symbol" or "module+<hexrva>" as a module-relative spec.
// The module part must look like a module name: expressions like "dead+beef"
// or "1234+8" are rejected (hex-parseable module part = arithmetic).
bool GleamDebugger::parseLogicalSpec(const std::string & s, LogicalBp & out)
{
    size_t bang = s.find('!');
    if(bang != std::string::npos)
    {
        std::string mod = s.substr(0, bang);
        std::string sym = s.substr(bang + 1);
        if(mod.empty() || sym.empty())
            return false;
        if(mod.find_first_of("+-()[]") != std::string::npos ||
           sym.find_first_of("+-()[]") != std::string::npos)
            return false;
        out.module = normalizeModuleName(mod);
        out.symbol = sym;
        return true;
    }
    size_t plus = s.find('+');
    if(plus == std::string::npos || plus == 0)
        return false;
    std::string mod = s.substr(0, plus);
    uint64_t dummy = 0, rva = 0;
    if(parseHex(mod, dummy) || !parseHex(s.substr(plus + 1), rva))
        return false;
    if(mod.find_first_of("-()[]") != std::string::npos)
        return false;
    out.module = normalizeModuleName(mod);
    out.rva = rva;
    return true;
}

// Insert a logical entry, or replace the fields of an identical pending
// one (same module+symbol/rva): duplicate specs share one entry so they
// can never diverge into "one bound, one pending forever".
void GleamDebugger::upsertLogicalBp(const LogicalBp & lb)
{
    for(auto & e : mLogicalBps)
    {
        if(!e.boundAddr && e.module == lb.module &&
           e.symbol == lb.symbol && e.rva == lb.rva)
        {
            e.once = lb.once;
            e.rule = lb.rule;
            e.boundAddr = lb.boundAddr;
            e.boundBase = lb.boundBase;
            return;
        }
    }
    mLogicalBps.push_back(lb);
}

GleamDebugger::CmdResult GleamDebugger::tryBreakpointCommand(const std::vector<std::string> & args)
{
    const std::string & cmd = args[0];
    uint64_t a = 0, b = 0;

    // bp <addr> [once] [if <reg><op><hexval>] [do <command...>]
    // <addr> may also be module!symbol or module+rva (module-relative: bound
    // now when loaded, pending until the module loads otherwise).
    if(cmd == "bp" && args.size() >= 2)
    {
        LogicalBp lb;
        const bool logical = parseLogicalSpec(args[1], lb);
        // For logical specs (module!symbol or module+rva), parseAddress is just
        // a probe to see if it resolves immediately - if it fails but the logical
        // parse succeeded, suppress the error (it will become a pending bp).
        const bool resolved = parseAddress(args[1], a);
        // Check for ambiguous symbol BEFORE proceeding - ambiguous symbols
        // must NEVER bind (not now, not as pending).
        const bool ambiguous = !mAddrError.empty() && mAddrError.find("ambiguous") != std::string::npos;
        if(logical && !resolved)
            mAddrError.clear(); // the logical spec succeeded; don't leak the probe's error
        if(!resolved && !logical)
            return CmdResult::NotMine;
        bool once = false;
        BpRule rule;
        bool badArgs = false;
        for(size_t i = 2; i < args.size() && !badArgs; i++)
        {
            if(args[i] == "once")
                once = true;
            else if(args[i] == "if" && i + 1 < args.size())
            {
                if(!parseCondition(args[i + 1], rule.condReg, rule.condOp, rule.condValue))
                    badArgs = true;
                i++;
            }
            else if(args[i] == "do" && i + 1 < args.size())
            {
                for(size_t j = i + 1; j < args.size(); j++)
                {
                    if(!rule.command.empty())
                        rule.command += ' ';
                    rule.command += args[j];
                }
                break;
            }
            else
                badArgs = true;
        }
        if(badArgs)
        {
            printf("usage: bp <addr> [once] [if <reg><==|!=|<|>><hexval>] [do <command...>]\n");
        }
        else if(resolved)
        {
            // RVA form with the module already loaded: reject out-of-image
            // offsets immediately instead of recording a bogus entry.
            uint64_t infoBase = 0;
            uint32_t imageSize = 0;
            if(logical && lb.symbol.empty() &&
               (!moduleInfoOf(lb.module, infoBase, imageSize) || lb.rva >= imageSize))
            {
                printf("rva 0x%llX out of image for module %s\n",
                       (unsigned long long)lb.rva, lb.module.c_str());
            }
            else if(mProcess->SetBreakpoint(a, once))
            {
                if(rule.condReg != RegId::Invalid || rule.trace || !rule.command.empty())
                    mBpRules[a] = rule;
                if(logical) // module-relative: remember for unbind/re-bind
                {
                    lb.once = once;
                    lb.rule = rule;
                    lb.boundAddr = a;
                    moduleBaseByName(lb.module, lb.boundBase);
                    upsertLogicalBp(lb);
                }
                printf("%sbreakpoint set at 0x%llX\n", once ? "one-shot " : "", a);
            }
            else
                printf("failed to set breakpoint at 0x%llX\n", a);
        }
        else if(ambiguous)
        {
            // Ambiguous symbols must NEVER bind: not now, not later. The
            // refusal was already printed by resolveModuleSymbol; do NOT
            // register a pending breakpoint (its rebind retries would spam
            // the same error at every debug event).
            printf("breakpoint refused (ambiguous symbol)\n");
        }
        else // module not loaded yet: bind when it loads
        {
            lb.once = once;
            lb.rule = rule;
            upsertLogicalBp(lb);
            if(!lb.symbol.empty())
                printf("breakpoint pending module=%s symbol=%s\n", lb.module.c_str(), lb.symbol.c_str());
            else
                printf("breakpoint pending module=%s rva=0x%llX\n", lb.module.c_str(), (unsigned long long)lb.rva);
        }
        fflush(stdout);
        return CmdResult::Handled;
    }
    if(cmd == "trace" && args.size() >= 2 && args.size() <= 3 && parseAddress(args[1], a))
    {
        bool once = args.size() == 3 && args[2] == "once";
        if(args.size() == 3 && !once)
            printf("usage: trace <addr> [once]\n");
        else if(mProcess->SetBreakpoint(a, once))
        {
            BpRule rule;
            rule.trace = true;
            mBpRules[a] = rule;
            printf("%stracepoint set at 0x%llX\n", once ? "one-shot " : "", a);
        }
        else
            printf("failed to set tracepoint at 0x%llX\n", a);
        fflush(stdout);
        return CmdResult::Handled;
    }
    if(cmd == "rbp" && args.size() == 2)
    {
        if(parseAddress(args[1], a))
        {
            // Drop any logical entry bound at this address too.
            for(auto it = mLogicalBps.begin(); it != mLogicalBps.end();)
                it = it->boundAddr == a ? mLogicalBps.erase(it) : std::next(it);
            mBpRules.erase(a);
            mIgnoreHits.erase(a);
            printf(mProcess->DeleteBreakpoint(a) ? "breakpoint removed at 0x%llX\n" : "failed to remove breakpoint at 0x%llX\n", a);
            fflush(stdout);
            return CmdResult::Handled;
        }
        LogicalBp spec;
        if(parseLogicalSpec(args[1], spec))
        {
            mAddrError.clear(); // spec parsed; the probe's error must not leak
            for(auto it = mLogicalBps.begin(); it != mLogicalBps.end(); ++it)
            {
                if(!it->boundAddr && it->module == spec.module &&
                   it->symbol == spec.symbol && it->rva == spec.rva)
                {
                    mLogicalBps.erase(it);
                    printf("pending breakpoint removed module=%s\n", spec.module.c_str());
                    fflush(stdout);
                    return CmdResult::Handled;
                }
            }
            printf("no pending breakpoint for %s\n", args[1].c_str());
            fflush(stdout);
            return CmdResult::Handled;
        }
        return CmdResult::NotMine;
    }
    if(cmd == "hbp" && args.size() >= 2 && args.size() <= 4 && parseAddress(args[1], a))
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
        else if(!mRawDrThreads.empty())
        {
            // P0-6 bidirectional conflict rule: raw DR state and engine
            // hardware breakpoints must never coexist.
            printf("engine hardware breakpoints unavailable after raw dr write\n");
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
    if(cmd == "hbpd" && args.size() == 2 && parseAddress(args[1], a))
    {
        printf(mProcess->DeleteHardwareBreakpoint(a) ? "hardware breakpoint removed at 0x%llX\n" : "failed to remove hardware breakpoint at 0x%llX\n", a);
        fflush(stdout);
        return CmdResult::Handled;
    }
    if(cmd == "mbp" && (args.size() == 3 || args.size() == 4) && parseAddress(args[1], a) && parseHex(args[2], b))
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
    if(cmd == "mbpd" && args.size() == 2 && parseAddress(args[1], a))
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
    if(cmd == "ignore" && args.size() == 3 && parseAddress(args[1], a))
    {
        uint32_t count = (uint32_t)strtoul(args[2].c_str(), nullptr, 10);
        mIgnoreHits[a] = count;
        printf("will ignore the next %u hits of the breakpoint at 0x%llX\n", count, a);
        fflush(stdout);
        return CmdResult::Handled;
    }
    return CmdResult::NotMine;
}
