// Thread Safety and Concurrency Model
//
// This file documents the threading model of GleamDebugger to prevent
// race conditions and clarify synchronization requirements.
//
// ## Thread Roles
//
// 1. **Main Thread (Debug Loop)**
//    - Runs GleeBug event loop (Init/Attach + Start)
//    - Processes debug events (breakpoints, exceptions, DLL loads)
//    - Executes commands when debuggee is paused
//    - EXCLUSIVE owner of: mProcess, mThread, mDebugEvent
//
// 2. **REPL Thread (Command Input)**
//    - Reads commands from stdin
//    - Calls pushCommand() to enqueue commands
//    - Calls requestPause() to interrupt running debuggee
//    - Does NOT directly access debugger state
//
// ## Synchronization Primitives
//
// ### Command Queue
// - `mCmdQueue`: std::queue<std::string>
// - `mCmdMutex`: std::mutex (protects mCmdQueue)
// - `mCmdCv`: std::condition_variable (notifies main thread)
// - Thread-safe operations: pushCommand(), commandLoop()
//
// ### Atomic Flags
// - `mIsPaused`: atomic<bool> - debuggee is suspended
// - `mInDebugEvent`: atomic<bool> - between event delivery and ContinueDebugEvent
// - `mQuitting`: atomic<bool> - detach/quit in flight
// - `mPauseAfterResume`: atomic<bool> - pause request pending
// - `mBreakInExpected`: atomic<bool> - break-in event expected
//
// ### Break-in Stub State
// - `mBreakInStubThread`: atomic<HANDLE> - injected thread handle
// - `mBreakInStubPage`: atomic<void*> - stub page address
// - `mBreakInStubTid`: atomic<uint32_t> - stub thread ID
// - `mBreakInMutex`: mutex - serializes stub injection and cleanup
// - Thread-safe operations: forceBreakIn(), cleanupBreakInStub()
//
// ## Invariants
//
// 1. **Single Writer Rule**: Only main thread writes to mProcess/mThread
// 2. **Command Execution**: Commands only execute when mIsPaused == true
// 3. **Stub Lifecycle**: Page freed only after thread death confirmed
// 4. **Queue Ordering**: Commands execute in FIFO order
//
// ## Safe Access Patterns
//
// ### Pattern 1: Posting Work to Main Thread
//   REPL Thread:
//     pushCommand("bp 0x401000")  // Enqueues command
//   Main Thread:
//     commandLoop()               // Dequeues and executes
//
// ### Pattern 2: Interrupting Debuggee
//   REPL Thread:
//     requestPause()              // Atomic flag + forceBreakIn()
//   Main Thread:
//     cbPreDebugEvent()           // Detects flag and pauses
//
// ### Pattern 3: Stub Management
//   Main Thread (pause request):
//     forceBreakIn()              // Locks mBreakInMutex
//       -> ensureBreakInStub()
//       -> CreateRemoteThread()
//   Main Thread (cleanup):
//     cleanupBreakInStub()        // Locks mBreakInMutex
//       -> TerminateThread()
//       -> freeBreakInStubPage()
//
// ## Common Pitfalls
//
// ❌ DO NOT access mProcess/mThread from REPL thread
// ❌ DO NOT modify mBreakInStub* without holding mBreakInMutex
// ❌ DO NOT assume atomic loads are ordered without explicit barriers
// ✅ DO use pushCommand() to defer work to main thread
// ✅ DO check mIsPaused before executing commands
// ✅ DO lock mBreakInMutex before stub injection or cleanup
//
// ## Future Improvements
//
// 1. Add GUARDED_BY annotations when C++20 attributes available
// 2. Consider reader-writer lock for symbol cache
// 3. Profile lock contention on mBreakInMutex
// 4. Document memory ordering requirements for atomics

#ifndef GLEAM_THREAD_SAFETY_H
#define GLEAM_THREAD_SAFETY_H

// Thread safety documentation - this header is included for reference only.
// The actual thread safety model is implemented in GleamDebugger.h/cpp.

#endif // GLEAM_THREAD_SAFETY_H
