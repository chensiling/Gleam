#include "GleamDebugger.h"

#include <cstdio>
#include <psapi.h>

using namespace GleeBug;

namespace
{
    // Normalized module name for a loaded module base ("" on failure).
    std::string moduleNameFromBase(HANDLE hProcess, uint64_t base)
    {
        char path[MAX_PATH] = "";
        if(!GetModuleFileNameExA(hProcess, (HMODULE)base, path, sizeof(path)))
            return std::string();
        return normalizeModuleName(path);
    }
}

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
    // Set-then-check: the request is published first, then both sides race
    // to consume it with exchange(). Whoever wins injects; the loser finds
    // the flag already gone. Once quitting, requests are refused entirely.
    mPauseAfterResume.store(true);
    if(!mIsPaused.load() && !mInDebugEvent.load() && !mQuitting.load())
    {
        if(mPauseAfterResume.exchange(false))
            forceBreakIn();
    }
}

void GleamDebugger::forceBreakIn()
{
    // Serialize against cleanup and the quitting transition: no injection
    // may interleave with a cleanup or start after quitting began.
    std::lock_guard<std::mutex> lock(mBreakInMutex);
    if(mQuitting.load())
    {
        printf("event breakin skip=quitting\n");
        fflush(stdout);
        return;
    }

    auto process = mProcess;
    if(!process)
    {
        // Engine state not ready (no process event processed yet): do NOT
        // drop the request - re-defer it; the next event's consume point
        // will fire the injection with mProcess in place.
        mPauseAfterResume.store(true);
        return;
    }

    // A stub break-in is already in flight: don't inject another one (the
    // pending event will arrive and clean itself up).
    if(mBreakInStubThread.load())
    {
        printf("event breakin skip=in_flight\n");
        fflush(stdout);
        return;
    }

    // NOTE: DebugBreakProcess checks PEB.BeingDebugged and refuses to inject
    // when it is cleared (our "hide" does exactly that), so inject our own
    // stub thread instead. Lifecycle (no publication race):
    //   1. allocate ONE stub page per session (reused, freed at session end)
    //   2. create the thread CREATE_SUSPENDED
    //   3. publish page + handle
    //   4. ResumeThread - only now can the int3 fire
    // The thread self-terminates via call ExitThread; the page is freed at
    // session end, never while a stub thread might still execute on it.
    if(!ensureBreakInStub(process))
        return;

    HANDLE hThread = CreateRemoteThread(process->hProcess, nullptr, 0,
                                        (LPTHREAD_START_ROUTINE)mBreakInStubPage.load(),
                                        nullptr, CREATE_SUSPENDED, nullptr);
    if(!hThread)
    {
        printf("event breakin fail=thread_create err=%lu\n", GetLastError());
        fflush(stdout);
        fallbackDebugBreak(process);
        return;
    }
    mBreakInStubThread.store(hThread);
    if(ResumeThread(hThread) == (DWORD)-1)
    {
        printf("event breakin fail=resume err=%lu\n", GetLastError());
        fflush(stdout);
        // Don't leave a permanently suspended thread in the target.
        TerminateThread(hThread, 0);
        WaitForSingleObject(hThread, 1000);
        CloseHandle(mBreakInStubThread.exchange(nullptr));
        fallbackDebugBreak(process);
        return;
    }
    printf("event breakin injected page=0x%p\n", mBreakInStubPage.load());
    fflush(stdout);
}

void GleamDebugger::cleanupBreakInStub()
{
    std::lock_guard<std::mutex> lock(mBreakInMutex);
    auto hThread = mBreakInStubThread.load();
    if(hThread)
    {
        TerminateThread(hThread, 0);
        // Only after the thread is CONFIRMED dead may the page go away;
        // on timeout keep the handle and the page and say so visibly.
        DWORD wr = WaitForSingleObject(hThread, 1000);
        if(wr != WAIT_OBJECT_0)
        {
            printf("event breakin cleanup wait=0x%lX (thread+page kept)\n", wr);
            fflush(stdout);
            CloseHandle(hThread);
            mBreakInStubThread.store(nullptr);
            return; // page intentionally kept
        }
        CloseHandle(hThread);
        mBreakInStubThread.store(nullptr);
    }
    if(auto page = mBreakInStubPage.exchange(nullptr))
        VirtualFreeEx(mProcess->hProcess, page, 0, MEM_RELEASE);
}

