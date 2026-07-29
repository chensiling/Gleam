#include <cstdio>
#include <cstdint>
#include <cstring>
#include <windows.h>

// Known content for the debugger to read back: "GLEAM-TEST-DATA!"
__declspec(align(16)) uint8_t g_data[16] = {
    0x47, 0x4C, 0x45, 0x41, 0x4D, 0x2D, 0x54, 0x45,
    0x53, 0x54, 0x2D, 0x44, 0x41, 0x54, 0x41, 0x21
};

// In "mt" mode the callee of marker() burns cycles ON THE MAIN THREAD ONLY,
// so the owner's call stays in flight while busyWorker (fast inner) crosses
// the internal breakpoint address many times.
static bool g_slowInner = false;
static DWORD g_mainTid = 0;
static volatile LONG g_gate = 0; // 2 = main is inside its slow inner call

// inner(41)=46; marker(41)=47. With /Od the first argument arrives in RCX.
__declspec(noinline) uint64_t inner(uint64_t x)
{
    volatile uint64_t k = x;
    if(g_slowInner && GetCurrentThreadId() == g_mainTid)
    {
        InterlockedExchange(&g_gate, 2); // busyWorker may launch marker(1) now
        for(volatile uint64_t i = 0; i < 10000000; i++)
            k += 0;
    }
    return k + 5;
}

__declspec(noinline) uint64_t marker(uint64_t x)
{
    volatile uint64_t keep = x;
    return inner(keep) + 1;
}

// A function with a real loop: stepout must fast-forward it, not single-step
// all 100000 iterations.
__declspec(noinline) uint64_t looper(uint64_t n)
{
    volatile uint64_t sum = 0;
    for(uint64_t i = 0; i < n; i++)
        sum += i;
    return sum;
}

static DWORD WINAPI worker(LPVOID)
{
    Sleep(60000);
    return 0;
}

// "mtx" mode: a thread that never returns from its callee. stepout arms its
// call-skip breakpoint after "call exitCallee" and resumes at full speed, but
// the callee calls ExitThread - so the OWNER thread dies while the internal
// breakpoint is still armed (the owner-exit abort path).
__declspec(noinline) uint64_t exitCallee(uint64_t x)
{
    volatile uint64_t k = x;
    if(k)
        ExitThread(0);
    return k;
}

__declspec(noinline) uint64_t exiter(uint64_t x)
{
    volatile uint64_t k = x;
    k = exitCallee(k) + 1;
    return k;
}

static DWORD WINAPI exitWorker(LPVOID)
{
    exiter(1);
    return 0;
}

// "mt" mode: hammer marker() so stepout's internal call-skip breakpoint
// gets hit by this (non-owner) thread while the main thread stepouts.
static DWORD WINAPI busyWorker(LPVOID)
{
    // Launch marker(1) exactly when main is inside its slow inner call, so
    // the internal call-skip breakpoint (marker body, after the call) is
    // crossed while it is armed.
    while(g_gate < 2)
        Sleep(0);
    for(int i = 0; i < 12; i++)
        marker(1);
    // Thread-level completion marker: proves this thread actually resumed and
    // ran to its end (process exit alone cannot - ExitProcess also kills
    // frozen threads). W16's resume-failure injection asserts on this line.
    printf("BUSYWORKER_DONE=1\n");
    fflush(stdout);
    return 0;
}

// "mtl" mode: same gate alignment, but hammers for a long time. The
// reply-later fault injection needs a partner thread whose exceptions keep
// overlapping main's internal-step windows; 12 calls are over in ~1ms with
// an ignored breakpoint, after which main hammers alone and no deferral can
// ever happen (Release gate proved this: ~1M solo hits, zero deferrals).
static DWORD WINAPI busyWorkerLong(LPVOID)
{
    while(g_gate < 2)
        Sleep(0);
    for(int i = 0; i < 5000; i++)
        marker(1);
    printf("BUSYWORKER_DONE=1\n");
    fflush(stdout);
    return 0;
}

