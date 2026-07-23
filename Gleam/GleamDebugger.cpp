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
    if(mIsPaused.load())
        return;
    auto process = mProcess;
    if(process)
    {
        mBreakInExpected.store(true);
        DebugBreakProcess(process->hProcess);
    }
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

void GleamDebugger::cbCreateProcessEvent(const CREATE_PROCESS_DEBUG_INFO & createProcess, const Process & process)
{
    printf("[event] process %u created, entry=0x%p base=0x%p\n",
           mDebugEvent.dwProcessId,
           createProcess.lpStartAddress,
           createProcess.lpBaseOfImage);
    fflush(stdout);
}

void GleamDebugger::cbExitProcessEvent(const EXIT_PROCESS_DEBUG_INFO & exitProcess, const Process & process)
{
    printf("[event] process %u exited, code=0x%08X\n",
           mDebugEvent.dwProcessId,
           exitProcess.dwExitCode);
    fflush(stdout);
    closeSymSession();
}

void GleamDebugger::cbSystemBreakpoint()
{
    printf("[event] system breakpoint\n");
    fflush(stdout);
    mWantsPause = true;
}

void GleamDebugger::cbAttachBreakpoint()
{
    // Fired (instead of the system breakpoint) when attached to a process.
    printf("[event] attach breakpoint\n");
    fflush(stdout);
    mWantsPause = true;
}

void GleamDebugger::cbBreakpoint(const BreakpointInfo & info)
{
    // Ignore-count: auto-continue without pausing.
    auto ignore = mIgnoreHits.find(info.address);
    if(ignore != mIgnoreHits.end() && ignore->second > 0)
    {
        ignore->second--;
        printf("[event] breakpoint at 0x%p ignored (%u left)\n",
               (void*)info.address, ignore->second);
        fflush(stdout);
        return;
    }

    if(mStepOverArmed && info.singleshoot)
    {
        mStepOverArmed = false;
        printf("[event] stepped over to 0x%p\n", (void*)info.address);
    }
    else
    {
        const char* typeText =
            info.type == BreakpointType::Software ? "software" :
            info.type == BreakpointType::Hardware ? "hardware" : "memory";
        printf("[event] %s breakpoint hit at 0x%p\n", typeText, (void*)info.address);
    }
    fflush(stdout);
    mWantsPause = true;
}

void GleamDebugger::cbStep()
{
    // Only pause for user-requested steps; GleeBug also steps internally
    // (e.g. to restore software breakpoints).
    if(mStepArmed)
    {
        mStepArmed = false;
        mStepOverArmed = false;
        printf("[event] single step\n");
        fflush(stdout);
        mWantsPause = true;
    }
}

void GleamDebugger::cbUnhandledException(const EXCEPTION_RECORD & exceptionRecord, bool firstChance)
{
    mLastException = exceptionRecord;
    mLastExceptionValid = true;
    mLastExceptionFirstChance = firstChance;

    // Our own DebugBreakProcess break-in (triggered by "pause").
    if(exceptionRecord.ExceptionCode == STATUS_BREAKPOINT && mBreakInExpected.exchange(false))
    {
        printf("[event] paused (break-in)\n");
        fflush(stdout);
        mContinueStatus = DBG_CONTINUE;
        mWantsPause = true;
        return;
    }

    // Filtered exception codes are passed back to the debuggee without pausing.
    if(mIgnoredExceptions.count(exceptionRecord.ExceptionCode))
    {
        printf("[event] exception 0x%08X at 0x%p ignored\n",
               exceptionRecord.ExceptionCode,
               exceptionRecord.ExceptionAddress);
        fflush(stdout);
        mContinueStatus = DBG_CONTINUE;
        return;
    }

    printf("[event] unhandled exception (%s) code=0x%08X at 0x%p\n",
           firstChance ? "first chance" : "second chance",
           exceptionRecord.ExceptionCode,
           exceptionRecord.ExceptionAddress);
    fflush(stdout);
    mWantsPause = true;
}

void GleamDebugger::cbInternalError(const std::string & error)
{
    printf("[error] %s\n", error.c_str());
    fflush(stdout);
}

void GleamDebugger::cbPostDebugEvent(const DEBUG_EVENT & debugEvent)
{
    if(mWantsPause && !mQuitting && mProcess && mThread)
    {
        mWantsPause = false;
        commandLoop();
    }
}

void GleamDebugger::commandLoop()
{
    mIsPaused.store(true);
    {
        Registers r(mThread->hThread);
        printf("[gleam] paused, RIP=0x%llX TID=%u\n",
               (unsigned long long)r.Gip(),
               mDebugEvent.dwThreadId);
        fflush(stdout);
    }
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
    // A "pause" that arrived while we were paused takes effect right after
    // the resume, so the request is never silently dropped.
    if(mPauseAfterResume.exchange(false))
        requestPause();
}
