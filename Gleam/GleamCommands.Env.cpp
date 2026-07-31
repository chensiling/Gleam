/// @file GleamCommands.Env.cpp
/// @brief Process and thread environment inspection: peb, teb [tid], tls [tid].
///
/// @details
/// These answer "what does the target itself think its state is" - the data an
/// anti-debug check reads, where a thread's stack lives, what the last Win32
/// error was, and what sits in the TLS slots. Every field comes from the
/// debuggee's own memory, read with MemReadSafe, so a torn-down or partially
/// initialised process degrades to "unreadable" per field instead of failing
/// the whole command.
///
/// @section env_teb TEB base
/// The TEB base is NOT read via NtQueryInformationThread here: the engine
/// already recorded it from the CREATE_THREAD debug event
/// (Thread::lpThreadLocalBase), which is both cheaper and per-thread - exactly
/// what "teb <tid>" needs. This is the same source segmentBase() uses for
/// GSBASE, so "teb" and "regs" can never disagree.
///
/// @section env_offsets Offsets
/// Hand-coded x64 offsets, since the debuggee is not necessarily the same
/// bitness as any header we could include and these structures are only
/// partially documented. They are verified against observable truth in the
/// suite rather than trusted: PEB.ImageBaseAddress must equal the process base
/// the loader reported, and ProcessParameters.CommandLine must match the
/// command line the target was launched with.
///
/// @note x64 debuggee only (the project builds x64 exclusively). A WOW64
///   target has a second, 32-bit PEB/TEB pair that these offsets do not
///   describe; such a target is reported rather than silently misread.

#include "GleamDebugger.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace GleeBug;

namespace
{
    // ---- TEB (x64) ----
    const uint32_t kTebStackBase = 0x08; // NtTib.StackBase
    const uint32_t kTebStackLimit = 0x10; // NtTib.StackLimit
    const uint32_t kTebSelf = 0x30; // NtTib.Self - self-reference, sanity check
    const uint32_t kTebClientIdPid = 0x40;
    const uint32_t kTebClientIdTid = 0x48;
    const uint32_t kTebTlsPointer = 0x58; // ThreadLocalStoragePointer
    const uint32_t kTebPeb = 0x60;
    const uint32_t kTebLastError = 0x68; // LastErrorValue
    const uint32_t kTebTlsSlots = 0x1480; // TlsSlots[64], the fixed slot array
    const uint32_t kTebTlsExpansion = 0x1780; // TlsExpansionSlots
    const uint32_t kTlsSlotCount = 64; // TLS_MINIMUM_AVAILABLE

    // ---- PEB (x64) ----
    const uint32_t kPebBeingDebugged = 0x02;
    const uint32_t kPebImageBase = 0x10;
    const uint32_t kPebLdr = 0x18;
    const uint32_t kPebProcessParameters = 0x20;
    const uint32_t kPebProcessHeap = 0x30;
    const uint32_t kPebNtGlobalFlag = 0xBC;

    // ---- RTL_USER_PROCESS_PARAMETERS (x64) ----
    // Layout: 4 DWORDs, ConsoleHandle, ConsoleFlags, 3 std handles, CURDIR
    // (UNICODE_STRING + handle), then DllPath / ImagePathName / CommandLine as
    // UNICODE_STRINGs, then Environment.
    const uint32_t kParamsImagePath = 0x60;
    const uint32_t kParamsCommandLine = 0x70;
    const uint32_t kParamsEnvironment = 0x80;

    // A UNICODE_STRING is {USHORT Length; USHORT MaximumLength; PWSTR Buffer;}
    // with Buffer at +8 on x64. Length counts BYTES, not characters.
    const uint32_t kUniStrLength = 0x00;
    const uint32_t kUniStrBuffer = 0x08;

    // Cap remote string reads: Length is attacker-controlled data in a hostile
    // target, so a corrupt 64KB "string" must not become a 64KB print.
    const uint32_t kMaxStringBytes = 8192;

