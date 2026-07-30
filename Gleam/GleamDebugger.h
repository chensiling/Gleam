#ifndef GLEAM_DEBUGGER_H
#define GLEAM_DEBUGGER_H

#include <cstdint>
#include <string>
#include <vector>
#include <queue>
#include <map>
#include <set>
#include <mutex>
#include <atomic>
#include <condition_variable>

#include <GleeBug/Debugger.h>
#include <GleeBug/Debugger.Thread.Registers.h>

// Shared parsing helper (GleamCommands.cpp).
bool parseHex(const std::string & s, uint64_t & out);

// Shared helper (GleamCommands.cpp): basename, lowercase, ".dll" stripped.
std::string normalizeModuleName(const std::string & name);

// Command-driven headless debugger based on GleeBug.
//
// The debug loop (Init/Attach + Start) runs on the caller's thread. A REPL
// thread feeds commands via pushCommand(). Whenever the debuggee is suspended
// by an interesting event (system breakpoint, breakpoint hit, single step,
// unhandled exception), the debugger thread enters a command loop and executes
// queued commands; "g"/"step"/"stepover"/"ret"/"detach"/"quit" leave the
// command loop and resume the debuggee.
//
// Command implementations are split by functional area:
//   GleamCommands.cpp             command dispatch, parsing helpers, help
//   GleamCommands.Breakpoints.cpp software/hardware/memory breakpoints, ignore counts
//   GleamCommands.Inspect.cpp     registers, memory, disassembly, maps, modules, find, bt
//   GleamCommands.Control.cpp     execution control (g/step/over/ret/detach/quit), thread selection
class GleamDebugger : public GleeBug::Debugger
{
public:
    // Called from the REPL thread. Returns true if the queue was empty before
    // this push (i.e. the debugger is likely running free).
    bool pushCommand(const std::string & cmd);

    // Called from the REPL thread: interrupt a running debuggee.
    void requestPause();

    // True while the debugger thread sits in the command loop.
    bool isPaused() const;

    // Called from the REPL thread: break in right after the next resume.
    void pauseAfterResume();

    // Terminate the stub thread, wait for it to die, close the handle, and
    // only then free the page - never free memory a stub thread may run on.
    // Requests stub teardown; returns true only when nothing remains tracked
    // in the target (detach refuses on false - a held page must not leak).
    bool cleanupBreakInStub();
    // Frees the tracked stub page via the (injectable) VirtualFreeEx path;
    // the address is cleared only on success. Returns true when freed/none.
    bool freeBreakInStubPage();
    // Completes a deferred detach once the break-in stub thread is confirmed
    // dead: frees the stub page, then Detach(); refuses (stays attached)
    // when the page cannot be freed.
    void finishDeferredDetach();

    // "restart" support (main.cpp drives the re-Init + Start loop).
    void setLaunched(bool launched) { mHasLaunchInfo = launched; }
    bool takeRestartRequest() { const bool r = mRestartPending; mRestartPending = false; return r; }
    // Clear per-session state before a restart. Survives: logical
    // breakpoints (re-bind on module load), exception filters, breakon
    // switches, hide. Cleared: patches, ignore counts, thread selection,
    // last-exception state, all transient stepping/trace state.
    void resetTransientState();

private:
    // Unguarded break-in, only valid at the just-before-continue point.
    void forceBreakIn();
    // Lazily allocate/write the session stub page and resolve ExitThread.
    bool ensureBreakInStub(GleeBug::Process* process);
    // Last-resort break-in; sets the expectation flag only on success.
    void fallbackDebugBreak(GleeBug::Process* process);
    // Resolve/retry break-in symbols (debugger thread only, dbghelp).
    void resolveBreakInSymbols();

protected:
    void cbCreateProcessEvent(const CREATE_PROCESS_DEBUG_INFO & createProcess, const GleeBug::Process & process) override;
    void cbExitProcessEvent(const EXIT_PROCESS_DEBUG_INFO & exitProcess, const GleeBug::Process & process) override;
    void cbCreateThreadEvent(const CREATE_THREAD_DEBUG_INFO & createThread, const GleeBug::Thread & thread) override;
    void cbExitThreadEvent(const EXIT_THREAD_DEBUG_INFO & exitThread, const GleeBug::Thread & thread) override;
    void cbLoadDllEvent(const LOAD_DLL_DEBUG_INFO & loadDll) override;
    void cbUnloadDllEvent(const UNLOAD_DLL_DEBUG_INFO & unloadDll) override;
    void cbSystemBreakpoint() override;
    void cbAttachBreakpoint() override;
    void cbBreakpoint(const GleeBug::BreakpointInfo & info) override;
    void cbStep() override;
    void cbUnhandledException(const EXCEPTION_RECORD & exceptionRecord, bool firstChance) override;
    void cbInternalError(const std::string & error) override;
    void cbDetachRefused(const std::string & info) override;
    void cbPreDebugEvent(const DEBUG_EVENT & debugEvent) override;
    void cbPostDebugEvent(const DEBUG_EVENT & debugEvent) override;

private:
    // Note: R is a member enum of GleeBug::Registers (declared inside the class).
    using RegId = GleeBug::Registers::R;

