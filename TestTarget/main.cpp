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

static DWORD WINAPI worker(LPVOID)
{
    Sleep(60000);
    return 0;
}

int main(int argc, char** argv)
{
    if(argc > 1 && !strcmp(argv[1], "exc"))
    {
        // One continuable custom exception, then clean exit.
        RaiseException(0xE0DEAD00, 0, 0, nullptr);
        printf("SURVIVED_EXCEPTION\n");
        fflush(stdout);
        return 0;
    }

    CreateThread(nullptr, 0, worker, nullptr, 0, nullptr);

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

    // A self-write so data breakpoints have something to catch.
    g_data[0] = 0x58;
    printf("GDATA_AFTER=");
    for(int i = 0; i < 16; i++)
        printf("%02X", g_data[i]);
    printf("\n");
    fflush(stdout);
    return 0;
}
