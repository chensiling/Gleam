// Execution control commands: continue, stepping, run-to-return, detach,
// quit, thread selection, exception filters.

#include "GleamDebugger.h"

#include <cstdio>
#include <cstdlib>

using namespace GleeBug;

GleamDebugger::CmdResult GleamDebugger::tryControlCommand(const std::vector<std::string> & args)
{
    const std::string & cmd = args[0];

    if(cmd == "g" || cmd == "continue")
        return CmdResult::Resume;

    if(cmd == "step")
    {
        mStepArmed = true;
        currentThread()->StepInto();
        return CmdResult::Resume;
    }

    if(cmd == "stepover" || cmd == "next")
    {
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

    return CmdResult::NotMine;
}
