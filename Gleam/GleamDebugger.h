/**
 * @file GleamDebugger.h
 * @brief Command-driven headless Windows debugger built on the GleeBug engine.
 *
 * Declares GleamDebugger, the single class that owns a debug session: engine
 * event callbacks, the suspended-state command loop, and all per-session state.
 * Command implementations live in the GleamCommands.*.cpp files (see the class
 * documentation for the split).
 *
 * @section threading Thread model
 *
 * Two threads touch this object:
 *
 * - **Debugger thread** - the thread that called Init()/Attach() and Start().
 *   The GleeBug event loop runs here, so every `cb*` callback, commandLoop(),
 *   and every command implementation executes on it. The debuggee is suspended
 *   whenever this thread is inside a callback, which is what makes it the only
 *   thread allowed to touch `mProcess`/`mThread` and the engine's breakpoint
 *   tables.
 * - **REPL thread** - created by main.cpp, reads stdin. It may only call
 *   pushCommand(), requestPause(), pauseAfterResume(), and isPaused().
 *
 * @subsection guarded Shared state and its guards
 *
 * | State                        | Guard                                    |
 * |------------------------------|------------------------------------------|
 * | mCmdQueue                    | mCmdMutex + mCmdCv                       |
 * | mBreakInStub{Thread,Page,Tid}| std::atomic, plus mBreakInMutex for the  |
 * |                              | inject-vs-cleanup critical sections      |
 * | mIsPaused, mInDebugEvent     | std::atomic (published by the debugger    |
 * | mQuitting, mBreakInExpected  | thread, read by the REPL thread)         |
 * | mPauseAfterResume            | std::atomic, consumed by exchange()      |
 * | everything else              | debugger thread only - no guard needed   |
 *
 * @subsection invariants Invariants
 *
 * - Commands execute only while the debuggee is suspended, i.e. only from
 *   commandLoop(), i.e. only with `mIsPaused == true`.
 * - `mProcess` and `mThread` (engine members) are read on the debugger thread
 *   only. The REPL thread must never dereference them - that is the reason
 *   `pause` is the only command not routed through the queue.
 * - Stub injection (forceBreakIn) and stub teardown (cleanupBreakInStub) are
 *   serialized by mBreakInMutex and both check `mQuitting`, so a stub can
 *   never be injected into a session that is tearing down.
 * - `mBreakInStubPage` stays registered until VirtualFreeEx actually succeeds:
 *   a page a stub thread might still execute on is never freed, and a page
 *   whose free failed is never forgotten (detach refuses instead).
 *
 * @see PROGRESS.md for the platform facts behind these rules.
 */

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

// Forward declarations
namespace Gleam {
    class Logger;
    class PerfMonitor;
}

/**
 * @brief Parse a hexadecimal literal (GleamCommands.cpp).
 * @param s    Text to parse; an optional "0x" prefix is accepted.
 * @param out  Receives the value on success.
 * @return true on success; @p out is untouched on failure.
 */
bool parseHex(const std::string & s, uint64_t & out);

/**
 * @brief Canonical module-name form (GleamCommands.cpp).
 *
 * The single normalization point for module names: strips the directory,
 * lowercases, and removes a trailing ".dll"/".exe". Every comparison of a
 * user-supplied module name against a loaded module goes through this, so
 * `bp KERNEL32.DLL!Sleep` and `bp c:\windows\system32\kernel32!Sleep` name
 * the same module.
 */
std::string normalizeModuleName(const std::string & name);

/**
 * @brief Command-driven headless debugger based on GleeBug.
 *
 * The debug loop (Init/Attach + Start) runs on the caller's thread. A REPL
 * thread feeds commands via pushCommand(). Whenever the debuggee is suspended
 * by an interesting event (system breakpoint, breakpoint hit, single step,
 * unhandled exception), the debugger thread enters a command loop and executes
 * queued commands; `g`/`step`/`stepover`/`ret`/`detach`/`quit` leave the
 * command loop and resume the debuggee.
 *
 * Command implementations are split by functional area:
 *
 * | File                          | Commands                                   |
 * |-------------------------------|--------------------------------------------|
 * | GleamCommands.cpp             | dispatch, parsing helpers, `help`          |
 * | GleamCommands.Control.cpp     | `g` `step` `stepover` `tgo` `ret` `until` `detach` `quit` `thread` `alloc` `free` `protect` `breakon` `hide` |
 * | GleamCommands.Breakpoints.cpp | `bp` `rbp` `hbp` `mbp` `bl` `ignore` `trace` |
 * | GleamCommands.Inspect.cpp     | `regs` `setreg` `read` `write` `disasm` `maps` `modules` `find` `bt` `stackscan` `patch` `threads` |
 * | GleamCommands.Symbols.cpp     | `imports` `exports` `sym` `frames`, address parsing |
 * | GleamCommands.Expr.cpp        | `eval`, the address expression grammar     |
 * | GleamCommands.Scan.cpp        | `xref` `findasm`                           |
 * | GleamCommands.Hide.cpp        | anti-anti-debug applied by `hide`          |
 *
 * @warning Every method is debugger-thread-only unless its documentation says
 *          otherwise. See the @ref threading section in this file's header.
 */