int main(int argc, char** argv)
{
    if(argc > 1 && !strcmp(argv[1], "exc"))
    {
        // One custom exception, handled in-process; survives unless the
        // debugger terminates us first.
        __try
        {
            RaiseException(0xE0DEAD00, 0, 0, nullptr);
        }
        __except(EXCEPTION_EXECUTE_HANDLER)
        {
        }
        printf("SURVIVED_EXCEPTION\n");
        fflush(stdout);
        return 0;
    }

    if(argc > 1 && !strcmp(argv[1], "dll"))
    {
        // Load a DLL that is NOT loaded at process creation, so the debugger
        // can exercise module-relative pending breakpoints. Falls through to
        // the normal body afterwards.
        HMODULE ver = LoadLibraryW(L"version.dll");
        if(ver)
        {
            printf("DLL_LOADED=1\n");
            auto fn = (DWORD(WINAPI*)(LPCWSTR, LPDWORD))GetProcAddress(ver, "GetFileVersionInfoSizeW");
            if(fn)
            {
                DWORD handle = 0;
                DWORD size = fn(L"C:\\Windows\\notepad.exe", &handle);
                printf("DLLCALL_RESULT=%lu\n", size);
            }
        }
        fflush(stdout);
    }

    if(argc > 1 && !strcmp(argv[1], "dll2"))
    {
        // Load DLLs with no export table (identity must come from the real
        // path / loader list, not the export dir) and a PDB-only symbol.
        HMODULE noexp = LoadLibraryW(L"NoExp.dll");
        printf("NOEXP_LOADED=%d\n", noexp != nullptr);
        HMODULE late = LoadLibraryW(L"Late.dll");
        printf("LATE_LOADED=%d\n", late != nullptr);
        fflush(stdout);
    }

    if(argc > 1 && !strcmp(argv[1], "av"))
    {
        // Unhandled access violation: first chance, second chance, death.
        *(volatile int*)nullptr = 0;
        return 0;
    }

    if(argc > 1 && !strcmp(argv[1], "dll3"))
    {
        // Load -> unload -> reload: delayed-breakpoint unbind/re-bind cycle.
        HMODULE late = LoadLibraryW(L"Late.dll");
        printf("LATE1=%d\n", late != nullptr);
        if(late)
            FreeLibrary(late);
        printf("LATE_UNLOADED=1\n");
        late = LoadLibraryW(L"Late.dll");
        printf("LATE2=%d\n", late != nullptr);
        fflush(stdout);
    }

    if(argc > 1 && !strcmp(argv[1], "dll4"))
    {
        // Decoy test: Late unload -> NoExp decoy load -> Late reload.
        HMODULE late = LoadLibraryW(L"Late.dll");
        printf("LATE1=%d\n", late != nullptr);
        if(late)
            FreeLibrary(late);
        printf("LATE_UNLOADED=1\n");
        HMODULE decoy = LoadLibraryW(L"NoExp.dll");
        printf("DECOY_LOADED=%d\n", decoy != nullptr);
        late = LoadLibraryW(L"Late.dll");
        printf("LATE2=%d\n", late != nullptr);
        fflush(stdout);
    }

    if(argc > 1 && !strcmp(argv[1], "mtx"))
    {
        // Owner-exit scenario: stepout on this thread, which dies inside its
        // callee while the internal call-skip breakpoint is still armed.
        printf("EXITER=%p\n", (void*)&exiter);
        fflush(stdout);
        HANDLE h = CreateThread(nullptr, 0, exitWorker, nullptr, 0, nullptr);
        if(h)
        {
            WaitForSingleObject(h, 10000);
            CloseHandle(h);
        }
        printf("EXITER_DONE=1\n");
        fflush(stdout);
    }

    if(argc > 1 && !strcmp(argv[1], "wait"))
    {
        // Long-lived, always-runnable target for attach scenarios: main spins
        // (steppable at any moment), a second thread sleeps (a thread that
        // can be frozen by a failed resume). Runs ~30s unless killed earlier.
        CreateThread(nullptr, 0, worker, nullptr, 0, nullptr);
        printf("WAIT_READY=1\n");
        fflush(stdout);
        DWORD start = GetTickCount();
        volatile uint64_t k = 0;
        while(GetTickCount() - start < 30000)
            k += 0;
        printf("WAIT_DONE=%llu\n", (unsigned long long)k);
        fflush(stdout);
        return 0;
    }

    if(argc > 1 && !strcmp(argv[1], "mt"))
    {
        // A second thread hammering marker(), so stepout's internal
        // call-skip breakpoint can be hit by a non-owner thread.
        g_slowInner = true;
        g_mainTid = GetCurrentThreadId();
        CreateThread(nullptr, 0, busyWorker, nullptr, 0, nullptr);
    }
    else if(argc > 1 && !strcmp(argv[1], "mtl"))
    {
        // Same alignment as mt, but a LONG-LIVED hammering partner for the
        // reply-later fault injection (see busyWorkerLong).
        g_slowInner = true;
        g_mainTid = GetCurrentThreadId();
        CreateThread(nullptr, 0, busyWorkerLong, nullptr, 0, nullptr);
    }
    else
        CreateThread(nullptr, 0, worker, nullptr, 0, nullptr);

    printf("ISDEBUGGERPRESENT=%d\n", IsDebuggerPresent() ? 1 : 0);
    fflush(stdout);

    // Fixed base (no ASLR) makes these addresses stable across runs.
    printf("MARKER=%p\n", (void*)&marker);
    printf("INNER=%p\n", (void*)&inner);
    printf("GDATA=%p\n", (void*)g_data);
    printf("GDATA_BYTES=");
    for(int i = 0; i < 16; i++)
        printf("%02X", g_data[i]);
    printf("\n");
    fflush(stdout);

    // Two calls so one-shot breakpoints and ignore counts can be exercised.
    uint64_t r1 = marker(41);
    printf("MARKER_RESULT_1=%llu\n", (unsigned long long)r1);
    uint64_t r2 = marker(7);
    printf("MARKER_RESULT_2=%llu\n", (unsigned long long)r2);

    printf("LOOPER=%p\n", (void*)&looper);
    fflush(stdout);
    uint64_t r3 = looper(100000);
    printf("LOOP_RESULT=%llu\n", (unsigned long long)r3);
    r3 = looper(100000);
    printf("LOOP_RESULT=%llu\n", (unsigned long long)r3);

    // A self-write so data breakpoints have something to catch.
    g_data[0] = 0x58;
    printf("GDATA_AFTER=");
    for(int i = 0; i < 16; i++)
        printf("%02X", g_data[i]);
    printf("\n");
    fflush(stdout);
    return 0;
}