    // Result of a try*Command handler.
    enum class CmdResult
    {
        NotMine,    // command not handled by this handler
        Handled,    // handled, debuggee stays suspended
        Resume      // handled, resume the debuggee
    };

    // Command handlers by functional area. Called in order from executeCommand.
    CmdResult tryControlCommand(const std::vector<std::string> & args);      // Control.cpp
    CmdResult tryBreakpointCommand(const std::vector<std::string> & args);   // Breakpoints.cpp
    CmdResult tryInspectCommand(const std::vector<std::string> & args);      // Inspect.cpp
    CmdResult trySymbolCommand(const std::vector<std::string> & args);       // Symbols.cpp
    CmdResult tryScanCommand(const std::vector<std::string> & args);         // Scan.cpp

    // Returns true when the debuggee should resume.
    bool executeCommand(const std::string & cmdLine);

    // Runs on the debugger thread while the debuggee is suspended.
    void commandLoop();

    // The thread register/memory-inspection commands operate on: the thread
    // selected with "thread <tid>", or the thread of the current debug event.
    GleeBug::Thread* currentThread();

    // Breakpoints.cpp
    void cmdBreakpointList();
    static const char* hwTypeText(GleeBug::HardwareType type);
    static const char* memTypeText(GleeBug::MemoryType type);

    // Inspect.cpp
    static bool registerByName(const std::string & name, RegId & reg);
    bool setRegisterExtended(const std::string & name, const std::string & valueText);
    void cmdPrintRegister(const std::string & name);
    // Raw DR mode, per thread (P0-6): TIDs whose DRs were written directly.
    // Blocks engine hardware breakpoints and gates DR6 hit reporting.
    std::set<uint32_t> mRawDrThreads;
    void cmdRegs();
    void cmdRead(uint64_t addr, uint64_t size);
    void cmdReadTyped(const char* type, uint64_t addr);          // u8/u16/u32/u64/ptr
    void cmdReadString(uint64_t addr, uint64_t maxLen, bool utf16);
    void cmdSaveMem(uint64_t addr, uint64_t size, const std::string & file);
    void cmdWrite(uint64_t addr, const std::vector<uint8_t> & bytes);
    void cmdThreads();
    void cmdDisasm(uint64_t addr, uint64_t count);
    void cmdMaps();
    void cmdModules();
    void cmdFind(uint64_t addr, uint64_t size, const std::string & pattern);
    void cmdFindString(uint64_t addr, uint64_t size, const std::string & text, bool utf16);
    void cmdExceptionInfo();
    void cmdBacktrace();
    void cmdStackScan(uint64_t count);
    void cmdPatch(uint64_t addr, const std::vector<uint8_t> & bytes);
    void cmdPatchList();
    void cmdRestore(uint64_t addr);

    // Breakpoints.cpp: conditional breakpoints and tracepoints.
    // Hit-time rule evaluated in the debugger thread before pausing.
    struct BpRule
    {
        RegId condReg = RegId::Invalid;  // Invalid = unconditional
        int condOp = 0;                  // 0:==, 1:!=, 2:<, 3:>
        uint64_t condValue = 0;
        bool trace = false;              // tracepoint: log and auto-continue
        std::string command;             // bp do <command>: run on hit
    };
    std::map<GleeBug::ptr, BpRule> mBpRules;
    bool evalBpRule(const GleeBug::BreakpointInfo & info, const BpRule* rule); // true = pause normally
    bool evalCondition(RegId reg, int op, uint64_t value); // current thread registers
    static bool parseCondition(const std::string & text, RegId & reg, int & op, uint64_t & value);

