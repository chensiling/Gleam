// BoundaryTarget: allocates a 2MB RWX region and places a call instruction
// straddling the 1MB boundary, for cross-chunk scan testing.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <windows.h>

int main()
{
    // Fixed hint address so tests can hardcode it (fails loudly if taken).
    auto base = (uint8_t*)VirtualAlloc((LPVOID)0x60000000, 0x200000,
                                       MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if(!base)
    {
        printf("ALLOC_FAIL\n");
        fflush(stdout);
        return 1;
    }
    memset(base, 0x90, 0x200000); // nop sled

    base[0] = 0xC3; // ret at base: the (unique) call target
    base[0x100] = 0xE8; // control call, NOT straddling
    int32_t rel2 = (int32_t)(base - (base + 0x105));
    memcpy(base + 0x101, &rel2, 4);

    // call rel32 (5 bytes) starting 3 bytes before the 1MB mark:
    // it straddles the boundary between two 1MB scan chunks.
    uint8_t* p = base + 0x100000 - 3;
    p[0] = 0xE8;
    int32_t rel = (int32_t)(base - (p + 5));
    memcpy(p + 1, &rel, 4);

    printf("BASE=%p\n", (void*)base);
    printf("STRADDLER=%p\n", (void*)p);
    fflush(stdout);

    __debugbreak(); // hand control to the debugger with the region in place
    return 0;
}
