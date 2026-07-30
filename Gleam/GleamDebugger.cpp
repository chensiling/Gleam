#include "GleamDebugger.h"
#include "Log.h"
#include "Logger.h"
#include "PerfMonitor.h"
#include "Constants.h"

#include <cstdio>
#include <psapi.h>
#include <dbghelp.h>

using namespace GleeBug;

// GleamDebugger constructor: create Logger and PerfMonitor instances
// and set them as the global defaults (for transition period)
GleamDebugger::GleamDebugger() {
    mLogger = new Gleam::Logger(Gleam::LogLevel::Info);
    mPerfMonitor = new Gleam::PerfMonitor();

    // Set as global defaults (transition period)
    Gleam::g_defaultLogger = mLogger;
    Gleam::g_defaultPerfMonitor = mPerfMonitor;
}

// GleamDebugger destructor: clean up Logger and PerfMonitor
GleamDebugger::~GleamDebugger() {
    // Clear global defaults before destruction
    if (Gleam::g_defaultLogger == mLogger) {
        Gleam::g_defaultLogger = nullptr;
    }
    if (Gleam::g_defaultPerfMonitor == mPerfMonitor) {
        Gleam::g_defaultPerfMonitor = nullptr;
    }

    delete mLogger;
    delete mPerfMonitor;
}

// Access to logger
Gleam::Logger& GleamDebugger::logger() {
    return *mLogger;
}

// Access to perf monitor
Gleam::PerfMonitor& GleamDebugger::perfMonitor() {
    return *mPerfMonitor;
}

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
        Gleam::logEvent("breakin skip=quitting");
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
        Gleam::logEvent("breakin skip=in_flight");
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

    DWORD stubTid = 0;
    HANDLE hThread = CreateRemoteThread(process->hProcess, nullptr, 0,
                                        (LPTHREAD_START_ROUTINE)mBreakInStubPage.load(),
                                        nullptr, CREATE_SUSPENDED, &stubTid);
    if(!hThread)
    {
        Gleam::logEvent("breakin fail=thread_create err=%lu", GetLastError());
        fallbackDebugBreak(process);
        return;
    }
    mBreakInStubTid.store(stubTid);
    mBreakInStubThread.store(hThread);
    bool resumed;
    if(mFailNextStubResume)
    {
        mFailNextStubResume = false; // injected failure (selftest)
        resumed = false;
        SetLastError(ERROR_ACCESS_DENIED);
    }
    else
    {
        resumed = ResumeThread(hThread) != (DWORD)-1;
    }
    if(!resumed)
    {
        Gleam::logEvent("breakin fail=resume err=%lu", GetLastError());
        // Don't leave a permanently suspended thread in the target - but
        // only close the handle when death is CONFIRMED; an unconfirmed
        // thread keeps handle + tid + page registered (the deferred detach
        // cleanup and cbExitThreadEvent keep tracking it).
        bool terminated = TerminateThread(hThread, 0) != 0;
        if(!terminated)
        {
            Gleam::logEvent("breakin fail=terminate err=%lu", GetLastError());
        }
        else if(WaitForSingleObject(hThread, Gleam::Limits::THREAD_WAIT_TIMEOUT_MS) == WAIT_OBJECT_0)
        {
            CloseHandle(mBreakInStubThread.exchange(nullptr));
            mBreakInStubTid.store(0);
        }
        else
        {
            Gleam::logEvent("breakin fail=terminate_wait (thread+page kept)");
        }
        // C3-R6 FIX: When hide is off, use DebugBreakProcess fallback.
        // When hide is on, DebugBreakProcess would fail (PEB.BeingDebugged cleared),
        // so we defer to the next natural event instead and re-arm mPauseAfterResume.
        if(!mHideOn)
        {
            fallbackDebugBreak(process);
        }
        else
        {
            // Under hide-on, DebugBreakProcess checks PEB.BeingDebugged (cleared)
            // and fails. Re-arm the pause request; it will be consumed at the next
            // natural debug event (exception, DLL load, thread create, etc).
            mPauseAfterResume.store(true);
            Gleam::logEvent("breakin deferred: waiting for next target event under hide on");
        }
        return;
    }
    Gleam::logEvent("breakin injected page=0x%p", mBreakInStubPage.load());
}

// Frees the stub page through the (injectable) VirtualFreeEx path. Only a
// SUCCESSFUL free clears the tracked address - a failure keeps it so the
// next cleanup round can retry. Returns true when no page remains tracked.
bool GleamDebugger::freeBreakInStubPage()
{
    auto page = mBreakInStubPage.load();
    if(!page)
        return true;
    bool freed;
    if(mFailNextVfree)
    {
        mFailNextVfree = false; // injected failure (selftest)
        freed = false;
        SetLastError(ERROR_ACCESS_DENIED);
    }
    else
    {
        freed = VirtualFreeEx(mProcess->hProcess, page, 0, MEM_RELEASE) != 0;
    }
    if(!freed)
    {
        Gleam::logEvent("error msg=\"Gleam: break-in stub page free failed for 0x%p (error %lu)\"",
               page, GetLastError());
        return false; // page address KEPT for the retry
    }
    mBreakInStubPage.store(nullptr);
    Gleam::logEvent("breakin stub freed page=0x%p", page);
    return true;
}