    // Module-relative logical breakpoints ("bp module!symbol" / "bp module+rva").
    // A pending entry binds when its module loads; unbind on unload keeps the
    // entry so a reload (or ASLR base change) re-binds to the right address.
    struct LogicalBp
    {
        std::string module;          // normalized: lowercase, basename, no .dll
        std::string symbol;          // empty = rva form
        uint64_t rva = 0;
        bool once = false;
        BpRule rule;
        GleeBug::ptr boundAddr = 0;  // 0 = pending
        uint64_t boundBase = 0;      // module base this binding belongs to
    };
    std::vector<LogicalBp> mLogicalBps;
    // Insert or replace a logical entry with the same spec (dedupe).
    void upsertLogicalBp(const LogicalBp & lb);
    // Parse "module!symbol" or "module+<hexrva>"; module part must look like
    // a module name (not a hex literal / not a pure expression).
    static bool parseLogicalSpec(const std::string & s, LogicalBp & out);
    // GleamDebugger.cpp: (un)bind on DLL load/unload events. The primary
    // module identity is the real path from the event's file handle (or the
    // loader list); the export-directory name is only an alias. imagePath
    // (may be null) feeds the PDB-only fallback (SymLoadModuleEx).
    void bindModuleBreakpoints(uint64_t moduleBase, const std::string & primaryName,
                               const wchar_t* imagePath = nullptr);
    void unbindModuleBreakpoints(uint64_t moduleBase);
    // Try to bind every pending entry whose module is already loaded
    // (system/attach breakpoint time - covers the main module, which has no
    // load event, and modules loaded before a restart).
    void rebindPendingBreakpoints();

    // Hide.cpp: anti-anti-debug.
    void cmdHide(bool on);
    void applyHides();

    // Scan.cpp: code scanning (xref, findasm).
    void cmdXref(uint64_t target);
    void cmdFindAsm(const std::string & text);

    // Inspect.cpp
    std::string disasmOne(uint64_t addr);

    // Control.cpp: conditional tracing ("tgo").
    bool mTraceActive = false;
    RegId mTraceCondReg = RegId::Invalid;
    int mTraceCondOp = 0;
    uint64_t mTraceCondValue = 0;
    uint64_t mTraceMax = 0;
    uint64_t mTraceCount = 0;
    bool mTraceLog = false;

    // Control.cpp: stepout ("ret") - a core stepping loop with two special
    // cases (ret / call), no stack analysis at all.
    bool mStepOutActive = false;
    bool mStepOutPending = false;   // a ret was just executed; finish next tick
    uint64_t mStepOutSteps = 0;
    uint64_t mStepOutMax = 0x40000;
    GleeBug::ptr mStepOutBpAddr = 0; // internal one-shot bp we are waiting on
    bool mStepOutBpOurs = false;     // ...and it is still physically OURS
                                     // (false after ANY hit at that address:
                                     // it may carry a user bp from then on)
    uint32_t mStepOutTid = 0;        // thread this stepout operation owns
    uint64_t mStepOutGen = 0;        // operation generation (per ret command)
    uint64_t mStepOutBpGen = 0;      // generation of the armed internal bp
    GleeBug::ptr mStepOutRearmPending = 0; // non-owner consumed our bp (stage 1)
    GleeBug::ptr mStepOutRearm = 0;        // arm at the NEXT event (stage 2)
    void stepOutTick();                 // inspect the current instruction, act
    void stepOutFinish(const char* reason);
    void abortStepOut(const char* why); // pause/exception/detach/quit/restart cleanup
    // Internal-breakpoint bookkeeping. MUST run first in cbBreakpoint, before
    // any user-state handling (ignore counts, rules): the engine deletes a
    // one-shot breakpoint after the callback whatever path it takes, so an
    // early return before this point strands the operation on a dead int3.
    // Returns true when the owner consumed the hit (a tick was driven and the
    // event must not surface as a user stop).
    bool handleStepOutBreakpoint(const GleeBug::BreakpointInfo & info);