    /// Read a UNICODE_STRING at @p addr and convert to UTF-8.
    /// Returns false when the descriptor or its buffer is unreadable.
    bool readUnicodeString(Process* process, uint64_t addr, std::string & out)
    {
        uint16_t lengthBytes = 0;
        uint64_t buffer = 0;
        if(!process->MemReadSafe(addr + kUniStrLength, &lengthBytes, sizeof(lengthBytes)))
            return false;
        if(!process->MemReadSafe(addr + kUniStrBuffer, &buffer, sizeof(buffer)))
            return false;
        if(!buffer)
        {
            out.clear();
            return true; // legitimately empty, not an error
        }
        uint32_t capped = lengthBytes;
        bool truncated = false;
        if(capped > kMaxStringBytes)
        {
            capped = kMaxStringBytes;
            truncated = true;
        }
        std::vector<wchar_t> wide(capped / sizeof(wchar_t) + 1, 0);
        if(capped && !process->MemReadSafe(buffer, wide.data(), capped))
            return false;
        int need = WideCharToMultiByte(CP_UTF8, 0, wide.data(), -1, nullptr, 0, nullptr, nullptr);
        if(need <= 0)
            return false;
        std::vector<char> utf8(need);
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), -1, utf8.data(), need, nullptr, nullptr);
        out.assign(utf8.data());
        if(truncated)
            out += " ...[truncated]";
        return true;
    }

    /// Print one pointer-sized field, or note that it could not be read.
    /// Separating "0" from "unreadable" matters: a real null ImageBaseAddress
    /// and a failed read mean very different things when chasing a bug.
    void printPtrField(Process* process, const char* label, uint64_t addr)
    {
        uint64_t value = 0;
        if(process->MemReadSafe(addr, &value, sizeof(value)))
            printf("  %-20s %016llX\n", label, (unsigned long long)value);
        else
            printf("  %-20s <unreadable at 0x%llX>\n", label, (unsigned long long)addr);
    }
}

// ---------------------------------------------------------------------------