class GleamDebugger : public GleeBug::Debugger
{
public:
    GleamDebugger();
    ~GleamDebugger();

    /// Per-instance logger (replaces the former global log level).
    Gleam::Logger& logger();
    /// Per-instance performance counters (replaces the former global stats).
    Gleam::PerfMonitor& perfMonitor();

    /**
     * @brief Queue a command line for execution. **REPL thread.**
     * @return true if the queue was empty before this push, i.e. the debuggee
     *         is likely running free and the caller should consider a pause.
     */
    bool pushCommand(const std::string & cmd);

    /**
     * @brief Interrupt a running debuggee. **REPL thread.**
     *
     * Publishes the request first, then races both ends to consume it
     * (set-then-check), so a request issued exactly at a resume boundary is
     * not lost. When `mProcess` is not ready yet the request is re-queued
     * rather than dropped.
     */
    void requestPause();

    /// Request quit from REPL thread. Processed at next debug event. **REPL thread.**
    void requestQuit();

    /// Request detach from REPL thread. Processed at next debug event. **REPL thread.**
    void requestDetach();

    /// True while the debugger thread sits in the command loop. **Any thread.**
    bool isPaused() const;

    /// Break in right after the next resume. **REPL thread.**
    void pauseAfterResume();

    /// Print help text (static, callable from REPL thread). **Any thread.**
    static void cmdHelp();

    /**
     * @name Break-in stub teardown
     *
     * The `pause` break-in works by injecting a thread that executes a page of
     * `int3`s. Tearing that down has a strict order - terminate the thread,
     * confirm it died, close the handle, and only then free the page - because
     * freeing memory a stub thread may still execute on crashes the debuggee.
     * @{
     */

    /**
     * @brief Request stub teardown.
     * @return true only when nothing remains tracked in the target.
     * @note `detach` refuses when this returns false: a held page must not leak.
     */
    bool cleanupBreakInStub();

    /**
     * @brief Free the tracked stub page through the (injectable) VirtualFreeEx path.
     * @return true when the page was freed or there was none.
     * @note The address is cleared **only** on success, so a failed free can be
     *       retried instead of leaking an unowned RWX page.
     */
    bool freeBreakInStubPage();

    /**
     * @brief Complete a detach that was deferred until the stub thread died.
     *
     * Frees the stub page, then calls Detach(). Refuses - staying attached -
     * when the page cannot be freed.
     */
    void finishDeferredDetach();
    /// @}

    /**
     * @name Session restart (`restart`)
     * main.cpp drives the re-Init + Start loop; this object is not destroyed,
     * which is how state survives across sessions.
     * @{
     */
    /// Mark the session as launched (not attached); `restart` requires it.
    void setLaunched(bool launched) { mHasLaunchInfo = launched; }
    /// Consume a pending restart request (main.cpp polls this).
    bool takeRestartRequest() { const bool r = mRestartPending; mRestartPending = false; return r; }