    // Symbols.cpp (dbghelp-backed)
    bool ensureSymSession();
    void closeSymSession();
    void cmdImports(const std::string & moduleName);
    void cmdExports(const std::string & moduleName, const std::string & filter);
    void cmdSym(uint64_t addr);
    // Real stack frame enumeration ("frames"): RtlVirtualUnwind over a local
    // mirror of the remote stack + our own .pdata lookup (StackWalk64 leaves
    // the x64 frame stale; dbghelp's SymFunctionTableAccess64 is unreliable).
    // Verified frames only - heuristic guessing stays in stackscan.
    void cmdFrames(uint32_t tid, uint64_t maxFrames);
    RUNTIME_FUNCTION* findRuntimeFunction(uint64_t pc);
    uint64_t moduleBaseOf(uint64_t addr);
    // Session cache: module base -> remote .pdata (sorted by RVA).
    struct PdataCache
    {
        std::vector<RUNTIME_FUNCTION> entries;
        uint32_t dirRva = 0;  // exception directory range, for resolving
        uint32_t dirSize = 0; // indirect table entries
    };
    std::map<uint64_t, PdataCache> mPdataCache;
    // Resolve an address argument: any expression accepted by evalExpression
    // (hex, registers, module base, module!symbol, [deref], +/-, parentheses).
    // On failure the concrete reason is kept in mAddrError for printAddrError.
    bool parseAddress(const std::string & s, uint64_t & out);
    bool parseAddress(const std::string & s, uint64_t & out, std::string & err);
    void printAddrError();           // GleamCommands.Symbols.cpp
    std::string mAddrError;          // reason of the last failed parseAddress
    // Set by resolveModuleSymbol when a name has multiple records and no
    // unique live body. Unlike "not found" (which may bind later and may
    // become a pending breakpoint), an ambiguous symbol must NEVER bind -
    // the bp command refuses instead of registering it as pending.
    bool mSymbolAmbiguous = false;
    // Look up a loaded module's base by name (case-insensitive, .dll optional).
    bool moduleBaseByName(const std::string & name, uint64_t & base);
    // Same, with image size (for RVA bounds checks).
    bool moduleInfoOf(const std::string & name, uint64_t & base, uint32_t & size);
    // Image size read directly from the PE at base (no loader list needed;
    // works during the DLL load event). 0 = cannot confirm.
    uint32_t moduleImageSize(uint64_t base);
    // Resolve "module!symbol" through the dbghelp session.
    bool resolveModuleSymbol(const std::string & modSym, uint64_t & out);
    // Loader-list-independent module identity + export resolution (they work
    // during the DLL load event, when EnumProcessModules/dbghelp are blind).
    std::string dllNameFromBase(uint64_t base);
    uint64_t findExportByName(uint64_t base, const std::string & name);
    // Resolve a PDB-only symbol by explicitly loading the module's symbols
    // from its file on disk (works during the load event; the loader list
    // is not needed). imagePath may be null when unknown.
    uint64_t resolvePdbSymbol(uint64_t moduleBase, const wchar_t* imagePath, const std::string & symbol);
    // Prove "loaded module == file on disk" via CodeView GUID+Age+SizeOfImage.
    bool verifyModuleIdentity(uint64_t moduleBase, const wchar_t* imagePath);
    // Bases we SymLoadModuleEx'd explicitly (for SymUnloadModule64 pairing).
    std::set<uint64_t> mSymLoadedBases;
    // Real image paths learned at DLL load events, keyed by normalized
    // module name: later events for the same module may have hFile == NULL.
    std::map<std::string, std::wstring> mModulePaths;

    // Expr.cpp: address expression evaluation. See the grammar comment there.
    bool evalExpression(const std::string & s, uint64_t & out, std::string & err);
    bool exprParseSum(const std::string & s, size_t & pos, uint64_t & out, std::string & err);
    bool exprParseUnary(const std::string & s, size_t & pos, uint64_t & out, std::string & err);
    bool exprParseAtom(const std::string & s, size_t & pos, uint64_t & out, std::string & err);
    bool exprReadPointer(uint64_t addr, uint64_t & out);
    // Best-effort symbol name for an address (empty on failure).
    std::string symNameByAddr(uint64_t addr);
    // OEP (AddressOfEntryPoint) of a loaded module, 0 on failure.
    uint64_t moduleEntryPoint(uint64_t base);
    // dbghelp StackWalk64 one-frame unwind (.pdata-aware).
    enum class UnwindStatus { Success, Leaf, Failed };
    // Returns (status, caller return address). Leaf = verified no remote
    // .pdata record for the ORIGINAL rip (use [rsp] per the x64 ABI).
    std::pair<UnwindStatus, uint64_t> stackWalkReturn(HANDLE hThread);

