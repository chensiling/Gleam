#include <cstdio>
#include <cstdint>
#include <cstring>
#include <windows.h>

// Known content for the debugger to read back: "GLEAM-TEST-DATA!"
__declspec(align(16)) uint8_t g_data[16] = {
    0x47, 0x4C, 0x45, 0x41, 0x4D, 0x2D, 0x54, 0x45,
    0x53, 0x54, 0x2D, 0x44, 0x41, 0x54, 0x41, 0x21
};

// inner(41)=46; marker(41)=47. With /Od the first argument arrives in RCX.
__declspec(noinline) uint64_t inner(uint64_t x)
{
    volatile uint64_t k = x;
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

    // A self-write so data breakpoints have something to catch.
    g_data[0] = 0x58;
    printf("GDATA_AFTER=");
    for(int i = 0; i < 16; i++)
        printf("%02X", g_data[i]);
    printf("\n");
    fflush(stdout);
    return 0;
}
