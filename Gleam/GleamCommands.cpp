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
    out = strtoull(p, &end, 16);
    return end && *end == '\0' && end != p;
}

void GleamDebugger::cmdHelp()
{
    printf(
        "execution control:\n"
        "  g                       continue\n"
        "  step                    single step (into)\n"
        "  stepover                step over calls\n"
        "  ret                     run until current function returns\n"
        "  pause                   interrupt a running debuggee\n"
        "  detach                  detach at the next suspended state (pause first if running)\n"
        "  quit                    terminate at the next suspended state (pause first if running)\n"
        "breakpoints:\n"
        "  bp <hexaddr> [once]     set software breakpoint\n"
        "  rbp <hexaddr>           remove software breakpoint\n"
        "  hbp <hexaddr> [x|w|rw] [1|2|4|8]  set hardware breakpoint\n"
        "  hbpd <hexaddr>          remove hardware breakpoint\n"
        "  mbp <hexaddr> <hexsize> [a|r|w|x] set memory breakpoint\n"
        "  mbpd <hexaddr>          remove memory breakpoint\n"
        "  bl                      list breakpoints\n"
        "  ignore <hexaddr> <n>    skip the next n hits of a breakpoint\n"
        "inspection:\n"
        "  regs                    dump registers\n"
        "  setreg <name> <hexval>  set register (rax..r15, rip)\n"
        "  read <hexaddr> <size>   read memory (hex dump)\n"
        "  write <hexaddr> <b...>  write memory (hex bytes)\n"
        "  disasm [hexaddr] [n]    disassemble n instructions (default: rip, 8)\n"
        "  maps                    list committed memory regions\n"
        "  modules                 list loaded modules\n"
        "  find <addr> <size> <pat>  search memory (pattern with ?? wildcards)\n"
        "  bt                      naive stack backtrace (rbp chain)\n"
        "  exinfo                  show last exception\n"
        "  imports [module]        import table of a module (default: main)\n"
        "  exports <module> [pat]  exports of a module, optional wildcard filter\n"
        "  threads                 list threads\n"
        "  thread [tid]            show/select the thread commands apply to\n"
        "exception filters:\n"
        "  ignoreexc <hexcode>     pass an exception code to the debuggee\n");
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
    };
    for(const auto & entry : handlers)
    {
        auto result = (this->*entry.handler)(args);
        if(result == CmdResult::Resume)
            return true;
        if(result == CmdResult::Handled)
            return false;
    }

    printf("unknown or malformed command (try 'help')\n");
    fflush(stdout);
    return false;
}