    /**
     * @brief Clear per-session state before a restart.
     *
     * Survives: logical breakpoints (they re-bind on module load), exception
     * filters, `breakon` switches, `hide`.
     * Cleared: patches, ignore counts, thread selection, last-exception state,
     * all transient stepping/trace state, and the symbol/ILT caches (the new
     * process may map the same modules at different bases).
     */
    void resetTransientState();
    /// @}

private:
    /**
     * @name Break-in injection
     * @{
     */
    /// Unguarded break-in; only valid at the just-before-continue point.
    void forceBreakIn();
    /// Lazily allocate and write the session stub page.
    bool ensureBreakInStub(GleeBug::Process* process);
    /**
     * @brief Last-resort break-in via DebugBreakProcess.
     * @warning Reads `PEB.BeingDebugged`, which `hide` zeroes - unusable while
     *          hiding is on. Sets the expectation flag only on success.
     */
    void fallbackDebugBreak(GleeBug::Process* process);
    /// Resolve/retry break-in symbols (debugger thread only, dbghelp).
    void resolveBreakInSymbols();
    /// @}

protected:
    /**
     * @name GleeBug engine callbacks
     *
     * All of these run on the debugger thread with the debuggee suspended.
     * Two ordering rules are load-bearing:
     *
     * - cbBreakpoint() must call handleStepOutBreakpoint() **first**. The engine
     *   deletes a one-shot breakpoint after the callback returns regardless of
     *   the path taken, so any early return before that bookkeeping strands the
     *   stepout operation waiting on an int3 that no longer exists.
     * - cbPostDebugEvent() enters the command loop **after** stepout re-arming,
     *   so a stop produced by a failed re-arm surfaces in the same event with a
     *   consistent RIP and context.
     * @{
     */
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
    /// @}

private:
    /// @note R is a member enum of GleeBug::Registers (declared inside the class).
    using RegId = GleeBug::Registers::R;

    /// Outcome of a `try*Command` handler.
    enum class CmdResult
    {
        NotMine,    ///< Command not handled by this handler; try the next one.
        Handled,    ///< Handled; the debuggee stays suspended.
        Resume      ///< Handled; resume the debuggee and leave the command loop.
    };

    /**
     * @name Command dispatch
     * Handlers are tried in this order from executeCommand(); the first one
     * that does not return CmdResult::NotMine owns the command.
     * @{
     */
    CmdResult tryControlCommand(const std::vector<std::string> & args);      ///< Control.cpp
    CmdResult tryBreakpointCommand(const std::vector<std::string> & args);   ///< Breakpoints.cpp
    CmdResult tryInspectCommand(const std::vector<std::string> & args);      ///< Inspect.cpp
    CmdResult trySymbolCommand(const std::vector<std::string> & args);       ///< Symbols.cpp
    CmdResult tryScanCommand(const std::vector<std::string> & args);         ///< Scan.cpp

    /// @return true when the debuggee should resume.
    bool executeCommand(const std::string & cmdLine);

    /**
     * @brief Execute queued commands while the debuggee is suspended.
     *
     * Runs on the debugger thread from a `cb*` callback and returns once a
     * command asks to resume (or the session is ending).
     */
    void commandLoop();

    /**
     * @brief Thread that register/memory inspection commands operate on.
     * @return The thread selected with `thread <tid>`, or the thread of the
     *         current debug event when no explicit selection is active.
     */
    GleeBug::Thread* currentThread();
    /// @}

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
    // Symbol resolution result: distinguishes "not found" (may bind later)
    // from "ambiguous" (must never bind). Replaces the cross-command-polluting
    // mSymbolAmbiguous boolean.
    enum class SymbolResult { Found, NotFound, Ambiguous };
    // Resolve module!symbol with full ILT-based disambiguation (all paths).
    // Look up a loaded module's base by name (case-insensitive, .dll optional).
    bool moduleBaseByName(const std::string & name, uint64_t & base);
    // Same, with image size (for RVA bounds checks).
    bool moduleInfoOf(const std::string & name, uint64_t & base, uint32_t & size);
    // Image size read directly from the PE at base (no loader list needed;
    // works during the DLL load event). 0 = cannot confirm.
    uint32_t moduleImageSize(uint64_t base);
    // Resolve "module!symbol" through the dbghelp session with ILT disambiguation.
    // Returns Found/NotFound/Ambiguous; only writes 'out' when Found.
    SymbolResult resolveModuleSymbol(const std::string & modSym, uint64_t & out);
    // Loader-list-independent module identity + export resolution (they work
    // during the DLL load event, when EnumProcessModules/dbghelp are blind).
    std::string dllNameFromBase(uint64_t base);
    uint64_t findExportByName(uint64_t base, const std::string & name);
    /**
     * @brief Resolve a PDB-only symbol by loading the module's symbols from disk.
     *
     * Works during the DLL load event, where the invade-based dbghelp session
     * is blind because the loader list is not linked yet. Applies the same
     * ILT-based disambiguation as resolveModuleSymbol().
     *
     * @param moduleBase Base the module is loaded at in the debuggee.
     * @param imagePath  Path to the image on disk; may be null when unknown.
     * @param symbol     Bare symbol name (no `module!` prefix).
     * @param out        Receives the address, written only on SymbolResult::Found.
     */
    SymbolResult resolvePdbSymbol(uint64_t moduleBase, const wchar_t* imagePath,
                                   const std::string & symbol, uint64_t & out);