    // Remote .pdata verification for the leaf/non-leaf distinction.
    enum class PdataCheck { HasRecord, NoRecord, Unknown };
    PdataCheck checkUnwindRecord(uint64_t rip);

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
    struct ExPolicyInput
    {
        bool firstChance;
        bool filterHit;
        int breakOn;        // 0=first 1=second 2=never (filter only)
        int handledBy;      // 0=pass 1=swallow (filter only)
        bool breakOnException;
    };
    struct ExPolicyOutput
    {
        bool pause;
        bool swallow;
    };
    static ExPolicyOutput decideExPolicy(const ExPolicyInput & in);

    // GleamDebugger.cpp: unified machine-readable stop record.
    // Format: "stop reason=<r> ... rip=0x... tid=<id>" (one line, key=value).
    void emitStop(const char* reason, const char* details) const;
    // Arm the one-shot OEP breakpoint (no-op if already armed or unavailable).
    void applyEntryBreakpoint();

    // GleamCommands.cpp
    static void cmdHelp();

    std::queue<std::string> mCmdQueue;
    std::mutex mCmdMutex;
    std::condition_variable mCmdCv;
    std::atomic<bool> mIsPaused{ false };
    std::atomic<bool> mInDebugEvent{ false };     // between event delivery and ContinueDebugEvent
    std::atomic<bool> mBreakInExpected{ false };  // "pause" break-in is on its way
    std::atomic<bool> mPauseAfterResume{ false }; // "pause" arrived while paused
    std::atomic<HANDLE> mBreakInStubThread{ nullptr }; // injected int3-stub thread
    std::atomic<void*> mBreakInStubPage{ nullptr };    // page backing the stub
    std::atomic<uint32_t> mBreakInStubTid{ 0 };        // tid of the stub thread (death confirmation)
    // detach deferred until the break-in stub thread is confirmed dead and
    // its page is freed (never detach leaving a remote RWX page behind).
    bool mDetachAfterStubCleanup = false;

    // Gleam-side fault-injection flags ("selftest failapi ..."): each fails
    // the matching API call ONCE in the break-in stub paths, then clears.
    bool mFailNextTerminate = false;  // TerminateThread on the stub thread
    bool mFailNextStubResume = false; // ResumeThread of a fresh stub thread
    bool mFailNextVfree = false;      // VirtualFreeEx of the stub page
    std::atomic<uint64_t> mDbgBreakInAddr{ 0 };        // ntdll!DbgUiRemoteBreakin (fallback break-in identity)
    uint32_t mExitThreadResolveAttempts = 0;           // rate-limit retry logging
    bool mWantsPause = false;
    bool mStepArmed = false;      // a user-requested step is in flight
    bool mStepOverArmed = false;  // a user-requested step-over is in flight
    std::atomic<bool> mQuitting{ false }; // detach/quit in flight: no injections

    // Serializes stub injection (forceBreakIn) against stub cleanup and the
    // quitting transition: an injection and a cleanup can never interleave.
    std::mutex mBreakInMutex;

    // "breakon" switches: which event kinds may trigger a pause.
    bool mBreakOnEntry = false;
    bool mBreakOnDll = false;
    bool mBreakOnThread = false;
    bool mBreakOnException = true;   // matches the historic default
    GleeBug::ptr mOepBreakpoint = 0; // one-shot OEP breakpoint address (0 = none)

    std::map<GleeBug::ptr, uint32_t> mIgnoreHits;  // breakpoint address -> remaining ignores
    // Exception filters ("excfilter"): per-code break chance + disposition.
    struct ExFilter
    {
        int breakOn = 2;    // 0=first chance, 1=second chance, 2=never
        int handledBy = 0;  // 0=pass to debuggee (NOT_HANDLED), 1=swallow (DBG_CONTINUE)
    };
    std::map<uint32_t, ExFilter> mExFilters;
    EXCEPTION_RECORD mLastException{};
    bool mLastExceptionValid = false;
    bool mLastExceptionFirstChance = false;
    bool mPausedOnException = false; // the current pause is an exception stop
    uint32_t mSelectedThreadId = 0;                // 0 = follow the event thread
    bool mSymInitialized = false;                  // dbghelp session is up
    bool mHideOn = false;                          // anti-anti-debug enabled
    std::map<uint64_t, std::vector<uint8_t>> mPatches; // patch addr -> original bytes
    std::vector<std::pair<uint64_t, std::vector<uint8_t>>> mHideOriginals; // hide writes, for restore
    bool mHasLaunchInfo = false;    // launched (not attached): "restart" allowed
    bool mRestartPending = false;   // "restart" command consumed by main.cpp
};

#endif //GLEAM_DEBUGGER_H