void GleamDebugger::cmdPeb(bool showEnvValues)
{
    Thread* thread = currentThread();
    if(!thread)
    {
        printf("no current thread\n");
        fflush(stdout);
        return;
    }
    uint64_t teb = (uint64_t)thread->lpThreadLocalBase;
    uint64_t peb = 0;
    if(!teb || !mProcess->MemReadSafe(teb + kTebPeb, &peb, sizeof(peb)) || !peb)
    {
        printf("cannot locate PEB (TEB=0x%llX)\n", (unsigned long long)teb);
        fflush(stdout);
        return;
    }

    printf("PEB      %016llX\n", (unsigned long long)peb);

    // Anti-debug relevant flags first: this is what a checking target reads.
    uint8_t beingDebugged = 0;
    if(mProcess->MemReadSafe(peb + kPebBeingDebugged, &beingDebugged, sizeof(beingDebugged)))
        printf("  %-20s %u\n", "BeingDebugged", beingDebugged);
    else
        printf("  %-20s <unreadable>\n", "BeingDebugged");
    uint32_t ntGlobalFlag = 0;
    if(mProcess->MemReadSafe(peb + kPebNtGlobalFlag, &ntGlobalFlag, sizeof(ntGlobalFlag)))
        printf("  %-20s 0x%08X\n", "NtGlobalFlag", ntGlobalFlag);
    else
        printf("  %-20s <unreadable>\n", "NtGlobalFlag");

    printPtrField(mProcess, "ImageBaseAddress", peb + kPebImageBase);
    printPtrField(mProcess, "Ldr", peb + kPebLdr);
    printPtrField(mProcess, "ProcessHeap", peb + kPebProcessHeap);
    printPtrField(mProcess, "ProcessParameters", peb + kPebProcessParameters);

    // Command line and image path live behind ProcessParameters. A failure here
    // must not lose the flags already printed above, so it is reported and the
    // command still succeeds.
    uint64_t params = 0;
    if(!mProcess->MemReadSafe(peb + kPebProcessParameters, &params, sizeof(params)) || !params)
    {
        printf("  (ProcessParameters unreadable: no command line or environment)\n");
        fflush(stdout);
        return;
    }

    std::string text;
    if(readUnicodeString(mProcess, params + kParamsImagePath, text))
        printf("  %-20s %s\n", "ImagePathName", text.c_str());
    else
        printf("  %-20s <unreadable>\n", "ImagePathName");
    if(readUnicodeString(mProcess, params + kParamsCommandLine, text))
        printf("  %-20s %s\n", "CommandLine", text.c_str());
    else
        printf("  %-20s <unreadable>\n", "CommandLine");

    // The environment block is a run of NUL-terminated "K=V" strings ending in
    // a double NUL. There is no length field, so read in chunks and stop at the
    // terminator or the cap - a corrupt block must not spin forever.
    uint64_t env = 0;
    if(!mProcess->MemReadSafe(params + kParamsEnvironment, &env, sizeof(env)) || !env)
    {
        printf("  %-20s <none>\n", "Environment");
        fflush(stdout);
        return;
    }
    printf("  %-20s %016llX\n", "Environment", (unsigned long long)env);
    std::vector<wchar_t> block;
    const uint32_t kChunkChars = 2048;
    const uint32_t kMaxEnvChars = 64 * 1024;
    bool terminated = false;
    while(block.size() < kMaxEnvChars && !terminated)
    {
        size_t before = block.size();
        block.resize(before + kChunkChars, 0);
        if(!mProcess->MemReadSafe(env + before * sizeof(wchar_t),
                                  block.data() + before, kChunkChars * sizeof(wchar_t)))
        {
            block.resize(before);
            break;
        }
        for(size_t i = (before ? before - 1 : 0); i + 1 < block.size(); i++)
        {
            if(block[i] == 0 && block[i + 1] == 0)
            {
                block.resize(i + 1);
                terminated = true;
                break;
            }
        }
    }
    // NAMES ONLY unless the caller explicitly asked for values.
    //
    // The environment routinely holds API tokens, passwords and session keys.
    // This command is driven by an LLM over MCP, so its output lands in
    // transcripts, logs and test artifacts; a default that dumps values would
    // leak every secret in the target's environment on a command whose usual
    // question is merely "does the target see variable X". Values are available
    // via "peb env", where the opt-in is the safety boundary.
    size_t count = 0;
    for(size_t i = 0; i < block.size(); )
    {
        if(!block[i])
            break;
        int need = WideCharToMultiByte(CP_UTF8, 0, &block[i], -1, nullptr, 0, nullptr, nullptr);
        if(need > 0)
        {
            std::vector<char> utf8(need);
            WideCharToMultiByte(CP_UTF8, 0, &block[i], -1, utf8.data(), need, nullptr, nullptr);
            if(showEnvValues)
                printf("    %s\n", utf8.data());
            else
            {
                // Split at the first '=': everything after it is the value.
                // A leading '=' is legal in the block (drive-relative cwd
                // entries like "=C:=C:\dir"), so scan from index 1.
                char* text = utf8.data();
                char* eq = strchr(text[0] ? text + 1 : text, '=');
                if(eq)
                    printf("    %.*s\n", (int)(eq - text), text);
                else
                    printf("    %s\n", text);
            }
        }
        count++;
        i += wcslen(&block[i]) + 1;
    }
    printf("  %zu environment variable(s)%s%s\n", count,
           terminated ? "" : " (block not terminated within cap)",
           showEnvValues ? "" : " - names only, use 'peb env' for values");
    fflush(stdout);
}

// ---------------------------------------------------------------------------

void GleamDebugger::cmdTeb(uint32_t tid)
{
    // tid 0 means "the current thread", so "teb" and "teb <tid>" share a path.
    Thread* thread = nullptr;
    if(tid == 0)
    {
        thread = currentThread();
        if(!thread)
        {
            printf("no current thread\n");
            fflush(stdout);
            return;
        }
    }
    else
    {
        auto found = mProcess->threads.find(tid);
        if(found == mProcess->threads.end())
        {
            printf("no such thread: %u (try 'threads')\n", tid);
            fflush(stdout);
            return;
        }
        thread = found->second.get();
    }

    uint64_t teb = (uint64_t)thread->lpThreadLocalBase;
    if(!teb)
    {
        printf("thread %u has no recorded TEB base\n", thread->dwThreadId);
        fflush(stdout);
        return;
    }
    printf("TEB      %016llX  (thread %u)\n", (unsigned long long)teb, thread->dwThreadId);

    // Self-reference check: NtTib.Self must point back at the TEB. A mismatch
    // means the offsets do not describe this target (a WOW64 process, say), so
    // say that instead of printing plausible-looking nonsense.
    uint64_t self = 0;
    if(mProcess->MemReadSafe(teb + kTebSelf, &self, sizeof(self)) && self != teb)
    {
        printf("  WARNING: NtTib.Self=%016llX != TEB - x64 offsets may not apply\n"
               "           (a WOW64 target has a separate 32-bit TEB)\n",
               (unsigned long long)self);
    }

    printPtrField(mProcess, "StackBase", teb + kTebStackBase);
    printPtrField(mProcess, "StackLimit", teb + kTebStackLimit);

    uint64_t pid64 = 0, tid64 = 0;
    if(mProcess->MemReadSafe(teb + kTebClientIdPid, &pid64, sizeof(pid64)) &&
       mProcess->MemReadSafe(teb + kTebClientIdTid, &tid64, sizeof(tid64)))
        printf("  %-20s pid=%llu tid=%llu\n", "ClientId",
               (unsigned long long)pid64, (unsigned long long)tid64);
    else
        printf("  %-20s <unreadable>\n", "ClientId");

    printPtrField(mProcess, "PEB", teb + kTebPeb);

    uint32_t lastError = 0;
    if(mProcess->MemReadSafe(teb + kTebLastError, &lastError, sizeof(lastError)))
        printf("  %-20s %u (0x%08X)\n", "LastErrorValue", lastError, lastError);
    else
        printf("  %-20s <unreadable>\n", "LastErrorValue");

    printPtrField(mProcess, "TlsPointer", teb + kTebTlsPointer);
    fflush(stdout);
}

