// Late.dll: has PDB symbols but NO exports. Delayed-breakpoint tests use
// the PDB-only fallback (export walk fails -> dbghelp after the loader
// list becomes visible).
#include <windows.h>

// Non-static so the PDB carries it, but NOT dllexport'ed: the export table
// stays empty and symbol resolution must come from the PDB.
__declspec(noinline) int LateInternal(int x)
{
    return x * 7 + 1;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID)
{
    if(reason == DLL_PROCESS_ATTACH)
    {
        volatile int sink = LateInternal(6);
        (void)sink;
    }
    return TRUE;
}
