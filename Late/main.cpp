// Late.dll: has PDB symbols but NO exports. Delayed-breakpoint tests use
// the PDB-only fallback (export walk fails -> dbghelp after the loader
// list becomes visible).
//
// Now also includes an ambiguous symbol (duplicate static "ambig" across
// ambig_a.cpp and ambig_b.cpp) to test SYM-1: pending breakpoint on
// ambiguous PDB-only symbol must be refused at bind time.
#include <windows.h>

// Non-static so the PDB carries it, but NOT dllexport'ed: the export table
// stays empty and symbol resolution must come from the PDB.
__declspec(noinline) int LateInternal(int x)
{
    return x * 7 + 1;
}

// From ambig_a.cpp - calls the live "ambig" static.
int LateAmbigA();

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID)
{
    if(reason == DLL_PROCESS_ATTACH)
    {
        volatile int sink = LateInternal(6);
        (void)sink;
        volatile int sink2 = LateAmbigA();
        (void)sink2;
    }
    return TRUE;
}
