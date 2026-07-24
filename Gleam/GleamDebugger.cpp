#include "GleamDebugger.h"

#include <cstdio>

using namespace GleeBug;

bool GleamDebugger::pushCommand(const std::string & cmd)
{
    bool wasEmpty;
    {
        std::lock_guard<std::mutex> lock(mCmdMutex);
        wasEmpty = mCmdQueue.empty();
        mCmdQueue.push(cmd);
    }
    mCmdCv.notify_one();
    return wasEmpty;
}

void GleamDebugger::requestPause()
{
    // Only safe to inject while the debuggee is running free (debugger
    // blocked in WaitForDebugEvent). During event/command processing the
    // request is deferred to just before ContinueDebugEvent instead.
    if(mIsPaused.load() || mInDebugEvent.load())
    {
        mPauseAfterResume.store(true);
        return;
    }
    forceBreakIn();
}

void GleamDebugger::forceBreakIn()
{
    auto process = mProcess;
    if(!process)
        return;

    // A stub break-in is already in flight: don't inject another one (the
    // pending event will arrive and clean itself up).
    if(mBreakInStubThread || mBreakInStubPage)
        return;

    // NOTE: DebugBreakProcess checks PEB.BeingDebugged and refuses to inject
    // when it is cleared (our "hide" does exactly that), so inject our own
    // int3 stub thread instead: int3; jmp $ (loops until we kill it at the
    // resulting pause).
    static const uint8_t stub[] = { 0xCC, 0xEB, 0xFE };
    auto page = VirtualAllocEx(process->hProcess, nullptr, 0x1000,
                               MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if(page && WriteProcessMemory(process->hProcess, page, stub, sizeof(stub), nullptr))
    {
        mBreakInExpected.store(true);
        HANDLE hThread = CreateRemoteThread(process->hProcess, nullptr, 0,
                                            (LPTHREAD_START_ROUTINE)page, nullptr, 0, nullptr);
        if(hThread)
        {
            mBreakInStubPage = page;
            mBreakInStubThread = hThread;
            return;
        }
        VirtualFreeEx(process->hProcess, page, 0, MEM_RELEASE);
    }

    // Fallback (broken when BeingDebugged is hidden, but better than nothing).
    mBreakInExpected.store(true);
    DebugBreakProcess(process->hProcess);
}

bool GleamDebugger::isPaused() const
{
    return mIsPaused.load();
}

void GleamDebugger::pauseAfterResume()
{
    mPauseAfterResume.store(true);
}

Thread* GleamDebugger::currentThread()
{
    if(mSelectedThreadId && mProcess)
    {
        auto found = mProcess->threads.find(mSelectedThreadId);
        if(found != mProcess->threads.end())
            return found->second.get();
    }
    return mThread;
}

// Unified machine-readable stop record:
//   stop reason=<reason> [details...] rip=0x... tid=<id>
// key=value, single line, trivially convertible to JSON by the MCP layer.
void GleamDebugger::emitStop(const char* reason, const char* details) const
{
    uint64_t rip = 0;
    if(mThread)
    {
        Registers r(mThread->hThread);
        rip = r.Gip();
    }
    printf("stop reason=%s%s%s rip=0x%llX tid=%u\n",
           reason,
           details ? " " : "",
           details ? details : "",
           (unsigned long long)rip,
           mDebugEvent.dwThreadId);
    fflush(stdout);
}

void GleamDebugger::applyEntryBreakpoint()
{
    if(mOepBreakpoint || !mProcess)
        return;
    // OEP = image base + AddressOfEntryPoint. NOTE: lpStartAddress is the
    // thread start thunk, NOT the OEP.
    auto oep = moduleEntryPoint((uint64_t)mProcess->createProcessInfo.lpBaseOfImage);
    if(oep && mProcess->SetBreakpoint(oep, true))
        mOepBreakpoint = oep;
    else
    {
        printf("event error msg=\"failed to set OEP breakpoint\"\n");
        fflush(stdout);
    }
}

void GleamDebugger::cbCreateProcessEvent(const CREATE_PROCESS_DEBUG_INFO & createProcess, const Process & process)
{
    printf("event process op=create pid=%u base=0x%p start=0x%p\n",
           mDebugEvent.dwProcessId,
           createProcess.lpBaseOfImage,
           createProcess.lpStartAddress);
    fflush(stdout);

    if(mBreakOnEntry)
        applyEntryBreakpoint();
}

void GleamDebugger::cbExitProcessEvent(const EXIT_PROCESS_DEBUG_INFO & exitProcess, const Process & process)
{
    char details[64];
    sprintf_s(details, "code=0x%08X", exitProcess.dwExitCode);
    emitStop("exit", details);
    closeSymSession();
}

void GleamDebugger::cbCreateThreadEvent(const CREATE_THREAD_DEBUG_INFO & createThread, const Thread & thread)
{
    if(!mBreakOnThread)
        return;
    auto name = symNameByAddr((uint64_t)createThread.lpStartAddress);
    char details[320];
    sprintf_s(details, "op=create start=0x%p name=%s",
              createThread.lpStartAddress,
              name.empty() ? "?" : name.c_str());
    emitStop("thread", details);
    mWantsPause = true;
}

void GleamDebugger::cbExitThreadEvent(const EXIT_THREAD_DEBUG_INFO & exitThread, const Thread & thread)
{
    if(!mBreakOnThread)
        return;
    char details[64];
    sprintf_s(details, "op=exit code=0x%08X", exitThread.dwExitCode);
    emitStop("thread", details);
    mWantsPause = true;
}

void GleamDebugger::cbLoadDllEvent(const LOAD_DLL_DEBUG_INFO & loadDll)
{
    if(!mBreakOnDll)
        return;
    char details[80];
    sprintf_s(details, "op=load base=0x%p", loadDll.lpBaseOfDll);
    emitStop("dll", details);
    mWantsPause = true;
}

void GleamDebugger::cbUnloadDllEvent(const UNLOAD_DLL_DEBUG_INFO & unloadDll)
{
    if(!mBreakOnDll)
        return;
    char details[80];
    sprintf_s(details, "op=unload base=0x%p", unloadDll.lpBaseOfDll);
    emitStop("dll", details);
    mWantsPause = true;
}

void GleamDebugger::cbSystemBreakpoint()
{
    emitStop("system", nullptr);
    // Best moment to hide: no target code has run yet.
    if(mHideOn)
        applyHides();
    mWantsPause = true;
}

void GleamDebugger::cbAttachBreakpoint()
{
    // Fired (instead of the system breakpoint) when attached to a process.
    emitStop("attach", nullptr);
    mWantsPause = true;
}

void GleamDebugger::cbBreakpoint(const BreakpointInfo & info)
{
    // Ignore-count: auto-continue without pausing.
    auto ignore = mIgnoreHits.find(info.address);
    if(ignore != mIgnoreHits.end() && ignore->second > 0)
    {
        ignore->second--;
        printf("event ignored address=0x%llX left=%u\n",
               (unsigned long long)info.address, ignore->second);
        fflush(stdout);
        return;
    }

    // Conditional breakpoints and tracepoints: rule says "don't pause".
    if(!evalBpRule(info))
        return;

    char details[96];
    if(mOepBreakpoint && info.address == mOepBreakpoint && info.singleshoot)
    {
        mOepBreakpoint = 0;
        sprintf_s(details, "address=0x%llX", (unsigned long long)info.address);
        emitStop("entry", details);
    }
    else if(mStepOverArmed && info.singleshoot)
    {
        mStepOverArmed = false;
        emitStop("step", nullptr);
    }
    else
    {
        const char* typeText =
            info.type == BreakpointType::Software ? "software" :
            info.type == BreakpointType::Hardware ? "hardware" : "memory";
        sprintf_s(details, "type=%s address=0x%llX", typeText, (unsigned long long)info.address);
        emitStop("breakpoint", details);
    }
    // One-shot breakpoints are deleted by the engine on hit; drop the
    // associated rule/ignore state so a later breakpoint at the same
    // address does not inherit them.
    if(info.singleshoot)
    {
        mBpRules.erase(info.address);
        mIgnoreHits.erase(info.address);
    }
    mWantsPause = true;
}

void GleamDebugger::cbStep()
{
    // Conditional tracing ("tgo"): keep stepping in the core until the
    // condition holds or the step cap is reached.
    if(mTraceActive)
    {
        mTraceCount++;
        Registers r(mThread->hThread);
        auto rip = r.Gip();
        if(mTraceLog)
        {
            auto text = disasmOne(rip);
            printf("trace rip=0x%llX %s\n", (unsigned long long)rip, text.c_str());
            fflush(stdout);
        }
        bool done = evalCondition(mTraceCondReg, mTraceCondOp, mTraceCondValue);
        bool capped = mTraceCount >= mTraceMax;
        if(done || capped)
        {
            mTraceActive = false;
            mStepArmed = false;
            char details[96];
            sprintf_s(details, "%s steps=%llu", capped && !done ? "maxreached" : "condition",
                      (unsigned long long)mTraceCount);
            emitStop("trace", details);
            mWantsPause = true;
            return;
        }
        currentThread()->StepInto(); // mStepArmed stays armed
        return;
    }

    // Only pause for user-requested steps; GleeBug also steps internally
    // (e.g. to restore software breakpoints).
    if(mStepArmed)
    {
        mStepArmed = false;
        mStepOverArmed = false;
        emitStop("step", nullptr);
        mWantsPause = true;
    }
}

void GleamDebugger::cbUnhandledException(const EXCEPTION_RECORD & exceptionRecord, bool firstChance)
{
    mLastException = exceptionRecord;
    mLastExceptionValid = true;
    mLastExceptionFirstChance = firstChance;

    // Our own break-in (triggered by "pause").
    if(exceptionRecord.ExceptionCode == STATUS_BREAKPOINT && mBreakInExpected.exchange(false))
    {
        mContinueStatus = DBG_CONTINUE;
        // Clean up the injected stub thread if this was our stub break-in.
        if(mBreakInStubThread)
        {
            TerminateThread(mBreakInStubThread, 0);
            CloseHandle(mBreakInStubThread);
            mBreakInStubThread = nullptr;
        }
        if(mBreakInStubPage)
        {
            VirtualFreeEx(mProcess->hProcess, mBreakInStubPage, 0, MEM_RELEASE);
            mBreakInStubPage = nullptr;
        }
        emitStop("pause", nullptr);
        mWantsPause = true;
        return;
    }

    // Filtered exception codes are passed to the debuggee's own handlers
    // (DBG_EXCEPTION_NOT_HANDLED) without pausing - NOT DBG_CONTINUE, which
    // would swallow the exception entirely.
    if(mIgnoredExceptions.count(exceptionRecord.ExceptionCode))
    {
        printf("event exception code=0x%08lX action=passed-to-debuggee\n", exceptionRecord.ExceptionCode);
        fflush(stdout);
        return;
    }

    // Second chance always pauses (last chance before the process dies);
    // first chance follows the "breakon exception" switch.
    if(!firstChance || mBreakOnException)
    {
        char details[128];
        sprintf_s(details, "code=0x%08lX address=0x%p chance=%s",
                  exceptionRecord.ExceptionCode,
                  exceptionRecord.ExceptionAddress,
                  firstChance ? "first" : "second");
        emitStop("exception", details);
        mWantsPause = true;
    }
}

void GleamDebugger::cbInternalError(const std::string & error)
{
    printf("event error msg=\"%s\"\n", error.c_str());
    fflush(stdout);
}

void GleamDebugger::cbPreDebugEvent(const DEBUG_EVENT & debugEvent)
{
    mInDebugEvent.store(true);
}

void GleamDebugger::cbPostDebugEvent(const DEBUG_EVENT & debugEvent)
{
    if(mWantsPause && !mQuitting && mProcess && mThread)
    {
        mWantsPause = false;
        commandLoop();
    }
    mInDebugEvent.store(false);
}

void GleamDebugger::commandLoop()
{
    // The stop record was already emitted by the triggering event; it is the
    // pause notification. Here we only consume commands.
    mIsPaused.store(true);
    for(;;)
    {
        std::string cmd;
        {
            std::unique_lock<std::mutex> lock(mCmdMutex);
            mCmdCv.wait(lock, [this] { return !mCmdQueue.empty(); });
            cmd = mCmdQueue.front();
            mCmdQueue.pop();
        }
        if(executeCommand(cmd))
            break;
    }
    mIsPaused.store(false);
    // A "pause" that arrived while we were paused takes effect right before
    // the resume - the only deterministic injection point.
    if(mPauseAfterResume.exchange(false))
        forceBreakIn();
}
