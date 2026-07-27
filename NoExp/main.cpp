// NoExp.dll: a DLL with NO exported functions and no export directory name
// entry, for delayed-breakpoint identity tests (hFile real path / loader
// list must identify it; the export-dir alias path must not be the key).
#include <windows.h>

// Static on purpose: nothing may appear in the export table.
static DWORD g_marker = 0xDEADBEEF;

static void hiddenWork()
{
    g_marker++;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID)
{
    if(reason == DLL_PROCESS_ATTACH)
        hiddenWork();
    return TRUE;
}
