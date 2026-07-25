// Execution control commands: continue, stepping, run-to-return, detach,
// quit, thread selection, exception filters.

#include "GleamDebugger.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <GleeBug/Zydis/Zydis.h>

using namespace GleeBug;

#ifdef _WIN64
static const ZydisMachineMode kStepOutMode = ZYDIS_MACHINE_MODE_LONG_64;
#else
static const ZydisMachineMode kStepOutMode = ZYDIS_MACHINE_MODE_LONG_COMPAT_32;
#endif

GleamDebugger::CmdResult GleamDebugger::tryControlCommand(const std::vector<std::string> & args)
{
    const std::string & cmd = args[0];

    if(cmd == "g" || cmd == "continue")
        return CmdResult::Resume;

    if(cmd == "until" && args.size() == 2)
    {
        // Sugar for "bp <addr> once" + continue.
        uint64_t a = 0;
        if(!parseAddress(args[1], a))
        {
            printf("usage: until <addr|module!symbol>\n");
            fflush(stdout);
            return CmdResult::Handled;
        }
        if(!mProcess->SetBreakpoint(a, true))
        {
            printf("failed to set breakpoint at 0x%llX\n", a);
            fflush(stdout);
            return CmdResult::Handled;
        }
        return CmdResult::Resume;
    }

    if(cmd == "step")
    {
        mStepArmed = true;
        currentThread()->StepInto();
        return CmdResult::Resume;
    }

    if(cmd == "tgo" && args.size() >= 2 && args.size() <= 4)
    {
        // tgo <reg><op><hexval> [maxsteps] [log]
        // Conditional tracing: single-step in the core until the condition
        // holds (default cap 0x10000 steps). "log" prints every instruction.
        RegId reg = RegId::Invalid;
        int op = 0;
        uint64_t value = 0, maxSteps = 0x10000;
        bool log = false, badArgs = false;
        if(!parseCondition(args[1], reg, op, value))
            badArgs = true;
        for(size_t i = 2; i < args.size() && !badArgs; i++)
        {
            if(args[i] == "log")
                log = true;
            else if(!parseHex(args[i], maxSteps))
                badArgs = true;
        }
        if(badArgs)
        {
            printf("usage: tgo <reg><==|!=|<|>><hexval> [maxsteps] [log]\n");
            fflush(stdout);
            return CmdResult::Handled;
        }
        mTraceCondReg = reg;
        mTraceCondOp = op;
        mTraceCondValue = value;
        mTraceMax = maxSteps;
        mTraceCount = 0;
        mTraceLog = log;
        mTraceActive = true;
        mStepArmed = true;
        currentThread()->StepInto();
        printf("tracing until %s (max 0x%llX steps)\n", args[1].c_str(), maxSteps);
        fflush(stdout);
        return CmdResult::Resume;
    }

    if(cmd == "stepover" || cmd == "next")
    {
        // Upstream issue #52: Process::thread can be null (e.g. after thread
        // exit events) and the engine's StepOver dereferences it unchecked.
        if(!mProcess->thread)
        {
            printf("stepover unavailable (no current thread)\n");
            fflush(stdout);
            return CmdResult::Handled;
        }
        // StepOver falls back to StepInto for non-call instructions; arm both
        // pause paths and let cbStep/cbBreakpoint disambiguate.
        mStepArmed = true;
        mStepOverArmed = true;
        mProcess->StepOver([this]()
        {
            // Reached via the one-shot breakpoint path on calls; cbBreakpoint
            // has already reported and requested the pause.
        });
        return CmdResult::Resume;
    }

    if(cmd == "ret" || cmd == "stepout")
    {
        // stepout = a core stepping loop with three special cases:
        //   ret            -> execute it, stop in the caller
        //   call           -> one-shot bp after it, full speed (skip)
        //   backward jump  -> one-shot bp at the loop exit, full speed
        // No stack analysis: works on FPO, packed code and shellcode.
        mStepOutActive = true;
        mStepOutSteps = 0;
        if(args.size() == 2)
        {
            uint64_t maxSteps = 0;
            if(!parseHex(args[1], maxSteps) || !maxSteps)
            {
                printf("usage: ret [maxsteps-hex]\n");
                fflush(stdout);
                mStepOutActive = false;
                return CmdResult::Handled;
            }
            mStepOutMax = maxSteps;
        }
        stepOutTick();
        return CmdResult::Resume;
    }

    if(cmd == "detach")
    {
        mQuitting = true;
        // Reclaim stub resources left in the target before letting it go
        // (terminate -> wait -> close -> free, in that order).
        cleanupBreakInStub();
        Detach(); // detach happens at the end of the debug loop iteration
        printf("detaching...\n");
        fflush(stdout);
        return CmdResult::Resume;
    }

    if(cmd == "quit")
    {
        mQuitting = true;
        cleanupBreakInStub();
        Stop();
        return CmdResult::Resume;
    }

    // restart: terminate the target and re-launch it with the same path and
    // args (main.cpp's session loop). Logical breakpoints, exception filters
    // and hide survive; patches/ignore counts/thread selection are cleared.
    if(cmd == "restart" && args.size() == 1)
    {
        if(!mHasLaunchInfo)
        {
            printf("restart requires a launched session (not attach)\n");
            fflush(stdout);
            return CmdResult::Handled;
        }
        printf("restart requested\n");
        fflush(stdout);
        mRestartPending = true;
        mQuitting = true; // no pause injections during shutdown
        cleanupBreakInStub();
        Stop(); // the exit event ends Start(); main.cpp re-Inits
        return CmdResult::Resume;
    }

    if(cmd == "hide")
    {
        bool on = args.size() == 1 || args[1] == "on";
        cmdHide(on);
        return CmdResult::Handled;
    }

    if(cmd == "pause")
    {
        printf("already paused\n");
        fflush(stdout);
        return CmdResult::Handled;
    }

    if(cmd == "thread")
    {
        if(args.size() == 1)
        {
            printf("event thread: %u, selected thread: %s\n",
                   mDebugEvent.dwThreadId,
                   mSelectedThreadId ? std::to_string(mSelectedThreadId).c_str() : "(follow event)");
        }
        else if((args.size() == 2 || args.size() == 3) &&
                (args[args.size() - 1] == "suspend" || args[args.size() - 1] == "resume"))
        {
            // "thread suspend|resume"        -> event thread
            // "thread <tid> suspend|resume"  -> specific thread
            const std::string & op = args[args.size() - 1];
            uint32_t tid = args.size() == 3 ? (uint32_t)strtoul(args[1].c_str(), nullptr, 0)
                                            : mDebugEvent.dwThreadId;
            auto found = mProcess->threads.find(tid);
            if(found == mProcess->threads.end())
                printf("no such thread: %u\n", tid);
            else
            {
                bool ok = op == "suspend" ? found->second->Suspend() : found->second->Resume();
                printf("%s thread %u: %s\n", op.c_str(), tid, ok ? "ok" : "failed");
            }
        }
        else
        {
            uint32_t tid = (uint32_t)strtoul(args[1].c_str(), nullptr, 0);
            if(mProcess->threads.find(tid) != mProcess->threads.end())
            {
                mSelectedThreadId = tid;
                printf("selected thread %u\n", tid);
            }
            else
                printf("no such thread: %s\n", args[1].c_str());
        }
        fflush(stdout);
        return CmdResult::Handled;
    }

    if(cmd == "alloc" && args.size() >= 2 && args.size() <= 3)
    {
        uint64_t size = 0;
        if(!parseHex(args[1], size) || size == 0)
        {
            printf("usage: alloc <hexsize> [rwx]\n");
            fflush(stdout);
            return CmdResult::Handled;
        }
        DWORD prot = PAGE_EXECUTE_READWRITE;
        if(args.size() == 3)
        {
            if(args[2] == "rx") prot = PAGE_EXECUTE_READ;
            else if(args[2] == "rw") prot = PAGE_READWRITE;
            else if(args[2] == "r") prot = PAGE_READONLY;
            else if(args[2] != "rwx")
            {
                printf("usage: alloc <hexsize> [rwx|rx|rw|r]\n");
                fflush(stdout);
                return CmdResult::Handled;
            }
        }
        auto addr = VirtualAllocEx(mProcess->hProcess, nullptr, size, MEM_COMMIT | MEM_RESERVE, prot);
        if(addr)
            printf("allocated 0x%llX (%llu bytes)\n", (unsigned long long)(uintptr_t)addr, (unsigned long long)size);
        else
            printf("VirtualAllocEx failed (%lu)\n", GetLastError());
        fflush(stdout);
        return CmdResult::Handled;
    }

    if(cmd == "free" && args.size() == 2)
    {
        uint64_t a = 0;
        if(!parseAddress(args[1], a))
        {
            printf("usage: free <addr>\n");
            fflush(stdout);
            return CmdResult::Handled;
        }
        printf(VirtualFreeEx(mProcess->hProcess, (LPVOID)a, 0, MEM_RELEASE) ? "freed 0x%llX\n" : "free failed at 0x%llX\n", a);
        fflush(stdout);
        return CmdResult::Handled;
    }

    if(cmd == "protect" && args.size() == 4)
    {
        uint64_t a = 0, b = 0;
        DWORD prot = 0;
        if(!parseAddress(args[1], a) || !parseHex(args[2], b) || b == 0)
        {
            printf("usage: protect <addr> <hexsize> <rwx|rx|rw|r>\n");
            fflush(stdout);
            return CmdResult::Handled;
        }
        if(args[3] == "rwx") prot = PAGE_EXECUTE_READWRITE;
        else if(args[3] == "rx") prot = PAGE_EXECUTE_READ;
        else if(args[3] == "rw") prot = PAGE_READWRITE;
        else if(args[3] == "r") prot = PAGE_READONLY;
        else if(args[3] == "x") prot = PAGE_EXECUTE;
        else
        {
            printf("usage: protect <addr> <hexsize> <rwx|rx|rw|r|x>\n");
            fflush(stdout);
            return CmdResult::Handled;
        }
        DWORD oldProt = 0;
        if(mProcess->MemProtect(a, b, prot, &oldProt))
            printf("protected 0x%llX size 0x%llX (%s)\n", a, b, args[3].c_str());
        else
            printf("protect failed at 0x%llX\n", a);
        fflush(stdout);
        return CmdResult::Handled;
    }

    if(cmd == "ignoreexc" && args.size() == 2)
    {
        uint64_t code = 0;
        if(parseHex(args[1], code) && code <= 0xFFFFFFFF)
        {
            // Sugar for "excfilter add <code> never pass".
            mExFilters[(uint32_t)code] = ExFilter{};
            printf("will pass exception 0x%08llX to the debuggee\n", code);
        }
        else
            printf("usage: ignoreexc <hexcode>\n");
        fflush(stdout);
        return CmdResult::Handled;
    }

    // exception pass|handle: disposition for the CURRENT exception stop.
    if(cmd == "exception" && args.size() == 2)
    {
        if(!mPausedOnException || !mLastExceptionValid)
        {
            printf("not paused on an exception\n");
            fflush(stdout);
            return CmdResult::Handled;
        }
        if(args[1] == "pass")
        {
            mContinueStatus = DBG_EXCEPTION_NOT_HANDLED;
            printf("passing exception 0x%08lX to the debuggee\n", mLastException.ExceptionCode);
            fflush(stdout);
            return CmdResult::Resume;
        }
        if(args[1] == "handle")
        {
            if(!mLastExceptionFirstChance)
                printf("warning: swallowing a second-chance exception\n");
            mContinueStatus = DBG_CONTINUE;
            printf("swallowing exception 0x%08lX\n", mLastException.ExceptionCode);
            fflush(stdout);
            return CmdResult::Resume;
        }
        printf("usage: exception pass|handle\n");
        fflush(stdout);
        return CmdResult::Handled;
    }

    if(cmd == "excfilter")
    {
        if(args.size() == 1)
        {
            if(mExFilters.empty())
                printf("no exception filters\n");
            for(const auto & kv : mExFilters)
                printf("code=0x%08X break=%s handledby=%s\n", kv.first,
                       kv.second.breakOn == 0 ? "first" : kv.second.breakOn == 1 ? "second" : "never",
                       kv.second.handledBy == 1 ? "swallow" : "pass");
            fflush(stdout);
            return CmdResult::Handled;
        }
        if(args[1] == "add" && args.size() >= 3 && args.size() <= 5)
        {
            uint64_t code = 0;
            ExFilter f;
            bool ok = parseHex(args[2], code) && code <= 0xFFFFFFFF;
            for(size_t i = 3; ok && i < args.size(); i++)
            {
                if(args[i] == "first") f.breakOn = 0;
                else if(args[i] == "second") f.breakOn = 1;
                else if(args[i] == "never") f.breakOn = 2;
                else if(args[i] == "pass") f.handledBy = 0;
                else if(args[i] == "swallow") f.handledBy = 1;
                else ok = false;
            }
            if(ok)
            {
                mExFilters[(uint32_t)code] = f;
                printf("exception filter added code=0x%08llX\n", code);
            }
            else
                printf("usage: excfilter add <hexcode> [first|second|never] [pass|swallow]\n");
            fflush(stdout);
            return CmdResult::Handled;
        }
        if(args[1] == "del" && args.size() == 3)
        {
            uint64_t code = 0;
            if(parseHex(args[2], code) && mExFilters.erase((uint32_t)code))
                printf("exception filter removed code=0x%08llX\n", code);
            else
                printf("no exception filter for %s\n", args[2].c_str());
            fflush(stdout);
            return CmdResult::Handled;
        }
        printf("usage: excfilter [add <hexcode> [first|second|never] [pass|swallow] | del <hexcode>]\n");
        fflush(stdout);
        return CmdResult::Handled;
    }

    if(cmd == "breakon" && args.size() <= 3)
    {
        struct SwitchDef { const char* name; bool* flag; };
        SwitchDef switches[] = {
            { "entry", &mBreakOnEntry },
            { "dll", &mBreakOnDll },
            { "thread", &mBreakOnThread },
            { "exception", &mBreakOnException },
        };
        if(args.size() == 1)
        {
            for(const auto & sw : switches)
                printf("breakon %s=%s\n", sw.name, *sw.flag ? "on" : "off");
        }
        else
        {
            bool found = false;
            for(auto & sw : switches)
            {
                if(_stricmp(args[1].c_str(), sw.name) != 0)
                    continue;
                found = true;
                if(args.size() == 2)
                    printf("breakon %s=%s\n", sw.name, *sw.flag ? "on" : "off");
                else if(args[2] == "on")
                {
                    *sw.flag = true;
                    printf("breakon %s=on\n", sw.name);
                    // The entry switch may be enabled after process creation
                    // (e.g. at the system breakpoint): arm the OEP breakpoint now.
                    if(sw.flag == &mBreakOnEntry)
                        applyEntryBreakpoint();
                }
                else if(args[2] == "off")
                {
                    *sw.flag = false;
                    printf("breakon %s=off\n", sw.name);
                    // Disarm a pending OEP breakpoint when entry breaks are off.
                    if(sw.flag == &mBreakOnEntry && mOepBreakpoint)
                    {
                        mProcess->DeleteBreakpoint(mOepBreakpoint);
                        mOepBreakpoint = 0;
                    }
                }
                else
                    printf("usage: breakon <entry|dll|thread|exception> [on|off]\n");
            }
            if(!found)
                printf("unknown switch '%s' (entry|dll|thread|exception)\n", args[1].c_str());
        }
        fflush(stdout);
        return CmdResult::Handled;
    }

    return CmdResult::NotMine;
}

