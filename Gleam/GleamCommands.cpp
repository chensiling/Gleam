/**
 * @file GleamCommands.cpp
 * @brief Command dispatch, shared parsing helpers, and `help`.
 *
 * executeCommand() splits the line into arguments and offers it to each
 * `try*Command` handler in turn (control, breakpoints, inspect, symbols, scan);
 * the first handler that claims it wins. Unclaimed lines report an error rather
 * than being silently ignored.
 *
 * Also home to the two helpers every area shares: parseHex() and
 * normalizeModuleName().
 *
 * @see GleamDebugger for the file-by-file command split.
 */

#include "GleamDebugger.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace GleeBug;

bool parseHex(const std::string & s, uint64_t & out)
{
    if(s.empty())
        return false;
    const char* p = s.c_str();
    if(p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
        p += 2;
    char* end = nullptr;
    errno = 0;
    out = strtoull(p, &end, 16);
    if(errno == ERANGE)
        return false; // literal does not fit in 64 bits
    return end && *end == '\0' && end != p;
}

std::string normalizeModuleName(const std::string & name)
{
    // Keep only the basename.
    size_t slash = name.find_last_of("\\/");
    std::string base = slash == std::string::npos ? name : name.substr(slash + 1);
    // Lowercase (Windows module names are case-insensitive).
    for(auto & c : base)
        c = (char)tolower((unsigned char)c);
    // Strip a trailing ".dll" / ".exe".
    if(base.size() > 4 && (base.compare(base.size() - 4, 4, ".dll") == 0 ||
                           base.compare(base.size() - 4, 4, ".exe") == 0))
        base.resize(base.size() - 4);
    return base;
}

void GleamDebugger::cmdHelp()
{
    printf(
        "execution control:\n"
        "  g                       continue\n"
        "  step                    single step (into)\n"
        "  stepover                step over calls\n"
        "  ret [max]               run until current function returns (stops AFTER the\n"
        "                          ret, in the caller; x64dbg rtr stops BEFORE it)\n"
        "  pause                   interrupt a running debuggee\n"
        "  detach                  detach at the next suspended state (pause first if running)\n"
        "  quit                    terminate at the next suspended state (pause first if running)\n"
        "  restart                 terminate and re-launch the target (same path/args;\n"
        "                          logical breakpoints, exception filters and hide survive)\n"
        "breakpoints:\n"
        "  bp <hexaddr> [once]     set software breakpoint\n"
        "  bp <mod>!<sym> / <mod>+<rva>  module-relative bp (pending until the dll loads)\n"
        "  bp <addr> if <r><op><v>  conditional bp (op: == != < >)\n"
        "  trace <addr>            tracepoint (log hit, auto-continue)\n"
        "  rbp <hexaddr>           remove software breakpoint\n"
        "  hbp <hexaddr> [x|w|rw] [1|2|4|8]  set hardware breakpoint\n"
        "  hbpd <hexaddr>          remove hardware breakpoint\n"
        "  mbp <hexaddr> <hexsize> [a|r|w|x] set memory breakpoint\n"
        "  mbpd <hexaddr>          remove memory breakpoint\n"
        "  bl                      list breakpoints\n"
        "  ignore <hexaddr> <n>    skip the next n hits of a breakpoint\n"
        "inspection:\n"
        "  regs [name]             dump registers (GPR, EFLAGS, segments, DR, XMM,\n"
        "                          MXCSR); with a name, print just that register\n"
        "                          (also fsbase/gsbase, which are not in CONTEXT)\n"
        "  setreg <name> <hexval>  set register (rax..r15/rip + eax/ax/al etc slices,\n"
        "                          eflags, dr0-7, mxcsr; xmm0-15 = 32 hex chars high\n"
        "                          first. slice writes are read-modify-write (high\n"
        "                          bits kept). dr writes are raw: rejected while an\n"
        "                          engine hw bp exists, and block hbp afterwards.\n"
        "                          segments are read-only: the x64 kernel discards\n"
        "                          selector writes, and the bases are derived)\n"
        "  read <hexaddr> <size>   read memory (hex dump)\n"
        "  read u8|u16|u32|u64|ptr <addr>  typed read (single value)\n"
        "  read ansi|utf16 <addr> [n]  read string (default max 256)\n"
        "  savemem <addr> <size> <file>  export raw memory (page-granular, zero-filled holes)\n"
        "  write <hexaddr> <b...>  write memory (hex bytes)\n"
        "  disasm [hexaddr] [n]    disassemble n instructions (default: rip, 8)\n"
        "  maps                    list committed memory regions\n"
        "  meminfo <addr>          region details at addr (state/protect/type/module)\n"
        "  modules                 list loaded modules\n"
        "  find <addr> <size> <pat>  search memory (pattern with ?? wildcards)\n"
        "  find <addr> <size> ascii|utf16 <text>  search string\n"
        "  eval <expr>             evaluate an address expression\n"
        "  patch <addr> <b...>     patch memory (original bytes recorded)\n"
        "  patches                 list patches\n"
        "  restore <addr>          restore original bytes\n"
        "  stackscan [n]           scan stack for return addresses\n"
        "  sym <addr>              resolve address to symbol\n"
        "  until <addr>            run until address\n"
        "  hide [on|off]           anti-anti-debug (apply now + at system bp)\n"
        "  bt                      naive stack backtrace (rbp chain)\n"
        "  frames [tid] [n]        stack frames via RtlVirtualUnwind + own .pdata\n"
        "                          (best effort: verified for normal PE modules;\n"
        "                          frames in non-module memory are NOT trustworthy -\n"
        "                          use stackscan there)\n"
        "  exinfo                  show last exception\n"
        "  imports [module]        import table of a module (default: main)\n"
        "  exports <module> [pat]  exports of a module, optional wildcard filter\n"
        "  breakon [sw] [on|off]   pause switches: entry/dll/thread/exception\n"
        "\n"
        "addresses accept expressions: hex, registers, module, module!symbol,\n"
        "  [deref], seg:[deref], +/- and parentheses (e.g. bp kernel32!CreateFileW,\n"
        "  read [rsp+8] 10, eval gs:[60] = PEB. only gs has a base (the TEB))\n"
        "pauses are reported as: stop reason=<r> ... rip=0x... tid=<id>\n"
        "  threads                 list threads\n"
        "  thread [tid]            show/select the thread commands apply to\n"
        "exception filters:\n"
        "  ignoreexc <hexcode>     pass an exception code to the debuggee\n"
        "  exception pass|handle   disposition for the current exception stop\n"
        "  excfilter [add <code> [first|second|never] [pass|swallow] | del <code>]\n");
    fflush(stdout);
}

// Returns true when the debuggee should resume.
bool GleamDebugger::executeCommand(const std::string & cmdLine)
{
    std::vector<std::string> args;
    {
        size_t pos = 0;
        while(pos < cmdLine.size())
        {
            size_t sp = cmdLine.find_first_of(" \t", pos);
            if(sp == std::string::npos)
                sp = cmdLine.size();
            if(sp > pos)
                args.push_back(cmdLine.substr(pos, sp - pos));
            pos = sp + 1;
        }
    }
    if(args.empty())
        return false;

    if(args[0] == "help")
    {
        cmdHelp();
        return false;
    }

    static const struct
    {
        CmdResult(GleamDebugger::*handler)(const std::vector<std::string> &);
    } handlers[] = {
        { &GleamDebugger::tryControlCommand },
        { &GleamDebugger::tryBreakpointCommand },
        { &GleamDebugger::tryInspectCommand },
        { &GleamDebugger::trySymbolCommand },
        { &GleamDebugger::tryScanCommand },
    };
    for(const auto & entry : handlers)
    {
        auto result = (this->*entry.handler)(args);
        if(result == CmdResult::Resume)
        {
            mPausedOnException = false;
            return true;
        }
        if(result == CmdResult::Handled)
            return false;
    }

    printAddrError(); // surface a concrete address-parse reason, if any
    printf("unknown or malformed command (try 'help')\n");
    fflush(stdout);
    return false;
}
