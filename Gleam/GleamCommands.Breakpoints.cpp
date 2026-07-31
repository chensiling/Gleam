/**
 * @file GleamCommands.Breakpoints.cpp
 * @brief Breakpoint commands: software, hardware, memory, ignore counts,
 *        conditions and tracepoints.
 *
 * `bp` accepts three address forms, and the third one changes the lifecycle:
 * a literal expression binds immediately, while `module!symbol` and
 * `module+rva` create a *logical* breakpoint that may stay pending until its
 * module loads (see GleamDebugger::bindModuleBreakpoints).
 *
 * Hit-time behaviour (`if` conditions, `do` commands, `trace`, ignore counts)
 * is evaluated by evalBpRule() **inside the debugger thread's callback**, not
 * from the command loop. That keeps auto-continuing hits off the pause path,
 * which is what makes high-frequency tracepoints usable - so this logic must
 * stay in the core rather than moving behind a future MCP boundary.
 *
 * @note Two engine behaviours constrain the code here:
 *       - Re-setting a breakpoint at the current address inside the same pause
 *         hits again immediately: the restored original instruction has not
 *         executed yet, so the new breakpoint catches it. Not a bug - tests
 *         must expect it.
 *       - SetBreakpoint() refuses a second breakpoint at an address that
 *         already has one, so "user breakpoint at an address an internal
 *         breakpoint holds" is unreachable from the command model.
 */

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
    if(mProcess->breakpoints.empty() && mLogicalBps.empty() && mDisabledBps.empty())
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
        // Condition and trace are reported INDEPENDENTLY, not as alternatives:
        // bpedit can set both, and evalBpRule tests the condition BEFORE the
        // trace, so a conditional tracepoint only logs when the condition
        // holds. Hiding the condition here would misreport what will happen.
        char ruleText[64] = "";
        auto rule = mBpRules.find(info.address);
        if(rule != mBpRules.end())
        {
            if(rule->second.condReg != RegId::Invalid)
                sprintf_s(ruleText, " cond(op%d)", rule->second.condOp);
            if(rule->second.trace)
                strcat_s(ruleText, " trace");
            if(!rule->second.command.empty())
                strcat_s(ruleText, " do");
        }
        char hitText[32] = "";
        auto hits = mBpHits.find(info.address);
        if(hits != mBpHits.end() && hits->second)
            sprintf_s(hitText, " hits=%llu", (unsigned long long)hits->second);
        printf("0x%llX  %-8s %-16s enabled%s%s%s%s\n",
               (unsigned long long)info.address,
               typeText,
               detail,
               info.singleshoot ? " once" : "",
               ruleText,
               hitText,
               ignore != mIgnoreHits.end() && ignore->second > 0 ? " (ignoring)" : "");
    }
    // Disabled entries are no longer in the engine's table, so they are listed
    // from the saved specs - otherwise "disable" would look like "delete".
    for(const auto & kv : mDisabledBps)
    {
        const char* typeText =
            kv.second.type == BreakpointType::Software ? "software" :
            kv.second.type == BreakpointType::Hardware ? "hardware" : "memory";
        char detail[64] = "";
        switch(kv.second.type)
        {
        case BreakpointType::Hardware:
            sprintf_s(detail, "%s size=%d", hwTypeText(kv.second.hwType),
                      (int)kv.second.hwSize);
            break;
        case BreakpointType::Memory:
            sprintf_s(detail, "%s size=0x%llX", memTypeText(kv.second.memType),
                      (unsigned long long)kv.second.memSize);
            break;
        default:
            strcpy_s(detail, "int3");
            break;
        }
        char ruleText[64] = "";
        if(kv.second.hasRule)
        {
            if(kv.second.rule.condReg != RegId::Invalid)
                sprintf_s(ruleText, " cond(op%d)", kv.second.rule.condOp);
            if(kv.second.rule.trace)
                strcat_s(ruleText, " trace");
            if(!kv.second.rule.command.empty())
                strcat_s(ruleText, " do");
        }
        char hitText[32] = "";
        auto hits = mBpHits.find(kv.first);
        if(hits != mBpHits.end() && hits->second)
            sprintf_s(hitText, " hits=%llu", (unsigned long long)hits->second);
        printf("0x%llX  %-8s %-16s DISABLED%s%s%s\n",
               (unsigned long long)kv.first, typeText, detail,
               kv.second.singleshoot ? " once" : "", ruleText, hitText);
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
        if(lb.disabled)
            line += " DISABLED";
        else if(lb.boundAddr)
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

// ---- Enable / disable / edit (P1) ----

bool GleamDebugger::disableBreakpointAt(ptr addr)
{
    if(!mProcess)
        return false;
    // Find the engine's record first: it carries the parameters needed to
    // re-create the breakpoint exactly (DR type/size, memory range).
    const BreakpointInfo* found = nullptr;
    for(const auto & kv : mProcess->breakpoints)
    {
        if(kv.second.address == addr)
        {
            found = &kv.second;
            break;
        }
    }
    if(!found)
        return false;

    DisabledBp saved;
    saved.type = found->type;
    saved.singleshoot = found->singleshoot;
    switch(found->type)
    {
    case BreakpointType::Hardware:
        saved.hwType = found->internal.hardware.type;
        saved.hwSize = found->internal.hardware.size;
        break;
    case BreakpointType::Memory:
        saved.memType = found->internal.memory.type;
        saved.memSize = found->internal.memory.size;
        break;
    default:
        break;
    }
    auto rule = mBpRules.find(addr);
    if(rule != mBpRules.end())
    {
        saved.rule = rule->second;
        saved.hasRule = true;
    }

    // DeleteGenericBreakpoint dispatches on type, so one call covers all
    // three kinds and restores whatever the breakpoint had changed.
    const BreakpointInfo copy = *found;
    if(!mProcess->DeleteGenericBreakpoint(copy))
    {
        printf("failed to disable the breakpoint at 0x%llX\n", (unsigned long long)addr);
        return false;
    }
    mBpRules.erase(addr);
    mDisabledBps[addr] = saved;
    // A bound logical entry must remember it is off, or the next module reload
    // would re-arm it behind the user's back.
    for(auto & lb : mLogicalBps)
    {
        if(lb.boundAddr == addr)
        {
            lb.disabled = true;
            lb.disabledAddr = addr; // so enable can re-claim this exact entry
            lb.boundAddr = 0;
            // boundBase is deliberately KEPT: it is the only link from this
            // parked entry back to its module, and unbindModuleBreakpoints
            // needs it to drop the saved spec when that module unloads. `bl`
            // tests lb.disabled first, so nothing reports it as bound.
        }
    }
    return true;
}

bool GleamDebugger::enableBreakpointAt(ptr addr)
{
    auto it = mDisabledBps.find(addr);
    if(it == mDisabledBps.end() || !mProcess)
        return false;
    const DisabledBp spec = it->second;
    bool ok = false;
    switch(spec.type)
    {
    case BreakpointType::Software:
        ok = mProcess->SetBreakpoint(addr, spec.singleshoot);
        break;
    case BreakpointType::Hardware:
    {
        // The original DR slot was released on disable and may now be taken:
        // ask for a free one rather than assuming the old index.
        if(!mRawDrThreads.empty())
        {
            printf("cannot re-enable a hardware breakpoint after a raw dr write\n");
            return false;
        }
        HardwareSlot slot;
        if(!mProcess->GetFreeHardwareBreakpointSlot(slot))
        {
            printf("cannot re-enable 0x%llX: no free hardware breakpoint slot (4 max)\n",
                   (unsigned long long)addr);
            return false;
        }
        ok = mProcess->SetHardwareBreakpoint(addr, slot, spec.hwType, spec.hwSize,
                                            spec.singleshoot);
        break;
    }
    case BreakpointType::Memory:
        ok = mProcess->SetMemoryBreakpoint(addr, (ptr)spec.memSize, spec.memType,
                                           spec.singleshoot);
        break;
    }
    if(!ok)
    {
        printf("failed to re-enable the breakpoint at 0x%llX\n", (unsigned long long)addr);
        return false;
    }
    if(spec.hasRule)
        mBpRules[addr] = spec.rule;
    mDisabledBps.erase(addr);
    // Re-claim the logical entry that owned this address, by exact address:
    // the symbol and rva forms take the same path, and an ambiguous symbol
    // cannot defeat it. The module is still loaded (we just wrote to it), so
    // the binding is valid immediately.
    for(auto & lb : mLogicalBps)
    {
        if(!lb.disabled || lb.disabledAddr != addr)
            continue;
        lb.disabled = false;
        lb.boundAddr = addr;
        lb.boundBase = 0;
        moduleBaseByName(lb.module, lb.boundBase);
        lb.disabledAddr = 0;
        break;
    }
    return true;
}

bool GleamDebugger::forgetBreakpointState(ptr addr)
{
    const bool wasDisabled = mDisabledBps.erase(addr) != 0;
    mBpRules.erase(addr);
    mIgnoreHits.erase(addr);
    mBpHits.erase(addr);
    return wasDisabled;
}

// Resolve the argument shared by bpdisable/bpenable: an address expression, a
// module-relative spec, or "all".
void GleamDebugger::cmdBpDisable(const std::string & spec)
{
    if(spec == "all")
    {
        std::vector<ptr> targets;
        for(const auto & kv : mProcess->breakpoints)
            targets.push_back(kv.second.address);
        size_t n = 0;
        for(ptr a : targets)
        {
            if(disableBreakpointAt(a))
                n++;
        }
        // Pending (unbound) logical entries have nothing physical to remove,
        // but must still stop binding later.
        size_t pending = 0;
        for(auto & lb : mLogicalBps)
        {
            if(!lb.boundAddr && !lb.disabled)
            {
                lb.disabled = true;
                pending++;
            }
        }
        printf("disabled %zu breakpoint(s)%s\n", n,
               pending ? " (plus pending logical entries)" : "");
        fflush(stdout);
        return;
    }
    uint64_t a = 0;
    if(parseAddress(spec, a))
    {
        if(disableBreakpointAt((ptr)a))
            printf("breakpoint disabled at 0x%llX\n", (unsigned long long)a);
        else if(mDisabledBps.count((ptr)a))
            printf("breakpoint at 0x%llX is already disabled\n", (unsigned long long)a);
        else
            printf("no breakpoint at 0x%llX\n", (unsigned long long)a);
        fflush(stdout);
        return;
    }
    // Not an address: a pending logical spec can still be disabled by name.
    LogicalBp want;
    if(parseLogicalSpec(spec, want))
    {
        mAddrError.clear();
        for(auto & lb : mLogicalBps)
        {
            if(lb.module != want.module || lb.symbol != want.symbol || lb.rva != want.rva)
                continue;
            if(lb.boundAddr)
                disableBreakpointAt(lb.boundAddr);
            lb.disabled = true;
            printf("logical breakpoint disabled module=%s\n", lb.module.c_str());
            fflush(stdout);
            return;
        }
        printf("no logical breakpoint for %s\n", spec.c_str());
        fflush(stdout);
        return;
    }
    printAddrError();
    printf("usage: bpdisable <addr|module!symbol|module+rva|all>\n");
    fflush(stdout);
}

void GleamDebugger::cmdBpEnable(const std::string & spec)
{
    if(spec == "all")
    {
        std::vector<ptr> targets;
        for(const auto & kv : mDisabledBps)
            targets.push_back(kv.first);
        size_t n = 0;
        for(ptr a : targets)
        {
            if(enableBreakpointAt(a))
                n++;
        }
        // Only entries that were never bound are cleared here. An entry whose
        // physical re-enable just FAILED still has disabledAddr set and stays
        // in mDisabledBps; clearing it would let rebindPendingBreakpoints arm
        // it through a different path, contradicting the failure we reported.
        size_t pending = 0;
        for(auto & lb : mLogicalBps)
        {
            if(lb.disabled && lb.disabledAddr == 0)
            {
                lb.disabled = false;
                pending++;
            }
        }
        // Pending entries whose module is loaded can bind right now.
        if(pending)
            rebindPendingBreakpoints();
        printf("enabled %zu breakpoint(s)%s\n", n,
               pending ? " (pending logical entries re-armed)" : "");
        fflush(stdout);
        return;
    }
    uint64_t a = 0;
    if(parseAddress(spec, a))
    {
        if(enableBreakpointAt((ptr)a))
            printf("breakpoint enabled at 0x%llX\n", (unsigned long long)a);
        else if(!mDisabledBps.count((ptr)a))
            printf("no disabled breakpoint at 0x%llX\n", (unsigned long long)a);
        fflush(stdout);
        return;
    }
    LogicalBp want;
    if(parseLogicalSpec(spec, want))
    {
        mAddrError.clear();
        for(auto & lb : mLogicalBps)
        {
            if(lb.module != want.module || lb.symbol != want.symbol || lb.rva != want.rva)
                continue;
            // An entry that was bound when it was disabled has a saved
            // physical spec: it must be restored through enableBreakpointAt,
            // which also consumes the mDisabledBps record. Re-binding it
            // instead would arm the breakpoint AND leave the saved record
            // behind, so `bl` would list the same address twice - once
            // enabled, once DISABLED.
            if(lb.disabledAddr)
            {
                const ptr addr = lb.disabledAddr;
                if(!enableBreakpointAt(addr))
                    return; // reason already reported; entry stays disabled
                printf("logical breakpoint enabled module=%s bound=0x%llX\n",
                       lb.module.c_str(), (unsigned long long)addr);
                fflush(stdout);
                return;
            }
            lb.disabled = false;
            rebindPendingBreakpoints();
            printf("logical breakpoint enabled module=%s%s\n", lb.module.c_str(),
                   lb.boundAddr ? "" : " (still pending)");
            fflush(stdout);
            return;
        }
        printf("no logical breakpoint for %s\n", spec.c_str());
        fflush(stdout);
        return;
    }
    printAddrError();
    printf("usage: bpenable <addr|module!symbol|module+rva|all>\n");
    fflush(stdout);
}

// bpedit <addr> [if <cond>|ifclear] [do <cmd...>|doclear] [trace on|off]
//
// Edits the hit-time rule in place. Only mBpRules changes - the physical
// breakpoint is untouched, so editing never re-writes an int3 and never
// costs a DR slot. A disabled breakpoint can be edited too: the rule is
// stored on its saved spec and applies when it is re-enabled.
void GleamDebugger::cmdBpEdit(const std::vector<std::string> & args)
{
    uint64_t a = 0;
    if(!parseAddress(args[1], a))
    {
        printAddrError();
        printf("usage: bpedit <addr> [if <reg><op><hexval>|ifclear] "
               "[do <command...>|doclear] [trace on|off]\n");
        fflush(stdout);
        return;
    }
    const ptr addr = (ptr)a;
    auto disabled = mDisabledBps.find(addr);
    bool physical = false;
    for(const auto & kv : mProcess->breakpoints)
    {
        if(kv.second.address == addr)
        {
            physical = true;
            break;
        }
    }
    if(!physical && disabled == mDisabledBps.end())
    {
        printf("no breakpoint at 0x%llX\n", (unsigned long long)addr);
        fflush(stdout);
        return;
    }

    // Start from the current rule so an edit is incremental, not a reset.
    BpRule rule;
    if(disabled != mDisabledBps.end() && disabled->second.hasRule)
        rule = disabled->second.rule;
    else
    {
        auto existing = mBpRules.find(addr);
        if(existing != mBpRules.end())
            rule = existing->second;
    }

    bool badArgs = false, changed = false;
    for(size_t i = 2; i < args.size() && !badArgs; i++)
    {
        if(args[i] == "if" && i + 1 < args.size())
        {
            if(!parseCondition(args[i + 1], rule.condReg, rule.condOp, rule.condValue))
                badArgs = true;
            else
                changed = true;
            i++;
        }
        else if(args[i] == "ifclear")
        {
            rule.condReg = RegId::Invalid;
            rule.condOp = 0;
            rule.condValue = 0;
            changed = true;
        }
        else if(args[i] == "doclear")
        {
            rule.command.clear();
            changed = true;
        }
        else if(args[i] == "trace" && i + 1 < args.size() &&
                (args[i + 1] == "on" || args[i + 1] == "off"))
        {
            rule.trace = args[i + 1] == "on";
            changed = true;
            i++;
        }
        else if(args[i] == "do" && i + 1 < args.size())
        {
            // "do" swallows the rest of the line, so it must come last.
            rule.command.clear();
            for(size_t j = i + 1; j < args.size(); j++)
            {
                if(!rule.command.empty())
                    rule.command += ' ';
                rule.command += args[j];
            }
            changed = true;
            break;
        }
        else
            badArgs = true;
    }
    if(badArgs || !changed)
    {
        printf("usage: bpedit <addr> [if <reg><op><hexval>|ifclear] "
               "[do <command...>|doclear] [trace on|off]\n");
        fflush(stdout);
        return;
    }

    const bool empty = rule.condReg == RegId::Invalid && !rule.trace && rule.command.empty();
    if(disabled != mDisabledBps.end())
    {
        disabled->second.rule = rule;
        disabled->second.hasRule = !empty;
    }
    if(physical)
    {
        if(empty)
            mBpRules.erase(addr);
        else
            mBpRules[addr] = rule;
    }
    // Keep a logical entry's rule in step, or a re-bind would restore the old
    // one and silently undo this edit. Matched by ADDRESS in both states:
    // testing `lb.disabled && !physical` instead would hit every disabled
    // logical entry in the list, so editing one would rewrite the rules of all
    // the others.
    for(auto & lb : mLogicalBps)
    {
        if(lb.boundAddr == addr || lb.disabledAddr == addr)
            lb.rule = rule;
    }
    printf("bpedit 0x%llX cond=%s trace=%s do=%s\n",
           (unsigned long long)addr,
           rule.condReg == RegId::Invalid ? "none" : "set",
           rule.trace ? "on" : "off",
           rule.command.empty() ? "none" : rule.command.c_str());
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
            // Re-adding a spec that is currently DISABLED enables it: the
            // caller has already armed the physical breakpoint, so the saved
            // spec must go with it. Leaving it would make `bl` print a phantom
            // DISABLED line that only restart could clear.
            //
            // Only mDisabledBps is dropped here, NOT via forgetBreakpointState:
            // the caller writes mBpRules[addr] from its own command line BEFORE
            // calling us, so clearing rules would silently discard the
            // condition the user just typed on the re-adding `bp`.
            if(e.disabled)
            {
                if(e.disabledAddr)
                    mDisabledBps.erase(e.disabledAddr);
                e.disabled = false;
                e.disabledAddr = 0;
            }
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
        // SYM-2 FIX: Capture ambiguous state from THIS parseAddress call only.
        // Extract the result BEFORE clearing mAddrError to avoid cross-command pollution.
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
            // Drop any logical entry bound at this address too - including one
            // parked by bpdisable, which holds the address in disabledAddr
            // rather than boundAddr.
            for(auto it = mLogicalBps.begin(); it != mLogicalBps.end();)
                it = (it->boundAddr == a || it->disabledAddr == a)
                     ? mLogicalBps.erase(it) : std::next(it);
            // A disabled breakpoint has no physical presence, so DeleteBreakpoint
            // would fail and leave the saved spec behind for bpenable to revive.
            // Dropping the record IS the removal in that case.
            const bool wasDisabled = forgetBreakpointState(a);
            const bool removed = mProcess->DeleteBreakpoint(a) || wasDisabled;
            printf(removed ? "breakpoint removed at 0x%llX\n" : "failed to remove breakpoint at 0x%llX\n", a);
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
        const bool wasDisabled = forgetBreakpointState(a);
        const bool removed = mProcess->DeleteHardwareBreakpoint(a) || wasDisabled;
        printf(removed ? "hardware breakpoint removed at 0x%llX\n" : "failed to remove hardware breakpoint at 0x%llX\n", a);
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
        const bool wasDisabled = forgetBreakpointState(a);
        const bool removed = mProcess->DeleteMemoryBreakpoint(a) || wasDisabled;
        printf(removed ? "memory breakpoint removed at 0x%llX\n" : "failed to remove memory breakpoint at 0x%llX\n", a);
        fflush(stdout);
        return CmdResult::Handled;
    }
    if(cmd == "bl")
    {
        cmdBreakpointList();
        return CmdResult::Handled;
    }
    // Named bpdisable/bpenable rather than bpd/bpe: "hbpd"/"mbpd" already mean
    // DELETE for the hardware/memory kinds, and a three-letter neighbour that
    // means "disable" instead would be a trap.
    if(cmd == "bpdisable" && args.size() == 2)
    {
        cmdBpDisable(args[1]);
        return CmdResult::Handled;
    }
    if(cmd == "bpenable" && args.size() == 2)
    {
        cmdBpEnable(args[1]);
        return CmdResult::Handled;
    }
    if(cmd == "bpedit" && args.size() >= 3)
    {
        cmdBpEdit(args);
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