    /**
     * @brief Prove "this loaded module IS that file on disk".
     *
     * Compares CodeView GUID + Age + SizeOfImage.
     * @warning Never use symbol resolution as proof of identity: symbols from
     *          any file can be loaded against any base, which makes that test
     *          self-fulfilling.
     */
    bool verifyModuleIdentity(uint64_t moduleBase, const wchar_t* imagePath);
    // Bases we SymLoadModuleEx'd explicitly (for SymUnloadModule64 pairing).
    std::set<uint64_t> mSymLoadedBases;
    // Real image paths learned at DLL load events, keyed by normalized
    // module name: later events for the same module may have hFile == NULL.
    std::map<std::string, std::wstring> mModulePaths;

    /**
     * @name Symbol and ILT caches
     *
     * Both caches key on data that a module reload invalidates, so their
     * lifetime is tied to module identity:
     *
     * - mIltCache keys on the module **base**. A different module loaded at a
     *   recycled base would otherwise be disambiguated against the previous
     *   module's thunk targets.
     * - mSymbolCache keys on `"module!symbol"`, which does **not** encode the
     *   base. A module that unloads and reloads at a different base would
     *   otherwise resolve to its previous address - and a breakpoint written
     *   there lands in unrelated memory.
     *
     * @warning Every entry must therefore be dropped when the module it
     *          describes goes away: cbUnloadDllEvent() for a single module,
     *          resetTransientState() for a whole session.
     * @{
     */

    /// Module base -> ILT thunk targets, caching expensive iltThunkTargets() scans.
    std::unordered_map<uint64_t, std::unordered_set<uint64_t>> mIltCache;
    /// Drop every ILT entry (whole-session invalidation).
    void clearIltCache() { mIltCache.clear(); }
    /**
     * @brief ILT thunk targets of a module, scanning only on a cache miss.
     * @return Pointer to the cached set, or nullptr when the scan failed.
     *         Valid until the next invalidation of this module.
     */
    const std::unordered_set<uint64_t>* getIltTargets(uint64_t moduleBase);

    /// `"module!symbol"` -> resolved address (successful resolutions only).
    std::unordered_map<std::string, uint64_t> mSymbolCache;
    /// Drop every symbol entry (whole-session invalidation).
    void clearSymbolCache() { mSymbolCache.clear(); }
    /// @return The cached address, or 0 when the symbol is not cached.
    uint64_t getCachedSymbol(const std::string& modSym);
    /// Record a successful resolution. Never called for ambiguous symbols.
    void cacheSymbol(const std::string& modSym, uint64_t addr);
    /// @}

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

    /**
     * @name Instance-based services
     * These replaced process-wide globals so several debugger instances can
     * coexist (see GLOBAL_STATE_AUDIT.md).
     * @{
     */
    Gleam::Logger* mLogger;            ///< Owns the log level (was a global).
    Gleam::PerfMonitor* mPerfMonitor;  ///< Owns the perf counters (was a global).
    /// @}

    /**
     * @name Command queue (cross-thread)
     * The only channel from the REPL thread into the debugger thread.
     * @{
     */
    std::queue<std::string> mCmdQueue;      ///< Guarded by mCmdMutex.
    std::mutex mCmdMutex;                   ///< Guards mCmdQueue.
    std::condition_variable mCmdCv;         ///< Signals a newly queued command.
    std::atomic<bool> mIsPaused{ false };   ///< Debugger thread is in commandLoop().
    std::atomic<bool> mInDebugEvent{ false }; ///< Between event delivery and ContinueDebugEvent.
    /// @}

    /**
     * @name Break-in state (cross-thread)
     *
     * `pause` cannot stop the debuggee directly: it injects a thread that runs
     * into an `int3` on a page we own. Recognition is by **exception address**
     * (the stub page, then ntdll!DbgUiRemoteBreakin, then a fallback flag),
     * which decouples it from any bookkeeping order.
     * @{
     */
    std::atomic<bool> mBreakInExpected{ false };  ///< A `pause` break-in is on its way.
    std::atomic<bool> mPauseAfterResume{ false }; ///< `pause` arrived while already paused.
    std::atomic<bool> mQuitRequested{ false };    ///< `quit` requested from REPL thread.
    std::atomic<bool> mDetachRequested{ false };  ///< `detach` requested from REPL thread.
    std::atomic<HANDLE> mBreakInStubThread{ nullptr }; ///< Injected int3-stub thread.
    std::atomic<void*> mBreakInStubPage{ nullptr };    ///< Page backing the stub; cleared only after a successful free.
    std::atomic<uint32_t> mBreakInStubTid{ 0 };        ///< Stub tid, used to confirm its death.
    /**
     * @brief Detach is waiting for the stub thread to be confirmed dead.
     *
     * The session never detaches while a remote RWX page of ours is still
     * mapped; finishDeferredDetach() completes the operation from the
     * EXIT_THREAD event.
     */
    bool mDetachAfterStubCleanup = false;
    /// @}

