// Execution control commands: continue, stepping, run-to-return, detach,
// quit, thread selection, exception filters.

#include "GleamDebugger.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace GleeBug;

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
        // No frame analysis: the return address sits at [rsp] when stopped
        // inside a function. One-shot breakpoint there, then continue.
        Registers r(currentThread()->hThread);
        ptr rsp = r.Gsp();
        ptr retAddr = 0;
        if(!mProcess->MemReadSafe(rsp, &retAddr, sizeof(retAddr)))
        {
            printf("failed to read the stack at 0x%llX\n", (unsigned long long)rsp);
            fflush(stdout);
            return CmdResult::Handled;
        }
        if(!mProcess->SetBreakpoint(retAddr, true))
        {
            printf("failed to set return breakpoint at 0x%llX\n", (unsigned long long)retAddr);
            fflush(stdout);
            return CmdResult::Handled;
        }
        printf("stepping out to 0x%llX\n", (unsigned long long)retAddr);
        fflush(stdout);
        return CmdResult::Resume;
    }

    if(cmd == "detach")
    {
        mQuitting = true;
        Detach(); // detach happens at the end of the debug loop iteration
        printf("detaching...\n");
        fflush(stdout);
        return CmdResult::Resume;
    }

    if(cmd == "quit")
    {
        mQuitting = true;
        Stop();
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
            mIgnoredExceptions.insert((uint32_t)code);
            printf("will pass exception 0x%08llX to the debuggee\n", code);
        }
        else
            printf("usage: ignoreexc <hexcode>\n");
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