bool GleamDebugger::cleanupBreakInStub()
{
    // Request stub teardown WITHOUT ever losing track of it. The thread is
    // hastened with TerminateThread; only a CONFIRMED death (or the thread's
    // EXIT_THREAD event, see cbExitThreadEvent) releases the handle, and
    // only a released handle lets the page go. If the thread cannot be
    // confirmed dead right now (a held debug event freezes the whole
    // process, so any wait inside a pause times out by construction), the
    // handle and page stay registered: the detach path defers on them, and
    // quit/restart paths reclaim them when the target dies.
    //
    // Returns true only when NOTHING remains tracked in the target (no live
    // thread, no held page) - the detach command refuses on false.
    std::lock_guard<std::mutex> lock(mBreakInMutex);
    if(auto hThread = mBreakInStubThread.load())
    {
        // C3-R6 FIX: Keep trying to terminate until confirmed dead or we give up.
        // Don't limit to 16 INT3s - keep the thread tracked until death is confirmed.
        int retries = 0;
        const int maxRetries = Gleam::Limits::MAX_TERMINATE_RETRIES;
        while(retries < maxRetries)
        {
            if(!TerminateThread(hThread, 0))
            {
                Gleam::logEvent("error msg=\"Gleam: TerminateThread failed for break-in stub tid %u (error %lu, retry %d)\"",
                       mBreakInStubTid.load(), GetLastError(), retries);
                retries++;
                Sleep(Gleam::Limits::TERMINATE_RETRY_DELAY_MS);
                continue;
            }
            // Termination call succeeded, check if thread is dead
            if(WaitForSingleObject(hThread, Gleam::Limits::SHORT_THREAD_WAIT_MS) == WAIT_OBJECT_0)
            {
                CloseHandle(hThread);
                mBreakInStubThread.store(nullptr);
                mBreakInStubTid.store(0);
                break;
            }
            // Thread not dead yet, retry termination
            retries++;
            if(retries < maxRetries)
                Sleep(Gleam::Limits::TERMINATE_RETRY_DELAY_MS);
        }
        if(retries >= maxRetries)
        {
            Gleam::logEvent("error msg=\"Gleam: break-in stub thread %u could not be terminated after %d retries (handle+page kept)\"",
                   mBreakInStubTid.load(), maxRetries);
        }
    }
    if(mBreakInStubThread.load())
        return false; // unconfirmed thread: handle + page stay registered
    return freeBreakInStubPage();
}

void GleamDebugger::finishDeferredDetach()
{
    // The stub thread is confirmed dead (its EXIT_THREAD event arrived), so
    // the page can never be executed again. Free it BEFORE letting go: a
    // detach must not leave a remote RWX page in a surviving target.
    {
        std::lock_guard<std::mutex> lock(mBreakInMutex);
        if(freeBreakInStubPage())
        {
            Detach(); // detach happens at the end of the debug loop iteration
            Gleam::logInfo(Gleam::Strings::DETACHING);
            return;
        }
    }
    // The free failed: refuse the detach and stay attached - with the page
    // address KEPT, so a later detach command can retry the free. The lock
    // is released before forceBreakIn (it takes mBreakInMutex itself).
    // mWantsPause cannot hand control back at THIS event: an EXIT_THREAD
    // event always has mThread == nullptr (exitThreadEvent clears it), so
    // the command loop gate would fail and the session would hang on a
    // quiet target. Manufacture the re-entry event instead (same as
    // cbDetachRefused).
    mQuitting = false;
    mWantsPause = true;
    Gleam::logError(Gleam::Strings::DETACH_REFUSED_PAGE);
    forceBreakIn();
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
        Gleam::logEvent("breakin fail=debugbreakprocess err=%lu", GetLastError());
    }
}