    /**
     * @name Fault injection (`selftest failapi ...`)
     * Each flag fails the matching API call **once** in the break-in stub
     * paths and then clears itself, so an armed-but-untriggered flag cannot
     * leak into the next session.
     * @{
     */
    bool mFailNextTerminate = false;  ///< Fail TerminateThread on the stub thread.
    bool mFailNextStubResume = false; ///< Fail ResumeThread of a fresh stub thread.
    bool mFailNextVfree = false;      ///< Fail VirtualFreeEx of the stub page.
    /// @}

    std::atomic<uint64_t> mDbgBreakInAddr{ 0 }; ///< ntdll!DbgUiRemoteBreakin, a fallback break-in identity.
    uint32_t mExitThreadResolveAttempts = 0;    ///< Rate-limits symbol-retry logging.

    /**
     * @name Execution control state
     * @{
     */
    bool mWantsPause = false;     ///< This event should surface as a user stop.
    bool mStepArmed = false;      ///< A user-requested step is in flight.
    bool mStepOverArmed = false;  ///< A user-requested step-over is in flight.
    /**
     * @brief detach/quit is in flight; no new stub injections.
     * @note Atomic because the REPL thread reads it to decide whether a pause
     *       request is still meaningful.
     */
    std::atomic<bool> mQuitting{ false };
    /// @}

    /**
     * @brief Serializes stub injection against stub cleanup and the quitting
     *        transition, so an injection and a teardown can never interleave.
     */
    std::mutex mBreakInMutex;

    /**
     * @name `breakon` switches
     * Which event kinds may surface as a user stop. Survives a restart.
     * @{
     */
    bool mBreakOnEntry = false;
    bool mBreakOnDll = false;
    bool mBreakOnThread = false;
    bool mBreakOnException = true;   ///< Matches the historic default.
    /**
     * @brief One-shot OEP breakpoint address (0 = none).
     * @note Armed lazily by applyEntryBreakpoint(): `breakon entry on` usually
     *       runs at the system breakpoint, long after the process-creation
     *       event, so enabling it has to arm the breakpoint retroactively.
     */
    GleeBug::ptr mOepBreakpoint = 0;
    /// @}

    std::map<GleeBug::ptr, uint32_t> mIgnoreHits;  ///< Breakpoint address -> remaining ignores.
    // Exception filters ("excfilter"): per-code break chance + disposition.
    struct ExFilter
    {
        int breakOn = 2;    // 0=first chance, 1=second chance, 2=never
        int handledBy = 0;  // 0=pass to debuggee (NOT_HANDLED), 1=swallow (DBG_CONTINUE)
    };
    std::map<uint32_t, ExFilter> mExFilters;  ///< Exception code -> filter. Survives a restart.

    /**
     * @name Last exception
     * Feeds `exinfo` and the `exception pass|handle` disposition commands.
     * @{
     */
    EXCEPTION_RECORD mLastException{};
    bool mLastExceptionValid = false;
    bool mLastExceptionFirstChance = false;
    /// The current pause is an exception stop; any resume-class command clears it.
    bool mPausedOnException = false;
    /// @}

    uint32_t mSelectedThreadId = 0;  ///< `thread <tid>` selection; 0 = follow the event thread.
    bool mSymInitialized = false;    ///< dbghelp session is up (created lazily).
    bool mHideOn = false;            ///< Anti-anti-debug enabled; re-applied at each session start.

    /// Patch address -> original bytes. First write wins, so `restore` is exact.
    std::map<uint64_t, std::vector<uint8_t>> mPatches;
    /// Writes made by `hide`, replayed in reverse by `hide off`.
    std::vector<std::pair<uint64_t, std::vector<uint8_t>>> mHideOriginals;

    bool mHasLaunchInfo = false;    ///< Launched rather than attached: `restart` is allowed.
    bool mRestartPending = false;   ///< `restart` was requested; main.cpp consumes it.
};

#endif //GLEAM_DEBUGGER_H
