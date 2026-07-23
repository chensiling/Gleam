// Anti-anti-debug ("hide"): patch the common debugger-detection vectors in
// the debuggee. Best applied at the system breakpoint, before any target
// code has run.
//
// Coverage (v1):
//   - PEB.BeingDebugged
//   - PEB.NtGlobalFlag
//   - ProcessHeap Flags / ForceFlags debug bits
//   - kernelbase!IsDebuggerPresent         -> return FALSE
//   - kernelbase!CheckRemoteDebuggerPresent -> *pbDebuggerPresent = FALSE
// NOT covered (documented limitation): NtQueryInformationProcess-based checks
// (ProcessDebugPort/ProcessDebugObjectHandle/ProcessDebugFlags), which need
// an ntdll hook.

#include "GleamDebugger.h"

#include <cstdio>
#include <cstring>

using namespace GleeBug;

typedef LONG NTSTATUS;
typedef NTSTATUS(NTAPI *tNtQueryInformationThread)(HANDLE, ULONG, PVOID, ULONG, PULONG);

namespace
{
    struct ThreadBasicInfo
    {
        NTSTATUS ExitStatus;
        PVOID TebBaseAddress;
        ULONG_PTR ClientIdProcess;
        ULONG_PTR ClientIdThread;
        ULONG_PTR AffinityMask;
        LONG Priority;
        LONG BasePriority;
    };

#ifdef _WIN64
    const uint32_t kTebPebOffset = 0x60;
    const uint32_t kPebBeingDebugged = 0x02;
    const uint32_t kPebNtGlobalFlag = 0xBC;
    const uint32_t kPebProcessHeap = 0x30;
    const uint32_t kHeapFlags = 0x70;
    const uint32_t kHeapForceFlags = 0x74;
#else
    const uint32_t kTebPebOffset = 0x30;
    const uint32_t kPebBeingDebugged = 0x02;
    const uint32_t kPebNtGlobalFlag = 0x68;
    const uint32_t kPebProcessHeap = 0x18;
    const uint32_t kHeapFlags = 0x40;
    const uint32_t kHeapForceFlags = 0x44;
#endif

    const uint32_t kHeapDebugBits = 0x40000003; // TAIL_CHECKING|FREE_CHECKING|VALIDATE_PARAMETERS

    uint64_t pebAddress(Process* process, HANDLE hThread)
    {
        static auto queryThread = (tNtQueryInformationThread)GetProcAddress(
            GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread");
        if(!queryThread)
            return 0;
        ThreadBasicInfo tbi{};
        if(queryThread(hThread, 0 /*ThreadBasicInformation*/, &tbi, sizeof(tbi), nullptr) < 0)
            return 0;
        uint64_t teb = (uint64_t)tbi.TebBaseAddress;
        uint64_t peb = 0;
        if(!process->MemReadSafe(teb + kTebPebOffset, &peb, sizeof(peb)))
            return 0;
        return peb;
    }
}

void GleamDebugger::applyHides()
{
    if(!mProcess || !mThread)
    {
        printf("hide: no debuggee\n");
        fflush(stdout);
        return;
    }

    // 1) PEB flags.
    auto peb = pebAddress(mProcess, mThread->hThread);
    if(!peb)
    {
        printf("hide: failed to locate PEB\n");
        fflush(stdout);
        return;
    }
    uint8_t zero8 = 0;
    uint32_t zero32 = 0;
    mProcess->MemWriteSafe(peb + kPebBeingDebugged, &zero8, sizeof(zero8));
    mProcess->MemWriteSafe(peb + kPebNtGlobalFlag, &zero32, sizeof(zero32));
    printf("hide: PEB.BeingDebugged=0, PEB.NtGlobalFlag=0\n");

    // 2) Process heap debug bits.
    uint64_t heap = 0;
    if(mProcess->MemReadSafe(peb + kPebProcessHeap, &heap, sizeof(heap)) && heap)
    {
        uint32_t flags = 0, forceFlags = 0;
        if(mProcess->MemReadSafe(heap + kHeapFlags, &flags, sizeof(flags)))
        {
            flags &= ~kHeapDebugBits;
            mProcess->MemWriteSafe(heap + kHeapFlags, &flags, sizeof(flags));
        }
        if(mProcess->MemReadSafe(heap + kHeapForceFlags, &forceFlags, sizeof(forceFlags)))
        {
            forceFlags &= ~kHeapDebugBits;
            mProcess->MemWriteSafe(heap + kHeapForceFlags, &forceFlags, sizeof(forceFlags));
        }
        printf("hide: ProcessHeap Flags/ForceFlags cleaned\n");
    }

    // 3) Patch the user-mode detection APIs.
    // IsDebuggerPresent: mov eax, 0; ret
    static const uint8_t patchReturnFalse[] = { 0xB8, 0, 0, 0, 0, 0xC3 };
    // CheckRemoteDebuggerPresent(h, pbool): xor eax, eax; mov [rdx], eax; ret
    static const uint8_t patchRemoteFalse[] = { 0x33, 0xC0, 0x89, 0x02, 0xC3 };
    struct { const char* sym; const uint8_t* code; size_t size; const char* what; } apiPatches[] = {
        { "kernelbase!IsDebuggerPresent", patchReturnFalse, sizeof(patchReturnFalse), "IsDebuggerPresent" },
        { "kernel32!IsDebuggerPresent", patchReturnFalse, sizeof(patchReturnFalse), "IsDebuggerPresent" },
        { "kernelbase!CheckRemoteDebuggerPresent", patchRemoteFalse, sizeof(patchRemoteFalse), "CheckRemoteDebuggerPresent" },
        { "kernel32!CheckRemoteDebuggerPresent", patchRemoteFalse, sizeof(patchRemoteFalse), "CheckRemoteDebuggerPresent" },
    };
    int patchedApis = 0;
    for(const auto & p : apiPatches)
    {
        uint64_t addr = 0;
        if(!parseAddress(p.sym, addr) || !addr)
            continue;
        if(mProcess->MemWriteSafe(addr, p.code, p.size))
            patchedApis++;
    }
    printf("hide: patched %d API(s); NOTE: NtQueryInformationProcess checks are NOT hidden\n", patchedApis);
    fflush(stdout);
}

void GleamDebugger::cmdHide(bool on)
{
    mHideOn = on;
    printf("hide=%s\n", on ? "on" : "off");
    fflush(stdout);
    if(on)
        applyHides();
}
