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
    // Write bytes and record the originals for a later "hide off" restore.
    void recordAndWrite(GleeBug::Process* process, uint64_t addr, const void* data, size_t size,
                        std::vector<std::pair<uint64_t, std::vector<uint8_t>>> & originals)
    {
        std::vector<uint8_t> before(size);
        if(process->MemReadSafe(addr, before.data(), size) &&
           process->MemWriteSafe(addr, data, size))
            originals.emplace_back(addr, std::move(before));
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
    mHideOriginals.clear();

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
    recordAndWrite(mProcess, peb + kPebBeingDebugged, &zero8, sizeof(zero8), mHideOriginals);
    recordAndWrite(mProcess, peb + kPebNtGlobalFlag, &zero32, sizeof(zero32), mHideOriginals);
    printf("hide: PEB.BeingDebugged=0, PEB.NtGlobalFlag=0\n");

    // 2) Process heap debug bits.
    uint64_t heap = 0;
    if(mProcess->MemReadSafe(peb + kPebProcessHeap, &heap, sizeof(heap)) && heap)
    {
        uint32_t flags = 0, forceFlags = 0;
        if(mProcess->MemReadSafe(heap + kHeapFlags, &flags, sizeof(flags)))
        {
            uint32_t cleaned = flags & ~kHeapDebugBits;
            recordAndWrite(mProcess, heap + kHeapFlags, &cleaned, sizeof(cleaned), mHideOriginals);
        }
        if(mProcess->MemReadSafe(heap + kHeapForceFlags, &forceFlags, sizeof(forceFlags)))
        {
            uint32_t cleaned = forceFlags & ~kHeapDebugBits;
            recordAndWrite(mProcess, heap + kHeapForceFlags, &cleaned, sizeof(cleaned), mHideOriginals);
        }
        printf("hide: ProcessHeap Flags/ForceFlags cleaned\n");
    }

    // 3) Patch the user-mode detection APIs.
    // IsDebuggerPresent: mov eax, 0; ret
    static const uint8_t patchReturnFalse[] = { 0xB8, 0, 0, 0, 0, 0xC3 };
    // CheckRemoteDebuggerPresent(h, pbool): *pbool = FALSE, return TRUE:
    // xor eax, eax; mov [rdx], eax; mov eax, 1; ret
    static const uint8_t patchRemoteFalse[] = { 0x33, 0xC0, 0x89, 0x02, 0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3 };
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
        size_t before = mHideOriginals.size();
        recordAndWrite(mProcess, addr, p.code, p.size, mHideOriginals);
        if(mHideOriginals.size() > before)
            patchedApis++;
    }
    printf("hide: patched %d API(s); NOTE: NtQueryInformationProcess checks are NOT hidden\n", patchedApis);
    fflush(stdout);
}

void GleamDebugger::cmdHide(bool on)
{
    if(on)
    {
        // Re-applying while already hidden would lose the original bytes of
        // the first pass; keep the first set of originals instead.
        if(!mHideOriginals.empty())
        {
            mHideOn = true;
            printf("hide already applied (%zu modification(s) held)\n", mHideOriginals.size());
            fflush(stdout);
            return;
        }
        mHideOn = true;
        printf("hide=on\n");
        fflush(stdout);
        applyHides();
        return;
    }
    mHideOn = false;
    printf("hide=off\n");
    fflush(stdout);
    // Restore everything applyHides changed, in reverse order.
    for(auto it = mHideOriginals.rbegin(); it != mHideOriginals.rend(); ++it)
        mProcess->MemWriteSafe(it->first, it->second.data(), it->second.size());
    printf("hide: restored %zu modification(s)\n", mHideOriginals.size());
    mHideOriginals.clear();
    fflush(stdout);
}