// Allocate/write the session stub page (once). The stub is pure int3s: the
// stub thread is ALWAYS terminated by the debugger at its int3 stop (see
// the break-in block in cbExceptionEvent), so it never executes past the
// first byte and never needs a resolved ExitThread address. That dependency
// used to make break-in unavailable when the resolution failed - and a
// WRONG resolution turned the stub's "call ExitThread" into a target crash.
bool GleamDebugger::ensureBreakInStub(GleeBug::Process* process)
{
    if(mBreakInStubPage.load())
        return true;

    uint8_t stub[Gleam::Memory::BREAK_IN_STUB_SIZE];
    memset(stub, 0xCC, sizeof(stub)); // int3 ...
    auto page = VirtualAllocEx(process->hProcess, nullptr, Gleam::Memory::BREAK_IN_PAGE_SIZE,
                               MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if(!page)
    {
        Gleam::logEvent("breakin fail=alloc err=%lu", GetLastError());
        fallbackDebugBreak(process);
        return false;
    }
    // C3-R5-R FIX: Register the page IMMEDIATELY after allocation, before any
    // operation that can fail. This ensures the page is always trackable even
    // if WriteProcessMemory or VirtualFreeEx fails.
    mBreakInStubPage.store(page);

    if(!WriteProcessMemory(process->hProcess, page, stub, sizeof(stub), nullptr))
    {
        Gleam::logEvent("breakin fail=write err=%lu", GetLastError());
        // Try to free immediately; if that fails, the page is already registered
        // in mBreakInStubPage and freeBreakInStubPage can retry later.
        if(!VirtualFreeEx(process->hProcess, page, 0, MEM_RELEASE))
        {
            Gleam::logEvent("breakin fail=free err=%lu (page retained for retry)", GetLastError());
            // Page stays registered for cleanup retry
        }
        else
        {
            // Successfully freed; clear the registration
            mBreakInStubPage.store(nullptr);
        }
        fallbackDebugBreak(process);
        return false;
    }
    // Page is already registered above; write succeeded
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
        // A stale selection must never silently redirect commands at the
        // event thread (P0-6): report and let callers refuse to act.
        Gleam::logWarn("selected thread %u no longer exists (use 'thread' to reselect)",
               mSelectedThreadId);
        return nullptr;
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
    Gleam::logStop(reason, details, rip, mDebugEvent.dwThreadId);
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
        Gleam::logEvent("error msg=\"failed to set OEP breakpoint\"");
    }
}

void GleamDebugger::cbCreateProcessEvent(const CREATE_PROCESS_DEBUG_INFO & createProcess, const Process & process)
{
    Gleam::logEvent("process op=create pid=%u base=0x%p start=0x%p",
           mDebugEvent.dwProcessId,
           createProcess.lpBaseOfImage,
           createProcess.lpStartAddress);

    if(mBreakOnEntry)
        applyEntryBreakpoint();

    // Resolve break-in related symbols on the debugger thread (dbghelp is
    // not thread-safe).
    resolveBreakInSymbols();
}

// Resolve (and retry) the address needed to identify the fallback break-in.
void GleamDebugger::resolveBreakInSymbols()
{
    if(!mDbgBreakInAddr.load())
    {
        uint64_t addr = 0;
        if(parseAddress("ntdll!DbgUiRemoteBreakin", addr))
            mDbgBreakInAddr.store(addr);
    }
}

void GleamDebugger::cbExitProcessEvent(const EXIT_PROCESS_DEBUG_INFO & exitProcess, const Process & process)
{
    char details[Gleam::Memory::SMALL_DETAIL_BUFFER];
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
    mRawDrThreads.erase(mDebugEvent.dwThreadId); // raw-DR ownership dies with the thread
    // Break-in stub thread exited: the kernel reports the exit only after
    // the thread's last user-mode instruction, so it can never touch the
    // stub page again - this is the death confirmation the deferred detach
    // cleanup waits for (a WaitForSingleObject inside a pause could never
    // succeed: a held debug event freezes the whole process).
    if(mBreakInStubTid.load() != 0 && mDebugEvent.dwThreadId == mBreakInStubTid.load())
    {
        if(auto hThread = mBreakInStubThread.exchange(nullptr))
            CloseHandle(hThread);
        mBreakInStubTid.store(0);
        if(mDetachAfterStubCleanup)
        {
            mDetachAfterStubCleanup = false;
            finishDeferredDetach();
        }
    }
    // The thread a stepout operation owns is gone: cancel it.
    if(mStepOutActive && mDebugEvent.dwThreadId == mStepOutTid)
        abortStepOut("thread exit");
    if(!mBreakOnThread)
        return;
    char details[64];
    sprintf_s(details, "op=exit code=0x%08X", exitThread.dwExitCode);
    emitStop("thread", details);
    mWantsPause = true;
}