// ---------------------------------------------------------------------------

void GleamDebugger::cmdTls(uint32_t tid)
{
    Thread* thread = nullptr;
    if(tid == 0)
    {
        thread = currentThread();
        if(!thread)
        {
            printf("no current thread\n");
            fflush(stdout);
            return;
        }
    }
    else
    {
        auto found = mProcess->threads.find(tid);
        if(found == mProcess->threads.end())
        {
            printf("no such thread: %u (try 'threads')\n", tid);
            fflush(stdout);
            return;
        }
        thread = found->second.get();
    }

    uint64_t teb = (uint64_t)thread->lpThreadLocalBase;
    if(!teb)
    {
        printf("thread %u has no recorded TEB base\n", thread->dwThreadId);
        fflush(stdout);
        return;
    }

    // Two distinct things share the "TLS" name and both matter:
    //   TlsSlots      - the TEB's fixed 64-slot array (TlsAlloc/TlsGetValue)
    //   TlsPointer    - the __declspec(thread) / _tls_index implicit TLS array,
    //                   allocated per module by the loader
    // Print both, and only the non-zero fixed slots: 64 lines of zeros buries
    // the two or three slots that are actually in use.
    printf("TLS      thread %u  TEB=%016llX\n",
           thread->dwThreadId, (unsigned long long)teb);

    uint64_t tlsPointer = 0;
    if(mProcess->MemReadSafe(teb + kTebTlsPointer, &tlsPointer, sizeof(tlsPointer)))
    {
        printf("  ThreadLocalStoragePointer %016llX\n", (unsigned long long)tlsPointer);
        if(tlsPointer)
        {
            // Implicit TLS: one pointer per module with a .tls section. The
            // count is not stored per-thread, so show the first few entries
            // and let "read" go deeper.
            for(uint32_t i = 0; i < 4; i++)
            {
                uint64_t entry = 0;
                if(!mProcess->MemReadSafe(tlsPointer + i * sizeof(uint64_t),
                                          &entry, sizeof(entry)))
                    break;
                if(!entry)
                    continue;
                printf("    [module %u] %016llX\n", i, (unsigned long long)entry);
            }
        }
    }
    else
    {
        printf("  ThreadLocalStoragePointer <unreadable>\n");
    }

    uint32_t shown = 0;
    for(uint32_t i = 0; i < kTlsSlotCount; i++)
    {
        uint64_t slot = 0;
        if(!mProcess->MemReadSafe(teb + kTebTlsSlots + i * sizeof(uint64_t),
                                  &slot, sizeof(slot)))
        {
            printf("  TlsSlots[%u] <unreadable>\n", i);
            break;
        }
        if(!slot)
            continue;
        printf("  TlsSlots[%-2u] %016llX\n", i, (unsigned long long)slot);
        shown++;
    }
    printf("  %u of %u fixed slot(s) non-zero\n", shown, kTlsSlotCount);

    uint64_t expansion = 0;
    if(mProcess->MemReadSafe(teb + kTebTlsExpansion, &expansion, sizeof(expansion)) && expansion)
        printf("  TlsExpansionSlots %016llX (slots 64..1087)\n",
               (unsigned long long)expansion);
    fflush(stdout);
}
