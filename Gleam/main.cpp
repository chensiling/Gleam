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
#include <iostream>
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

// Quote one argument following the Windows command-line rules
// (the ones CommandLineToArgvW parses):
//   - empty arguments and arguments containing space/tab/quote are quoted
//   - backslashes are doubled only before a quote or at the end
static std::wstring quoteArg(const wchar_t* arg)
{
    const bool needsQuotes = !*arg || wcscspn(arg, L" \t\"") != wcslen(arg);
    if(!needsQuotes)
        return arg;
    std::wstring out = L"\"";
    size_t backslashes = 0;
    for(const wchar_t* p = arg; ; p++)
    {
        if(*p == L'\\')
        {
            backslashes++;
            continue;
        }
        if(*p == L'"')
        {
            out.append(backslashes * 2 + 1, L'\\');
            out += L'"';
            backslashes = 0;
            continue;
        }
        if(*p == L'\0')
        {
            out.append(backslashes * 2, L'\\'); // trailing backslashes double before the closing quote
            break;
        }
        out.append(backslashes, L'\\');
        backslashes = 0;
        out += *p;
    }
    out += L'"';
    return out;
}

static std::atomic<bool> g_replStop{ false };

static void replThread(GleamDebugger* dbg)
{
    // std::getline handles arbitrarily long commands (no fixed buffer).
    std::string line;
    while(!g_replStop.load() && std::getline(std::cin, line))
    {
        while(!line.empty() && line.back() == '\r')
            line.pop_back();
        if(!line.empty())
        {
            // "pause" is the only command that acts while the debuggee is
            // running. Everything else is queued and executed at the next
            // suspended state - to detach/quit a running debuggee, issue
            // "pause" first. This keeps scripted command order deterministic.
            if(line == "pause")
                dbg->requestPause(); // deferral handled internally
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

    // Heap-allocated and intentionally leaked: the detached REPL thread may
    // still reference the debugger while the process is exiting.
    auto dbg = new GleamDebugger();
    bool attached = false;
    if(!wcscmp(argv[1], L"-a") || !wcscmp(argv[1], L"attach"))
    {
        if(argc < 3)
        {
            printf("usage: gleam -a <pid>\n");
            return 1;
        }
        DWORD pid = (DWORD)wcstoul(argv[2], nullptr, 0);
        if(!dbg->Attach(pid))
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
            commandLine += quoteArg(argv[i]);
        }
        // newConsole=false: the debuggee shares our console so its output is captured too.
        if(!dbg->Init(filePath.c_str(), commandLine.empty() ? nullptr : commandLine.c_str(), nullptr, false))
        {
            printf("failed to start debuggee '%s'\n", toUtf8(argv[1]).c_str());
            return 1;
        }
        printf("[gleam] debugging '%s'\n", toUtf8(argv[1]).c_str());
    }
    fflush(stdout);

    std::thread repl(replThread, dbg);
    dbg->Start();

    printf("[gleam] session finished%s\n", attached ? " (detached or target exited)" : "");
    fflush(stdout);

    // Controlled REPL shutdown: set the stop flag, then cancel the blocking
    // read. CancelSynchronousIo only cancels an in-flight read, so retry
    // until the thread actually exits (handles the "cancel before the next
    // read starts" race).
    g_replStop.store(true);
    while(WaitForSingleObject(repl.native_handle(), 50) != WAIT_OBJECT_0)
        CancelSynchronousIo(repl.native_handle());
    repl.join();
    delete dbg; // proper cleanup: no intentional leak
    return 0;
}