void GleamDebugger::fallbackDebugBreak(GleeBug::Process* process)
{
    // Publish the expectation BEFORE injecting (the event can arrive
    // immediately); clear it again when the API fails, so a later unrelated
    // int3 is not mistaken for our break-in.
    mBreakInExpected.store(true);
    if(!DebugBreakProcess(process->hProcess))
    {
        mBreakInExpected.store(false);
        printf("event breakin fail=debugbreakprocess err=%lu\n", GetLastError());
        fflush(stdout);
    }
}

// Allocate/write the session stub page (once) and resolve ExitThread.
bool GleamDebugger::ensureBreakInStub(GleeBug::Process* process)
{
    if(mBreakInStubPage.load())
        return true;

    uint8_t stub[] = {
        0xCC,                         // int3
        0xB9, 0, 0, 0, 0,             // mov ecx, 0
        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, // mov rax, ExitThread
        0xFF, 0xD0                    // call rax
    };
    uint64_t exitThread = mExitThreadAddr.load();
    if(!exitThread)
    {
        // Retry the resolution - safe only on the debugger thread (dbghelp).
        if(mInDebugEvent.load() && parseAddress("kernel32!ExitThread", exitThread))
            mExitThreadAddr.store(exitThread);
    }
    if(!exitThread)
    {
        printf("event breakin fail=no_exitthread (will retry later)\n");
        fflush(stdout);
        // NOTE: DebugBreakProcess is BeingDebugged-dependent; use only as a
        // last resort and expect a later retry via the stub path.
        fallbackDebugBreak(process);
        return false;
    }
    memcpy(stub + 8, &exitThread, 8);

    auto page = VirtualAllocEx(process->hProcess, nullptr, 0x1000,
                               MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if(!page)
    {
        printf("event breakin fail=alloc err=%lu\n", GetLastError());
        fflush(stdout);
        fallbackDebugBreak(process);
        return false;
    }
    if(!WriteProcessMemory(process->hProcess, page, stub, sizeof(stub), nullptr))
    {
        printf("event breakin fail=write err=%lu\n", GetLastError());
        fflush(stdout);
        VirtualFreeEx(process->hProcess, page, 0, MEM_RELEASE);
        fallbackDebugBreak(process);
        return false;
    }
    mBreakInStubPage.store(page);
    return true;
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

    // Resolve break-in related symbols on the debugger thread (dbghelp is
    // not thread-safe).
    resolveBreakInSymbols();
}

// Resolve (and retry) the addresses needed for stub/fallback break-ins.
void GleamDebugger::resolveBreakInSymbols()
{
    if(!mExitThreadAddr.load())
    {
        uint64_t addr = 0;
        if(parseAddress("kernel32!ExitThread", addr))
            mExitThreadAddr.store(addr);
    }
    if(!mDbgBreakInAddr.load())
    {
        uint64_t addr = 0;
        if(parseAddress("ntdll!DbgUiRemoteBreakin", addr))
            mDbgBreakInAddr.store(addr);
    }
}

void GleamDebugger::cbExitProcessEvent(const EXIT_PROCESS_DEBUG_INFO & exitProcess, const Process & process)
{
    char details[64];
    sprintf_s(details, "code=0x%08X", exitProcess.dwExitCode);
    emitStop("exit", details);
    cleanupBreakInStub();
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
    // Logical breakpoints bind regardless of the breakon dll switch.
    bindModuleBreakpoints((uint64_t)loadDll.lpBaseOfDll);
    if(!mBreakOnDll)
        return;
    char details[80];
    sprintf_s(details, "op=load base=0x%p", loadDll.lpBaseOfDll);
    emitStop("dll", details);
    mWantsPause = true;
}

void GleamDebugger::cbUnloadDllEvent(const UNLOAD_DLL_DEBUG_INFO & unloadDll)
{
    // Unbind keeps the logical entries: a reload re-binds them.
    unbindModuleBreakpoints((uint64_t)unloadDll.lpBaseOfDll);
    // The module's cached .pdata is stale from here on.
    mPdataCache.erase((uint64_t)unloadDll.lpBaseOfDll);
    if(!mBreakOnDll)
        return;
    char details[80];
    sprintf_s(details, "op=unload base=0x%p", unloadDll.lpBaseOfDll);
    emitStop("dll", details);
    mWantsPause = true;
}

// Bind pending module-relative breakpoints whose module just loaded.
void GleamDebugger::bindModuleBreakpoints(uint64_t moduleBase)
{
    if(mLogicalBps.empty() || !mProcess)
        return;
    // During the load event the loader-list APIs are still blind; the PE
    // export directory carries the DLL's own name and needs no loader list.
    std::string name = dllNameFromBase(moduleBase);
    if(name.empty())
        name = moduleNameFromBase(mProcess->hProcess, moduleBase); // fallback
    name = normalizeModuleName(name);
    if(name.empty())
        return;
    for(auto & lb : mLogicalBps)
    {
        if(lb.boundAddr || lb.module != name)
            continue;
        uint64_t addr = 0;
        if(!lb.symbol.empty())
        {
            addr = findExportByName(moduleBase, lb.symbol);
            if(!addr) // dbghelp fallback (PDB-only symbols)
                resolveModuleSymbol(lb.module + "!" + lb.symbol, addr);
            if(!addr)
                continue; // stay pending (symbols may be unavailable)
        }
        else
            addr = moduleBase + lb.rva;
        if(!mProcess->SetBreakpoint(addr, lb.once))
            continue;
        lb.boundAddr = addr;
        lb.boundBase = moduleBase;
        if(lb.rule.condReg != RegId::Invalid || lb.rule.trace || !lb.rule.command.empty())
            mBpRules[addr] = lb.rule;
        printf("event bp bound module=%s address=0x%llX\n",
               name.c_str(), (unsigned long long)addr);
        fflush(stdout);
    }
}

// Unbind breakpoints of an unloading module; the logical entries survive.
void GleamDebugger::unbindModuleBreakpoints(uint64_t moduleBase)
{
    if(mLogicalBps.empty() || !mProcess)
        return;
    for(auto & lb : mLogicalBps)
    {
        if(!lb.boundAddr || lb.boundBase != moduleBase)
            continue;
        // The DLL is still mapped while the unload event is delivered, so the
        // engine can restore the original bytes cleanly. A one-shot entry
        // that already fired simply fails DeleteBreakpoint - harmless.
        mProcess->DeleteBreakpoint(lb.boundAddr);
        mBpRules.erase(lb.boundAddr);
        printf("event bp unbound module=%s address=0x%llX\n",
               lb.module.c_str(), (unsigned long long)lb.boundAddr);
        lb.boundAddr = 0;
        lb.boundBase = 0;
        fflush(stdout);
    }
}

// Try to bind every pending logical breakpoint whose module is already
// loaded. Needed because the main module never fires a load event.
void GleamDebugger::rebindPendingBreakpoints()
{
    if(mLogicalBps.empty() || !mProcess)
        return;
    for(const auto & lb : mLogicalBps)
    {
        if(lb.boundAddr)
            continue;
        uint64_t base = 0;
        if(moduleBaseByName(lb.module, base))
            bindModuleBreakpoints(base);
    }
}

// Clear per-session state before a restart. See the policy comment in
// GleamDebugger.h (what survives vs what is cleared).
void GleamDebugger::resetTransientState()
{
    mSelectedThreadId = 0;
    mLastExceptionValid = false;
    mPausedOnException = false;
    mWantsPause = false;
    mStepArmed = false;
    mStepOverArmed = false;
    mTraceActive = false;
    mStepOutActive = false;
    mStepOutPending = false;
    mIgnoreHits.clear();
    mBpRules.clear();
    mPdataCache.clear();
    mHideOriginals.clear(); // old-process writes are meaningless now
    mOepBreakpoint = 0;
    mBreakInExpected = false;
    mPauseAfterResume = false;
    mExitThreadAddr = 0;
    mDbgBreakInAddr = 0;
    mExitThreadResolveAttempts = 0;
    mQuitting = false; // was set to shut the old session down cleanly
    if(!mPatches.empty())
    {
        // Patches never auto-reapply: the new process must be re-examined
        // and patched deliberately (fingerprint + original-bytes policy).
        printf("patches cleared on restart\n");
        mPatches.clear();
    }
    // Logical breakpoints survive but must re-bind in the new session.
    for(auto & lb : mLogicalBps)
    {
        lb.boundAddr = 0;
        lb.boundBase = 0;
    }
    fflush(stdout);
}

void GleamDebugger::cbSystemBreakpoint()
{
    emitStop("system", nullptr);
    // At the system breakpoint all system DLLs are fully loaded, unlike at
    // process-creation time.
    resolveBreakInSymbols();
    // The main module has no load event: bind its logical breakpoints here
    // (also covers re-binding after a restart).
    rebindPendingBreakpoints();
    // Best moment to hide: no target code has run yet.
    if(mHideOn)
        applyHides();
    mWantsPause = true;
}

void GleamDebugger::cbAttachBreakpoint()
{
    // Fired (instead of the system breakpoint) when attached to a process.
    emitStop("attach", nullptr);
    resolveBreakInSymbols();
    rebindPendingBreakpoints();
    mWantsPause = true;
}

void GleamDebugger::cbBreakpoint(const BreakpointInfo & info)
{
    // Snapshot per-breakpoint state up front. One-shot hits must not leak
    // rules or ignore counts into a later breakpoint at the same address,
    // regardless of which exit path this callback takes.
    BpRule rule;
    const BpRule* rulePtr = nullptr;
    auto ruleIt = mBpRules.find(info.address);
    if(ruleIt != mBpRules.end())
    {
        rule = ruleIt->second;
        rulePtr = &rule;
        if(info.singleshoot)
            mBpRules.erase(ruleIt);
    }
    uint32_t ignoreLeft = 0;
    bool hasIgnore = false;
    auto ignoreIt = mIgnoreHits.find(info.address);
    if(ignoreIt != mIgnoreHits.end())
    {
        hasIgnore = true;
        ignoreLeft = ignoreIt->second;
        if(info.singleshoot)
            mIgnoreHits.erase(ignoreIt);
    }

    // Ignore-count: auto-continue without pausing.
    if(hasIgnore && ignoreLeft > 0)
    {
        if(!info.singleshoot)
            mIgnoreHits[info.address] = ignoreLeft - 1;
        printf("event ignored address=0x%llX left=%u\n",
               (unsigned long long)info.address, ignoreLeft - 1);
        fflush(stdout);
        return;
    }

    // stepout engine: its internal one-shot breakpoints (call skips and loop
    // fast-forwards) drive the next tick instead of pausing.
    if(mStepOutActive && info.singleshoot)
    {
        stepOutTick();
        return;
    }

    // Conditional breakpoints, tracepoints and "do" commands may suppress
    // the pause entirely.
    if(!evalBpRule(info, rulePtr))
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
    mWantsPause = true;
}

void GleamDebugger::cbStep()
{
    // stepout engine: a step landed; inspect what is at GIP now.
    if(mStepOutActive)
    {
        stepOutTick();
        return;
    }

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

    // Our own break-in (triggered by "pause"). Identification, most precise
    // first: the stub's exception address, then the fallback thread's
    // DbgUiRemoteBreakin address, then the fallback flag as last resort.
    const bool isStubBreakIn = exceptionRecord.ExceptionCode == STATUS_BREAKPOINT &&
                               mBreakInStubPage.load() != nullptr &&
                               exceptionRecord.ExceptionAddress == mBreakInStubPage.load();
    const bool isFallbackBreakIn = exceptionRecord.ExceptionCode == STATUS_BREAKPOINT &&
                                   ((mDbgBreakInAddr.load() != 0 &&
                                     (uint64_t)exceptionRecord.ExceptionAddress == mDbgBreakInAddr.load()) ||
                                    mBreakInExpected.exchange(false));
    if(isStubBreakIn || isFallbackBreakIn)
    {
        mContinueStatus = DBG_CONTINUE;
        // The stub thread self-terminates via call ExitThread; kill it as a
        // belt-and-braces. The page is session-scoped and reused, freed at
        // cbExitProcessEvent - never while a stub thread may run on it.
        if(auto hThread = mBreakInStubThread.exchange(nullptr))
        {
            TerminateThread(hThread, 0);
            WaitForSingleObject(hThread, 1000);
            CloseHandle(hThread);
        }
        emitStop("pause", nullptr);
        mWantsPause = true;
        return;
    }

    // Exception filters decide: break at this chance (overrides the breakon
    // switch), or apply the configured disposition without pausing. "pass"
    // keeps the engine default DBG_EXCEPTION_NOT_HANDLED (the debuggee's own
    // handlers run); "swallow" is DBG_CONTINUE.
    bool shouldPause = !firstChance || mBreakOnException;
    auto filter = mExFilters.find(exceptionRecord.ExceptionCode);
    if(filter != mExFilters.end())
    {
        const ExFilter & f = filter->second;
        const bool breakNow = (f.breakOn == 0 && firstChance) || (f.breakOn == 1 && !firstChance);
        if(breakNow)
            shouldPause = true;
        else
        {
            if(f.handledBy == 1)
                mContinueStatus = DBG_CONTINUE;
            printf("event exception code=0x%08lX action=%s\n",
                   exceptionRecord.ExceptionCode,
                   f.handledBy == 1 ? "swallowed" : "passed-to-debuggee");
            fflush(stdout);
            return;
        }
    }

    // Second chance always pauses (last chance before the process dies);
    // first chance follows the "breakon exception" switch.
    if(shouldPause)
    {
        char details[128];
        sprintf_s(details, "code=0x%08lX address=0x%p chance=%s",
                  exceptionRecord.ExceptionCode,
                  exceptionRecord.ExceptionAddress,
                  firstChance ? "first" : "second");
        emitStop("exception", details);
        mPausedOnException = true;
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
    // Event-opportunity retry for break-in symbol resolution (rate-limited
    // logging; the pending state is simply "address still zero").
    if(!mExitThreadAddr.load() || !mDbgBreakInAddr.load())
    {
        if(mExitThreadResolveAttempts++ % 32 == 0)
        {
            printf("event breakin resolve retry=%u\n", mExitThreadResolveAttempts);
            fflush(stdout);
        }
        resolveBreakInSymbols();
    }

    if(mWantsPause && !mQuitting.load() && mProcess && mThread)
    {
        mWantsPause = false;
        commandLoop();
    }
    // Consume deferred pause requests LAST, after marking ourselves
    // running-free: requests arriving after this point go straight to
    // forceBreakIn, so no request can be stranded between the two checks.
    // When quitting (detach/quit in flight), stale requests are DISCARDED:
    // injecting a stub into a target we are letting go would crash it.
    mInDebugEvent.store(false);
    if(mQuitting.load())
        mPauseAfterResume.store(false);
    else if(mPauseAfterResume.exchange(false))
    {
        printf("event breakin deferred-fire\n");
        fflush(stdout);
        forceBreakIn();
    }
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
}
