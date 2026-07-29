#include "Debugger.h"
#include "Debugger.Thread.Registers.h"
#include <unordered_set>

#ifndef DBG_REPLY_LATER
#define DBG_REPLY_LATER ((NTSTATUS)0x40010001L)
#endif // DBG_REPLY_LATER

namespace GleeBug
{
    void Debugger::Start()
    {
        //initialize loop variables
        mBreakDebugger = false;
        mIsDebugging = true;
        mDetach = false;
        mDetachAndBreak = false;

        //use correct WaitForDebugEvent function
        typedef BOOL(WINAPI * MYWAITFORDEBUGEVENT)(
            _Out_ LPDEBUG_EVENT lpDebugEvent,
            _In_  DWORD         dwMilliseconds
        );
        static auto WFDEX = MYWAITFORDEBUGEVENT(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "WaitForDebugEventEx"));
        static auto MyWaitForDebugEvent = WFDEX ? WFDEX : MYWAITFORDEBUGEVENT(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "WaitForDebugEvent"));
        if(!MyWaitForDebugEvent)
        {
            cbInternalError("MyWaitForDebugEvent not set!");
            return;
        }

        DWORD ThreadBeingProcessed = 0;
        std::unordered_map<DWORD, HANDLE> SuspendedThreads;
        std::unordered_set<uint64_t> DeferredExceptionThreads;
        bool IsDbgReplyLaterSupported = false;

        // Single checked resume path for debugger-owned suspensions. Every site
        // that ends an internal step (single-step arrival, owner thread exiting,
        // detach, error break, natural loop end) goes through here so a failed
        // ResumeThread is reported with tid + error code, never silently dropped
        // leaving the target frozen.
        //
        // An entry holds the system-supplied thread handle from the debug event,
        // which is only valid while the thread is still known to us: the system
        // closes it once we continue past that thread's EXIT_THREAD, and all of
        // a process's handles once we continue past its EXIT_PROCESS. Entries
        // for threads we have already buried are therefore DISCARDED, not
        // resumed - there is nothing left to unfreeze, and resuming would fail
        // with ERROR_INVALID_HANDLE and report a bogus internal error on every
        // normal teardown.
        const auto resumeSuspendedThreads = [this, &SuspendedThreads, &ThreadBeingProcessed]()
        {
            for(auto & itr : SuspendedThreads)
            {
                bool stillKnown = false;
                for(const auto & process : mProcesses)
                {
                    if(process.second->threads.count(itr.first) != 0)
                    {
                        stillKnown = true;
                        break;
                    }
                }
                if(!stillKnown)
                    continue;

                if(ResumeThread(itr.second) == (DWORD)-1)
                {
                    char buf[128];
                    sprintf_s(buf, "Debugger: ResumeThread failed for tid %u (error %lu)",
                              itr.first, GetLastError());
                    cbInternalError(buf);
                }
            }
            SuspendedThreads.clear();
            ThreadBeingProcessed = 0;
        };

        // Unified suspension cleanup for loop exits (detach, error break,
        // natural end): resume everything, then drop the stepping flags so no
        // trap flag survives us. Mid-loop resume sites must NOT use this - they
        // still need isInternalStepping for the pending exceptionEvent dispatch.
        const auto cleanupSuspensions = [this, &resumeSuspendedThreads]()
        {
            if(mThread)
            {
                mThread->isInternalStepping = false;
                mThread->isSingleStepping = false;
            }
            resumeSuspendedThreads();
        };

        // Check if DBG_REPLY_LATER is supported based on Windows version (Windows 10, version 1507 or above)
        // https://www.gaijin.at/en/infos/windows-version-numbers
        const uint32_t NtBuildNumber = *(uint32_t*)(0x7FFE0000 + 0x260);
        if(NtBuildNumber != 0 && NtBuildNumber >= 10240)
        {
            IsDbgReplyLaterSupported = mSafeStep;
        }

        uint32 consecutiveTimeouts = 0;

        while(!mBreakDebugger)
        {
            //wait for a debug event
            mIsRunning = true;
            if(!MyWaitForDebugEvent(&mDebugEvent, 100))
            {
                if(mDetach)
                {
                    if(!UnsafeDetach())
                        cbInternalError("Debugger::Detach failed!");
                    break;
                }
                const DWORD waitError = GetLastError();
                if(waitError != ERROR_SEM_TIMEOUT)
                {
                    // A real API failure, not a timeout: report and bail out
                    // (loop-end cleanupSuspensions still runs).
                    char waitBuf[128];
                    sprintf_s(waitBuf, "Debugger::WaitForDebugEvent failed (error %lu)", waitError);
                    cbInternalError(waitBuf);
                    break;
                }
                else
                {
                    // Do not expire deleted breakpoints while a breakpoint step-over is
                    // still in flight. The stepped instruction can block indefinitely and
                    // breakpoint events from the suspended threads remain pending until its
                    // single-step event arrives.
                    consecutiveTimeouts++;
                    if(consecutiveTimeouts >= 2 && ThreadBeingProcessed == 0 && SuspendedThreads.empty() && DeferredExceptionThreads.empty() && mProcess)
                        mProcess->recentlyDeletedSwbp.clear();
                    continue;
                }
            }

            //event received, reset timeout counter
            consecutiveTimeouts = 0;
            const uint64_t eventThreadKey = (uint64_t(mDebugEvent.dwProcessId) << 32) | mDebugEvent.dwThreadId;
            const bool completingDeferredException = DeferredExceptionThreads.count(eventThreadKey) != 0;

            // Handle safe stepping
            if(IsDbgReplyLaterSupported)
            {
                if(mDebugEvent.dwDebugEventCode == EXCEPTION_DEBUG_EVENT)
                {
                    // Check if there is a thread processing a single step
                    if(ThreadBeingProcessed != 0 && mDebugEvent.dwThreadId != ThreadBeingProcessed)
                    {
                        // Reply to the event later and retain its ownership state until
                        // the same thread's event is processed normally.
                        DeferredExceptionThreads.insert(eventThreadKey);
                        if(!ContinueDebugEvent(mDebugEvent.dwProcessId, mDebugEvent.dwThreadId, DBG_REPLY_LATER))
                        {
                            char contBuf[160];
                            sprintf_s(contBuf, "Debugger::ContinueDebugEvent(DBG_REPLY_LATER) failed (error %lu, pid=%lu, tid=%lu)",
                                      GetLastError(), mDebugEvent.dwProcessId, mDebugEvent.dwThreadId);
                            cbInternalError(contBuf);
                            break;
                        }

                        // Wait for the next event
                        continue;
                    }
                }
                else if(mDebugEvent.dwDebugEventCode == EXIT_THREAD_DEBUG_EVENT)
                {
                    if(ThreadBeingProcessed != 0 && mDebugEvent.dwThreadId == ThreadBeingProcessed)
                    {
                        // Resume the other threads since the thread being processed
                        // is exiting (checked resume: a failure is reported).
                        resumeSuspendedThreads();
                    }
                }
            }

            // Signal we are currently paused
            mIsRunning = false;

            //set default continue status
            mContinueStatus = DBG_EXCEPTION_NOT_HANDLED;

            //set the current process and thread
            auto processFound = mProcesses.find(mDebugEvent.dwProcessId);
            if(processFound != mProcesses.end())
            {
                mProcess = processFound->second.get();
                auto threadFound = mProcess->threads.find(mDebugEvent.dwThreadId);
                if(threadFound != mProcess->threads.end())
                {
                    mThread = mProcess->thread = threadFound->second.get();
                }
                else
                {
                    mThread = mProcess->thread = nullptr;
                }
            }
            else
            {
                mThread = nullptr;
                if(mProcess)
                {
                    mProcess->thread = nullptr;
                    mProcess = nullptr;
                }
            }

            //call the pre debug event callback
            cbPreDebugEvent(mDebugEvent);

            //dispatch the debug event (documented here: https://msdn.microsoft.com/en-us/library/windows/desktop/ms679302(v=vs.85).aspx)
            switch(mDebugEvent.dwDebugEventCode)
            {
            case CREATE_PROCESS_DEBUG_EVENT:
                // HACK: when hollowing the process the debug event still delivers the original image base
                if(mDisableAslr && mDebugModuleImageBase != 0)
                {
                    auto startAddress = ULONG_PTR(mDebugEvent.u.CreateProcessInfo.lpStartAddress);
                    if(startAddress)
                    {
                        startAddress -= ULONG_PTR(mDebugEvent.u.CreateProcessInfo.lpBaseOfImage);
                        startAddress += mDebugModuleImageBase;
                        mDebugEvent.u.CreateProcessInfo.lpStartAddress = LPTHREAD_START_ROUTINE(startAddress);
                    }
                    mDebugEvent.u.CreateProcessInfo.lpBaseOfImage = LPVOID(mDebugModuleImageBase);
                }
                createProcessEvent(mDebugEvent.u.CreateProcessInfo);
                break;
            case EXIT_PROCESS_DEBUG_EVENT:
                exitProcessEvent(mDebugEvent.u.ExitProcess);
                break;
            case CREATE_THREAD_DEBUG_EVENT:
                createThreadEvent(mDebugEvent.u.CreateThread);
                break;
            case EXIT_THREAD_DEBUG_EVENT:
                exitThreadEvent(mDebugEvent.u.ExitThread);
                break;
            case LOAD_DLL_DEBUG_EVENT:
                loadDllEvent(mDebugEvent.u.LoadDll);
                break;
            case UNLOAD_DLL_DEBUG_EVENT:
                unloadDllEvent(mDebugEvent.u.UnloadDll);
                break;
            case EXCEPTION_DEBUG_EVENT:
                if(IsDbgReplyLaterSupported && mDebugEvent.u.Exception.ExceptionRecord.ExceptionCode == STATUS_SINGLE_STEP)
                {
                    // Resume the other threads since we are done processing the
                    // single step (checked resume: a failure is reported). The
                    // stepping flags must survive here - exceptionEvent() below
                    // still needs isInternalStepping to classify this event.
                    resumeSuspendedThreads();
                }
                exceptionEvent(mDebugEvent.u.Exception);
                break;
            case OUTPUT_DEBUG_STRING_EVENT:
                debugStringEvent(mDebugEvent.u.DebugString);
                break;
            case RIP_EVENT:
                ripEvent(mDebugEvent.u.RipInfo);
                break;
            default:
                unknownEvent(mDebugEvent.dwDebugEventCode);
                break;
            }

            //call the post debug event callback
            cbPostDebugEvent(mDebugEvent);

            //execute the delayed-detach
            if(mDetachAndBreak)
            {
                if(!UnsafeDetachAndBreak())
                    cbInternalError("Debugger::DetachAndBreak failed!");
                break;
            }

            //clear trap flag when set by GleeBug (to prevent an EXCEPTION_SINGLE_STEP after detach)
            if(mDetach && mThread)
            {
                if(mThread->isInternalStepping || mThread->isSingleStepping)
                    Registers(mThread->hThread, CONTEXT_CONTROL).TrapFlag = false;
            }

            // Handle safe stepping (never during a detach: no new internal
            // suspensions may be created once we are letting the target go)
            if(!mDetach && IsDbgReplyLaterSupported && mDebugEvent.dwDebugEventCode != EXIT_THREAD_DEBUG_EVENT)
            {
                // If TF is set (single step), then suspend all the other threads
                if(mThread && mThread->isInternalStepping)
                {
                    ThreadBeingProcessed = mDebugEvent.dwThreadId;

                    for(auto & Thread : mProcess->threads)
                    {
                        auto dwThreadId = Thread.first;
                        auto hThread = Thread.second->hThread;

                        // Do not suspend the current thread
                        if(ThreadBeingProcessed == dwThreadId)
                            continue;

                        // Check if the thread is already suspended
                        if(SuspendedThreads.count(dwThreadId) != 0)
                            continue;

                        if(SuspendThread(hThread) != -1)
                            SuspendedThreads.emplace(dwThreadId, hThread);
                    }
                }
            }

            //continue the debug event
            if(!ContinueDebugEvent(mDebugEvent.dwProcessId, mDebugEvent.dwThreadId, mContinueStatus))
            {
                char contBuf[160];
                sprintf_s(contBuf, "Debugger::ContinueDebugEvent failed (error %lu, pid=%lu, tid=%lu)",
                          GetLastError(), mDebugEvent.dwProcessId, mDebugEvent.dwThreadId);
                cbInternalError(contBuf);
                break;
            }
            if(completingDeferredException)
                DeferredExceptionThreads.erase(eventThreadKey);

            if(mDetach || mDetachAndBreak)
            {
                // Leave no debugger-owned suspension behind (unified cleanup,
                // also runs on the natural loop end below).
                cleanupSuspensions();
                if(!UnsafeDetach())
                    cbInternalError("Debugger::Detach failed!");
                break;
            }
        }

        //cleanup (unified: also covers error breaks and natural loop end)
        cleanupSuspensions();
        mProcesses.clear();
        mProcess = nullptr;
        mIsDebugging = false;
        // Handle ownership: CreateProcessW's PROCESS_INFORMATION handles are
        // caller-owned - close them here (launch sessions only). Attach
        // sessions hold system-owned debug-event handles in mMainProcess,
        // which Windows closes after the exit event is continued.
        if(!mAttachedToProcess)
        {
            if(mMainProcess.hThread)
                CloseHandle(mMainProcess.hThread);
            if(mMainProcess.hProcess)
                CloseHandle(mMainProcess.hProcess);
        }
        memset(&mMainProcess, 0, sizeof(mMainProcess));
    }
};