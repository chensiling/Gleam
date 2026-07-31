#include "Debugger.Thread.h"
#include "Debugger.Thread.Registers.h"

namespace GleeBug
{
    Thread::Thread(HANDLE hThread, uint32 dwThreadId, LPVOID lpThreadLocalBase, LPVOID lpStartAddress) :
        hThread(hThread),
        dwThreadId(dwThreadId),
        lpThreadLocalBase(ptr(lpThreadLocalBase)),
        lpStartAddress(ptr(lpStartAddress)),
        isSingleStepping(false),
        isInternalStepping(false),
        cbInternalStep(nullptr)
    {
    }

    void Thread::StepInto()
    {
        Registers(hThread).TrapFlag.Set();
        isSingleStepping = true;
    }

    void Thread::StepInto(const StepCallback & cbStep)
    {
        StepInto();
        // GI-1: target<void()>() returns nullptr for every lambda (each lambda has a
        // unique anonymous type and is never a plain void(*)()).  Two distinct lambdas
        // both produce nullptr, so the old duplicate check fired on every second lambda
        // and silently dropped it.  std::function has no portable equality test; callers
        // are responsible for not registering the same callback twice.
        stepCallbacks.push_back(cbStep);
    }

    bool Thread::Suspend()
    {
        return SuspendThread(hThread) != -1;
    }

    bool Thread::Resume()
    {
        return ResumeThread(hThread) != -1;
    }
};