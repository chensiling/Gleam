// ZombieTarget: pinned fixture for the ambiguous-symbol test (SYM-1).
//
// Two translation units each define a file-static function named "inner",
// so the PDB carries MULTIPLE records for the plain name "inner" - the same
// shape an incremental-link zombie record creates. gleam must resolve
// "ZombieTarget!inner" to the body that current code actually executes
// (the ILT thunk target), never to a record no live path references.
//
// Regenerating (manual): msbuild ZombieTarget.vcxproj -p:Configuration=Debug
// -p:Platform=x64, then copy bin\ZombieTarget.exe/.pdb into fixtures\.
#include <cstdio>
#include <cstdint>
#include <windows.h>

uint64_t innerA(uint64_t x);
uint64_t innerB(uint64_t x);

// innerA/innerB call their TU's OWN static "inner" (file scope picks the
// local record); only innerA is reachable from main.
int main()
{
    // Fixed base (no ASLR) keeps these addresses stable for the test suite.
    printf("INNERA=%p\n", (void*)&innerA);
    printf("INNERB=%p\n", (void*)&innerB);
    fflush(stdout);
    printf("R1=%llu\n", (unsigned long long)innerA(41));
    printf("R2=%llu\n", (unsigned long long)innerA(7));
    printf("RB=%llu\n", (unsigned long long)innerB(100));
    fflush(stdout);
    return 0;
}