// stepout engine. Every tick inspects the CURRENT instruction (GIP) and
// takes exactly one action:
//   ret            -> finish (we are in the caller now)
//   call           -> one-shot bp after it, resume full speed (skip)
//   backward jump  -> one-shot bp at the loop exit, resume full speed
//   anything else  -> single step
// Ticks come from cbStep (after a step) and cbBreakpoint (after a one-shot
// bp hit) while mStepOutActive is set.
void GleamDebugger::stepOutFinish(const char* reason)
{
    mStepOutActive = false;
    mStepArmed = false;
    char details[96];
    sprintf_s(details, "%s steps=%llu", reason, (unsigned long long)mStepOutSteps);
    emitStop("stepout", details);
    mWantsPause = true;
}

void GleamDebugger::stepOutTick()
{
    // The ret we just executed has landed us in the caller: finish.
    if(mStepOutPending)
    {
        mStepOutPending = false;
        stepOutFinish("return");
        return;
    }
    if(mStepOutSteps >= mStepOutMax)
    {
        stepOutFinish("maxreached");
        return;
    }

    Registers r(currentThread()->hThread);
    auto gip = r.Gip();
    uint8_t data[16];
    ZydisDisassembledInstruction insn;
    bool decoded = mProcess->MemReadSafe(gip, data, sizeof(data)) &&
                   ZYAN_SUCCESS(ZydisDisassembleIntel(kStepOutMode, gip, data, sizeof(data), &insn));
    if(decoded)
    {
        // ret / ret imm16 (also rep ret): execute it, then stop in the caller.
        if(!strcmp(insn.text, "ret") || !strncmp(insn.text, "ret ", 4))
        {
            mStepOutSteps++;
            mStepOutPending = true;
            mStepArmed = true;
            currentThread()->StepInto();
            return;
        }

        // call -> skip it at full speed.
        if(!strncmp(insn.text, "call", 4))
        {
            mStepOutSteps++;
            if(mProcess->SetBreakpoint(gip + insn.info.length, true))
                return; // bp hit -> tick again
            // Fall through to single stepping if the bp cannot be set.
        }
        else
        {
            // Backward jump (loop back edge) -> fast-forward to the loop exit.
            uint64_t target = 0;
            uint8_t len = insn.info.length;
            bool isJump = false;
            if(data[0] == 0xE9 && len == 5) // jmp rel32
            {
                int32_t rel;
                memcpy(&rel, data + 1, 4);
                target = gip + len + (int64_t)rel;
                isJump = true;
            }
            else if(data[0] == 0xEB && len == 2) // jmp rel8
            {
                target = gip + len + (int8_t)data[1];
                isJump = true;
            }
            else if((data[0] & 0xF0) == 0x70 && len == 2) // jcc rel8
            {
                target = gip + len + (int8_t)data[1];
                isJump = true;
            }
            else if(data[0] == 0x0F && (data[1] & 0xF0) == 0x80 && len == 6) // jcc rel32
            {
                int32_t rel;
                memcpy(&rel, data + 2, 4);
                target = gip + len + (int64_t)rel;
                isJump = true;
            }
            if(isJump && target < gip)
            {
                mStepOutSteps++;
                if(mProcess->SetBreakpoint(gip + len, true))
                    return; // loop-exit bp hit -> tick again
            }
        }
    }

    // Default: single step (also the fallback when decoding/bp-setting fails).
    mStepOutSteps++;
    mStepArmed = true;
    currentThread()->StepInto();
}
