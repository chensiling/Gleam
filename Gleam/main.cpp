// Gleam: command-driven headless debugger based on GleeBug.
//
// Usage:
//   gleam <target.exe> [args...]   start and debug a process
//   gleam -a <pid>                 attach to a running process
//
// Commands are read from stdin (try 'help'). The debug loop runs on the main
// thread; the REPL thread below forwards stdin lines to the debugger.
//
// wmain is used so target paths with non-ASCII characters (e.g. Chinese)
// arrive as proper UTF-16 instead of lossy ANSI (CP_ACP) argv.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include <windows.h>

#include "GleamDebugger.h"

// Wide (UTF-16) -> UTF-8, for our narrow-string output.
static std::string toUtf8(const wchar_t* wide)
{
    int size = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if(size <= 1)
        return std::string();
    std::string result((size_t)size - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, &result[0], size, nullptr, nullptr);
    return result;
}

static void replThread(GleamDebugger* dbg)
{
    std::string line;
    char buf[1024];
    while(fgets(buf, sizeof(buf), stdin))
    {
        line = buf;
        while(!line.empty() && (line.back() == '\n' || line.back() == '\r'))
            line.pop_back();
        if(!line.empty())
        {
            // "pause" is the only command that acts while the debuggee is
            // running. Everything else is queued and executed at the next
            // suspended state - to detach/quit a running debuggee, issue
            // "pause" first. This keeps scripted command order deterministic.
            if(line == "pause")
            {
                // Already suspended: defer the break-in to right after the
                // resume instead of dropping the request.
                if(dbg->isPaused())
                    dbg->pauseAfterResume();
                else
                    dbg->requestPause();
            }
            else
                dbg->pushCommand(line);
        }
    }
    // NOTE: no automatic quit on EOF - a script must end with an explicit
    // quit/detach if it wants the session to end before the debuggee exits.
}

int wmain(int argc, wchar_t* argv[])
{
    // Our printf output is UTF-8; make the console render it correctly.
    SetConsoleOutputCP(CP_UTF8);

    if(argc < 2)
    {
        printf("usage: gleam <target.exe> [args...]\n");
        printf("       gleam -a <pid>\n");
        printf("commands are read from stdin (try 'help')\n");
        return 1;
    }

    GleamDebugger dbg;
    bool attached = false;
    if(!wcscmp(argv[1], L"-a") || !wcscmp(argv[1], L"attach"))
    {
        if(argc < 3)
        {
            printf("usage: gleam -a <pid>\n");
            return 1;
        }
        DWORD pid = (DWORD)wcstoul(argv[2], nullptr, 0);
        if(!dbg.Attach(pid))
        {
            printf("failed to attach to process %lu\n", pid);
            return 1;
        }
        attached = true;
        printf("[gleam] attached to process %lu\n", pid);
    }
    else
    {
        std::wstring filePath(argv[1]);
        std::wstring commandLine;
        for(int i = 2; i < argc; i++)
        {
            if(!commandLine.empty())
                commandLine += L' ';
            commandLine += argv[i];
        }
        // newConsole=false: the debuggee shares our console so its output is captured too.
        if(!dbg.Init(filePath.c_str(), commandLine.empty() ? nullptr : commandLine.c_str(), nullptr, false))
        {
            printf("failed to start debuggee '%s'\n", toUtf8(argv[1]).c_str());
            return 1;
        }
        printf("[gleam] debugging '%s'\n", toUtf8(argv[1]).c_str());
    }
    fflush(stdout);

    std::thread repl(replThread, &dbg);
    dbg.Start();
    repl.join();

    printf("[gleam] session finished%s\n", attached ? " (detached or target exited)" : "");
    fflush(stdout);
    return 0;
}