void GleamDebugger::cbLoadDllEvent(const LOAD_DLL_DEBUG_INFO & loadDll)
{
    // Module identity, authoritative first: the real path from the event's
    // file handle (valid during this callback), then the loader list. The
    // PE export-directory name is only an alias (bindModuleBreakpoints).
    std::string name;
    wchar_t wpath[MAX_PATH * 2] = L"";
    if(loadDll.hFile)
    {
        char path[MAX_PATH * 2] = "";
        if(GetFinalPathNameByHandleA(loadDll.hFile, path, sizeof(path), VOLUME_NAME_DOS))
            name = normalizeModuleName(path);
        if(GetFinalPathNameByHandleW(loadDll.hFile, wpath, ARRAYSIZE(wpath), VOLUME_NAME_DOS) && !name.empty())
            mModulePaths[name] = wpath; // later events for this DLL may have hFile == NULL
    }
    // Logical breakpoints bind regardless of the breakon dll switch.
    bindModuleBreakpoints((uint64_t)loadDll.lpBaseOfDll, name, wpath);
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
    // Pair the explicit symbol load, if any (stale symbols on reload).
    if(mSymLoadedBases.erase((uint64_t)unloadDll.lpBaseOfDll) && mProcess)
    {
        ensureSymSession();
        SymUnloadModule64(mProcess->hProcess, (DWORD64)unloadDll.lpBaseOfDll);
    }
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
// primaryName: authoritative identity (real path / loader list), possibly
// empty. The export-directory name is tried only as a non-authoritative
// alias. Symbol entries that fail to resolve stay pending and are retried
// at the next module event or pause (rebindPendingBreakpoints).
void GleamDebugger::bindModuleBreakpoints(uint64_t moduleBase, const std::string & primaryName,
                                          const wchar_t* imagePath)
{
    if(mLogicalBps.empty() || !mProcess)
        return;
    std::string name = primaryName;
    if(name.empty())
        name = moduleNameFromBase(mProcess->hProcess, moduleBase);
    const std::string alias = normalizeModuleName(dllNameFromBase(moduleBase));
    for(size_t i = 0; i < mLogicalBps.size(); )
    {
        auto & lb = mLogicalBps[i];
        if(lb.boundAddr)
        {
            i++;
            continue;
        }
        bool match = (!name.empty() && lb.module == name) ||
                     (!alias.empty() && lb.module == alias);
        // Identity fallback for anonymous events (hFile NULL, no export
        // name): prove the loaded module IS the pending entry's file by
        // comparing CodeView GUID+Age - never by resolving a symbol (that
        // can be arranged for any image).
        if(!match && name.empty() && alias.empty() && !lb.symbol.empty())
        {
            auto foundPath = mModulePaths.find(lb.module);
            if(foundPath != mModulePaths.end() &&
               verifyModuleIdentity(moduleBase, foundPath->second.c_str()))
                match = true;
        }
        if(!match)
        {
            i++;
            continue;
        }
        uint64_t addr = 0;
        bool ambiguous = false;
        if(!lb.symbol.empty())
        {
            addr = findExportByName(moduleBase, lb.symbol);
            SymbolResult sr = SymbolResult::NotFound;
            if(!addr) // invade-session fallback (needs the loader list)
                sr = resolveModuleSymbol(lb.module + "!" + lb.symbol, addr);
            if(!addr && sr != SymbolResult::Ambiguous) // PDB-only fallback
            {
                const wchar_t* path = imagePath && *imagePath ? imagePath : nullptr;
                if(!path)
                {
                    auto found = mModulePaths.find(lb.module);
                    if(found != mModulePaths.end())
                        path = found->second.c_str();
                }
                sr = resolvePdbSymbol(moduleBase, path, lb.symbol, addr);
            }
            // Ambiguous symbols must NEVER bind (not now, not as pending).
            // The refusal was already printed by resolve*Symbol.
            if(sr == SymbolResult::Ambiguous)
            {
                Gleam::logEvent("bp rejected module=%s symbol=%s (ambiguous)",
                       lb.module.c_str(), lb.symbol.c_str());
                mLogicalBps.erase(mLogicalBps.begin() + i);
                continue;
            }
            if(!addr)
            {
                i++;
                continue; // stay pending, retried at the next event/pause
            }
        }
        else
        {
            // RVA form: validate against the PE read directly at moduleBase
            // (no loader-list dependency - the list is blind during the
            // load event). If the PE cannot be read we cannot confirm yet:
            // stay pending instead of rejecting a possibly-valid entry.
            const uint32_t imageSize = moduleImageSize(moduleBase);
            if(!imageSize)
            {
                i++;
                continue;
            }
            if(lb.rva >= imageSize || moduleBase + lb.rva < moduleBase)
            {
                Gleam::logEvent("bp rejected module=%s rva=0x%llX (out of image)",
                       lb.module.c_str(), (unsigned long long)lb.rva);
                mLogicalBps.erase(mLogicalBps.begin() + i);
                continue;
            }
            addr = moduleBase + lb.rva;
        }
        if(!mProcess->SetBreakpoint(addr, lb.once))
        {
            i++;
            continue;
        }
        lb.boundAddr = addr;
        lb.boundBase = moduleBase;
        if(lb.rule.condReg != RegId::Invalid || lb.rule.trace || !lb.rule.command.empty())
            mBpRules[addr] = lb.rule;
        Gleam::logEvent("bp bound module=%s address=0x%llX",
               lb.module.c_str(), (unsigned long long)addr);
        i++;
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
        // The DLL is still mapped while the unload event is delivered, but
        // only READS are guaranteed - restoring the original byte can fail.
        // That failure is harmless (the mapping is going away anyway), so
        // fall back to dropping the engine bookkeeping directly, or the
        // stale entry blocks re-binding when the module reloads.
        if(!mProcess->DeleteBreakpoint(lb.boundAddr))
        {
            mProcess->breakpoints.erase({ BreakpointType::Software, lb.boundAddr });
            mProcess->softwareBreakpointReferences.erase(lb.boundAddr);
        }
        mBpRules.erase(lb.boundAddr);
        Gleam::logEvent("bp unbound module=%s address=0x%llX",
               lb.module.c_str(), (unsigned long long)lb.boundAddr);
        lb.boundAddr = 0;
        lb.boundBase = 0;
    }
}

// Try to bind every pending logical breakpoint whose module is already
// loaded. Needed because the main module never fires a load event.
void GleamDebugger::rebindPendingBreakpoints()
{
    if(mLogicalBps.empty() || !mProcess)
        return;
    // Collect first: binding may erase entries, which would invalidate
    // iteration over mLogicalBps.
    std::vector<uint64_t> bases;
    for(const auto & lb : mLogicalBps)
    {
        if(lb.boundAddr)
            continue;
        uint64_t base = 0;
        if(moduleBaseByName(lb.module, base))
            bases.push_back(base);
    }
    for(uint64_t base : bases)
    {
        wchar_t wpath[MAX_PATH * 2] = L"";
        GetModuleFileNameExW(mProcess->hProcess, (HMODULE)base, wpath, ARRAYSIZE(wpath));
        bindModuleBreakpoints(base, std::string(), wpath);
    }
}

// Clear per-session state before a restart. See the policy comment in
// GleamDebugger.h (what survives vs what is cleared).
void GleamDebugger::resetTransientState()
{
    mSelectedThreadId = 0;
    mLastExceptionValid = false;
    mPausedOnException = false;
    mRawDrThreads.clear();
    mWantsPause = false;
    mStepArmed = false;
    mStepOverArmed = false;
    mTraceActive = false;
    // Every PER-OPERATION stepout field returns to its initial value. The old
    // process is gone, so there is no int3 left to delete - only bookkeeping.
    // mStepOutGen is deliberately NOT reset: it is a monotonic counter, and
    // keeping it monotonic across sessions is what makes a stale generation
    // impossible to mistake for the current one.
    mStepOutActive = false;
    mStepOutPending = false;
    mStepOutSteps = 0;
    mStepOutMax = 0x40000;
    mStepOutBpAddr = 0;
    mStepOutBpOurs = false;
    mStepOutTid = 0;
    mStepOutBpGen = 0;
    mStepOutRearmPending = 0;
    mStepOutRearm = 0;
    mIgnoreHits.clear();
    mBpRules.clear();
    mPdataCache.clear();
    mSymLoadedBases.clear(); // explicit symbol loads die with the old process
    mModulePaths.clear();    // image paths are per-process-session
    mHideOriginals.clear(); // old-process writes are meaningless now
    mOepBreakpoint = 0;
    mBreakInExpected = false;
    mPauseAfterResume = false;
    // Break-in stub records point into the OLD (now dead) process: the OS
    // reclaimed the page and the thread, so just drop our records - no
    // VirtualFreeEx (the address space is gone).
    if(auto hThread = mBreakInStubThread.exchange(nullptr))
        CloseHandle(hThread);
    mBreakInStubTid = 0;
    mBreakInStubPage = nullptr;
    mDetachAfterStubCleanup = false;
    // Fault-injection flags die with the old session (one-shot by design,
    // but an armed-but-unfired flag must not leak into the restart).
    mFailNextTerminate = false;
    mFailNextStubResume = false;
    mFailNextVfree = false;
    mDbgBreakInAddr = 0;
    mExitThreadResolveAttempts = 0;
    mQuitting = false; // was set to shut the old session down cleanly
    // Fault-injection hooks are per-process statics: clear them here so an
    // armed-but-never-fired hook cannot leak into the restarted session.
    // (A fired hook already disarmed itself - see failapi* in
    // GleamCommands.Symbols.cpp.)
    mTestHookWaitForDebugEvent = nullptr;
    mTestHookContinueDebugEvent = nullptr;
    mTestHookResumeThread = nullptr;
    if(!mPatches.empty())
    {
        Gleam::logInfo(Gleam::Strings::PATCHES_CLEARED);
        mPatches.clear();
    }
    // Logical breakpoints survive but must re-bind in the new session.
    for(auto & lb : mLogicalBps)
    {
        lb.boundAddr = 0;
        lb.boundBase = 0;
    }
    // Clear caches on restart (new process)
    clearIltCache();
    clearSymbolCache();
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

// Internal (stepout) breakpoint bookkeeping for a breakpoint hit.
//
// This MUST run before any user-visible state handling. The engine deletes a
// one-shot breakpoint after this callback returns no matter which path the
// callback took, so an early return (ignore counts, conditional rules) before
// this point would leave mStepOutActive set while the physical int3 is gone -
// the operation would then wait forever and the target runs to exit.
//
// Returns true when the OWNER consumed the hit: a tick was already driven and
// the event must not surface as a user stop.
bool GleamDebugger::handleStepOutBreakpoint(const BreakpointInfo & info)
{
    // The hit is ours only if it is still physically OUR breakpoint: after
    // any consumption (mStepOutBpOurs=false) the address may carry a user
    // breakpoint, which must keep its normal semantics.
    if(!mStepOutActive || !info.singleshoot || mStepOutBpAddr == 0 ||
       !mStepOutBpOurs || info.address != mStepOutBpAddr)
        return false;

    // ANY hit at that address costs us the right to delete it: the engine
    // deletes the one-shot after this callback, so from here on the address
    // may carry a USER breakpoint instead. Clear ownership before branching,
    // so no path can leave a stale delete right behind.
    mStepOutBpOurs = false;

    // Stale generation (hit by ANY thread): the physical bp is gone and the
    // operation has nothing left to wait on. Converge to an error finish
    // BEFORE the owner/non-owner split, so a stale hit can never register a
    // deferred re-arm - that would write an int3 back for a dead generation
    // and set mStepOutBpOurs for an operation that no longer owns it.
    if(mStepOutBpGen != mStepOutGen)
    {
        mStepOutBpAddr = 0;
        stepOutFinish("error");
        return true; // consumed; stop record was emitted by stepOutFinish
    }

    // Owner thread, owning generation: drive the next tick.
    if(mDebugEvent.dwThreadId == mStepOutTid)
    {
        mStepOutBpAddr = 0;
        stepOutTick();
        return true;
    }

    // Another thread tripped the internal breakpoint at that address. The
    // engine restores the original byte and re-executes it with an internal
    // step AFTER this event, so arming a replacement now would make the
    // non-owner trip it again. Two-stage: register here, arm at the NEXT
    // event (cbPostDebugEvent). The hit surfaces as a normal stop.
    mStepOutRearmPending = mStepOutBpAddr;
    Gleam::logEvent("stepout internal bp hit by non-owner tid=%u (re-arm deferred)",
           mDebugEvent.dwThreadId);
    return false;
}

void GleamDebugger::cbBreakpoint(const BreakpointInfo & info)
{
    // stepout bookkeeping FIRST: the engine consumes the one-shot breakpoint
    // regardless of how this callback exits, so ownership hand-off, tick
    // advancement and deferred re-arming must not sit behind a user-state
    // early return (ignore counts, conditional rules).
    if(handleStepOutBreakpoint(info))
        return;

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
    // A fired one-shot logical breakpoint is consumed: drop the logical
    // entry so bl/restart/reload see the true state.
    if(info.singleshoot)
    {
        for(size_t i = 0; i < mLogicalBps.size(); i++)
        {
            if(mLogicalBps[i].boundAddr == info.address)
            {
                mLogicalBps.erase(mLogicalBps.begin() + i);
                break;
            }
        }
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
        Gleam::logEvent("ignored address=0x%llX left=%u",
               (unsigned long long)info.address, ignoreLeft - 1);
        return;
    }

    // Conditional breakpoints, tracepoints and "do" commands may suppress
    // the pause entirely.
    if(!evalBpRule(info, rulePtr))
        return;

    char details[Gleam::Memory::MEDIUM_DETAIL_BUFFER];
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
    // stepout engine: a step landed on the OWNING thread; inspect what is
    // at GIP now. Steps from other threads never drive the loop.
    if(mStepOutActive && mDebugEvent.dwThreadId == mStepOutTid)
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
            Gleam::logInfo("trace rip=0x%llX %s", (unsigned long long)rip, text.c_str());
        }
        bool done = evalCondition(mTraceCondReg, mTraceCondOp, mTraceCondValue);
        bool capped = mTraceCount >= mTraceMax;
        if(done || capped)
        {
            mTraceActive = false;
            mStepArmed = false;
            char details[Gleam::Memory::MEDIUM_DETAIL_BUFFER];
            sprintf_s(details, "%s steps=%llu", capped && !done ? "maxreached" : "condition",
                      (unsigned long long)mTraceCount);
            emitStop("trace", details);
            mWantsPause = true;
            return;
        }
        Thread* traceThread = currentThread();
        if(!traceThread)
        {
            mTraceActive = false;
            mStepArmed = false;
            emitStop("trace", "error steps=0");
            mWantsPause = true;
            return;
        }
        traceThread->StepInto(); // mStepArmed stays armed
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

// Exception disposition policy (P0-3), x64dbg semantics. Pure decision
// function so the full combination matrix is unit-testable ("selftest").
// Rules:
//   filter hit, first chance  -> pause iff breakOn==first; disposition =
//     handledBy (pass = NOT_HANDLED, swallow = DBG_CONTINUE)
//   filter hit, second chance -> ONLY breakOn==never && handledBy==pass
//     may pass NOT_HANDLED without pausing (explicit: can kill the
//     process); every other combination pauses with swallow as default
//   filter miss, first chance -> breakOnException ? pause+pass : silent pass
//   filter miss, second chance -> pause with swallow default;
//     "exception pass" is the explicit escape (warned).
GleamDebugger::ExPolicyOutput GleamDebugger::decideExPolicy(const ExPolicyInput & in)
{
    ExPolicyOutput out{};
    if(in.filterHit)
    {
        if(in.firstChance)
        {
            out.pause = in.breakOn == 0;
            out.swallow = in.handledBy == 1;
        }
        else if(in.breakOn == 2 && in.handledBy == 0)
        {
            out.pause = false;  // explicit DoNotBreak + pass: may kill it
            out.swallow = false;
        }
        else
        {
            out.pause = true;
            out.swallow = true;
        }
    }
    else
    {
        out.pause = !in.firstChance || in.breakOnException;
        out.swallow = out.pause && !in.firstChance;
    }
    return out;
}

void GleamDebugger::cbUnhandledException(const EXCEPTION_RECORD & exceptionRecord, bool firstChance)
{
    mLastException = exceptionRecord;
    mLastExceptionValid = true;
    mLastExceptionFirstChance = firstChance;

    // Raw DR mode (P0-6): a hardware breakpoint hit arrives as
    // STATUS_SINGLE_STEP that the engine cannot attribute to any of its
    // slots. Only report it when the EVENT thread has raw DR writes and
    // DR6 actually names a slot; anything else falls through to the
    // normal exception policy.
    if(exceptionRecord.ExceptionCode == STATUS_SINGLE_STEP && mThread &&
       mRawDrThreads.count(mDebugEvent.dwThreadId))
    {
        Registers r(mThread->hThread);
        const uint64_t dr6 = r.GetContext()->Dr6;
        const int slot = (dr6 & 1) ? 0 : (dr6 & 2) ? 1 : (dr6 & 4) ? 2 : (dr6 & 8) ? 3 : -1;
        if(slot >= 0)
        {
            char details[Gleam::Memory::MEDIUM_DETAIL_BUFFER];
            sprintf_s(details, "raw-hardware slot=%d address=0x%p", slot, exceptionRecord.ExceptionAddress);
            mContinueStatus = DBG_CONTINUE; // single-step exceptions continue
            emitStop("hardware", details);
            mWantsPause = true;
            return;
        }
    }

    // Our own break-in (triggered by "pause"). Identification, most precise
    // first: the stub's exception address (any of the stub's int3 bytes, so
    // a thread that survived a failed TerminateThread is recognized again at
    // the next one), then the fallback thread's DbgUiRemoteBreakin address,
    // then the fallback flag as last resort.
    const uint8_t* stubPage = (uint8_t*)mBreakInStubPage.load();
    const bool isStubBreakIn = exceptionRecord.ExceptionCode == STATUS_BREAKPOINT &&
                               stubPage != nullptr &&
                               (uint8_t*)exceptionRecord.ExceptionAddress >= stubPage &&
                               (uint8_t*)exceptionRecord.ExceptionAddress < stubPage + 16;
    const bool isFallbackBreakIn = exceptionRecord.ExceptionCode == STATUS_BREAKPOINT &&
                                   ((mDbgBreakInAddr.load() != 0 &&
                                     (uint64_t)exceptionRecord.ExceptionAddress == mDbgBreakInAddr.load()) ||
                                    mBreakInExpected.exchange(false));
    if(isStubBreakIn || isFallbackBreakIn)
    {
        mContinueStatus = DBG_CONTINUE;
        // Terminate the stub thread right at its int3: the stub is pure
        // int3s, the thread is at a known-good point inside our own page,
        // and past the stub only zero bytes (an instant crash) await - so a
        // failed TerminateThread is loudly reported but still fatal to the
        // thread. Keep the HANDLE: the EXIT_THREAD event is the death
        // confirmation the deferred detach cleanup waits for before the
        // stub page may be freed. (Waiting here could never succeed - a
        // held debug event freezes the whole process.)
        if(auto hThread = mBreakInStubThread.load())
        {
            bool terminated;
            if(mFailNextTerminate)
            {
                mFailNextTerminate = false; // injected failure (selftest)
                terminated = false;
                SetLastError(ERROR_ACCESS_DENIED);
            }
            else
            {
                terminated = TerminateThread(hThread, 0) != 0;
            }
            if(!terminated)
            {
                Gleam::logEvent("error msg=\"Gleam: TerminateThread failed for break-in stub tid %u (error %lu)\"",
                       mBreakInStubTid.load(), GetLastError());
            }
        }
        emitStop("pause", nullptr);
        abortStepOut("pause");
        mWantsPause = true;
        return;
    }

    // Policy decision (P0-3): filter lookup -> decideExPolicy, then act.
    ExPolicyInput pi{ firstChance, false, 2, 0, mBreakOnException };
    auto filter = mExFilters.find(exceptionRecord.ExceptionCode);
    if(filter != mExFilters.end())
    {
        pi.filterHit = true;
        pi.breakOn = filter->second.breakOn;
        pi.handledBy = filter->second.handledBy;
    }
    const auto policy = decideExPolicy(pi);
    if(!policy.pause)
    {
        // "pass" keeps the engine default DBG_EXCEPTION_NOT_HANDLED (the
        // debuggee's own handlers run); "swallow" is DBG_CONTINUE.
        if(policy.swallow)
            mContinueStatus = DBG_CONTINUE;
        Gleam::logEvent("exception code=0x%08lX action=%s",
               exceptionRecord.ExceptionCode,
               policy.swallow ? "swallowed" : "passed-to-debuggee");
        return;
    }

    // Second chance pauses with swallow as the default disposition (see
    // decideExPolicy); "exception pass" is the explicit escape.
    if(policy.swallow)
        mContinueStatus = DBG_CONTINUE;
    char details[Gleam::Memory::LARGE_DETAIL_BUFFER];
    sprintf_s(details, "code=0x%08lX address=0x%p chance=%s",
              exceptionRecord.ExceptionCode,
              exceptionRecord.ExceptionAddress,
              firstChance ? "first" : "second");
    emitStop("exception", details);
    abortStepOut("exception");
    mPausedOnException = true;
    mWantsPause = true;
}

void GleamDebugger::cbInternalError(const std::string & error)
{
    Gleam::logEvent("error msg=\"%s\"", error.c_str());
}

void GleamDebugger::cbDetachRefused(const std::string & info)
{
    // The engine refused the detach (unrestored debugger-owned suspensions).
    // The refusal is reported SYNCHRONOUSLY: re-arm command control right
    // here - waiting for the next target event could hang forever on a
    // quiet target. forceBreakIn manufactures the event that re-enters the
    // command loop, so queued commands (disarm, retry detach) run even if
    // the target never produces another event on its own.
    mQuitting = false; // detach is off; the session is fully attached again
    mWantsPause = true;
    Gleam::logError(Gleam::Strings::DETACH_REFUSED_THREADS);
    forceBreakIn();
}

void GleamDebugger::cbPreDebugEvent(const DEBUG_EVENT & debugEvent)
{
    mInDebugEvent.store(true);
}

void GleamDebugger::cbPostDebugEvent(const DEBUG_EVENT & debugEvent)
{
    // Pending logical breakpoints retry at EVERY debug event opportunity
    // (PDB-only symbols become resolvable as the loader proceeds).
    for(const auto & lb : mLogicalBps)
    {
        if(!lb.boundAddr)
        {
            rebindPendingBreakpoints();
            break;
        }
    }
    // Event-opportunity retry for break-in symbol resolution (rate-limited
    // logging; the pending state is simply "address still zero").
    if(!mDbgBreakInAddr.load())
    {
        if(mExitThreadResolveAttempts++ % Gleam::Limits::RESOLVE_RETRY_LOG_INTERVAL == 0)
        {
            Gleam::logEvent("breakin resolve retry=%u", mExitThreadResolveAttempts);
        }
        resolveBreakInSymbols();
    }

    // Two-stage re-arm for a non-owner-consumed internal breakpoint:
    // stage 1 (this event) only defers; stage 2 (next event, after the
    // engine's internal step has completed) writes the int3 back.
    if(mStepOutRearmPending)
    {
        mStepOutRearm = mStepOutRearmPending;
        mStepOutRearmPending = 0;
    }
    else if(mStepOutRearm)
    {
        if(mStepOutActive && mProcess)
        {
            if(!mProcess->SetBreakpoint(mStepOutRearm, true))
            {
                Gleam::logError("stepout error: failed to re-arm internal breakpoint at 0x%llX",
                       (unsigned long long)mStepOutRearm);
                stepOutFinish("error");
            }
            else
            {
                mStepOutBpOurs = true; // physically ours again
                Gleam::logEvent("stepout internal bp re-armed at 0x%llX",
                       (unsigned long long)mStepOutRearm);
            }
        }
        mStepOutRearm = 0;
    }

    // Enter the command loop AFTER event-side work: a stop produced by the
    // re-arm path above (e.g. a re-arm failure) must pause in THIS event,
    // not after the event has already been continued.
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
        Gleam::logEvent("breakin deferred-fire");
        forceBreakIn();
    }
}

void GleamDebugger::commandLoop()
{
    // The stop record was already emitted by the triggering event; it is the
    // pause notification. Here we only consume commands.
    // Symbols may have become available since the last event: retry pending
    // logical breakpoints (PDB-only modules, deferred resolution).
    rebindPendingBreakpoints();
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
