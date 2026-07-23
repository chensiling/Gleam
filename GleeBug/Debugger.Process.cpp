#include "Debugger.Process.h"
#include "Debugger.Thread.Registers.h"
#include "Zydis/Zydis.h"

#include <cstring>

namespace GleeBug
{
    Process::Process(HANDLE hProcess, uint32 dwProcessId, uint32 dwMainThreadId, const CREATE_PROCESS_DEBUG_INFO & createProcessInfo) :
        hProcess(hProcess),
        dwProcessId(dwProcessId),
        dwMainThreadId(dwMainThreadId),
        createProcessInfo(createProcessInfo),
        thread(nullptr),
        systemBreakpoint(false),
        permanentDep(false)
    {
        for(int i = 0; i < HWBP_COUNT; i++)
            hardwareBreakpoints[i].internal.hardware.enabled = false;
    }

    // NOTE: the vendored Zydis header enum is out of sync with the decoder
    // tables baked into Zydis.c (e.g. ZYDIS_MNEMONIC_CALL is 67 in the header
    // but the decoder returns 71 for call, and ZydisMnemonicGetString(71) is
    // "call"). Never compare info.mnemonic against the enum constants; go
    // through the string table, which is consistent with the decoder.
    static bool MnemonicIs(const ZydisDecodedInstruction & info, const char* name)
    {
        auto str = ZydisMnemonicGetString(info.mnemonic);
        return str != nullptr && strcmp(str, name) == 0;
    }

    static bool IsRepeated(const ZydisDecodedInstruction & info)
    {
        // https://www.felixcloutier.com/x86/rep:repe:repz:repne:repnz
        // TODO: allow extracting the affected range
        if((info.attributes & (ZYDIS_ATTRIB_HAS_REP | ZYDIS_ATTRIB_HAS_REPZ | ZYDIS_ATTRIB_HAS_REPNZ)) == 0)
            return false;
        // REP-prefixed string operations: ins/outs/movs/lods/stos/cmps/scas
        auto str = ZydisMnemonicGetString(info.mnemonic);
        if(str == nullptr)
            return false;
        return strncmp(str, "ins", 3) == 0 ||
               strncmp(str, "outs", 4) == 0 ||
               strncmp(str, "movs", 4) == 0 ||
               strncmp(str, "lods", 4) == 0 ||
               strncmp(str, "stos", 4) == 0 ||
               strncmp(str, "cmps", 4) == 0 ||
               strncmp(str, "scas", 4) == 0;
    }

    void Process::StepOver(const StepCallback & cbStep)
    {
        auto gip = Registers(thread->hThread, CONTEXT_CONTROL).Gip();
        unsigned char data[16];
        if(MemReadSafe(gip, data, sizeof(data)))
        {
            ZydisDisassembledInstruction instruction;
            if(ZYAN_SUCCESS(ZydisDisassembleIntel(
                                GleeArchValue(ZYDIS_MACHINE_MODE_LONG_COMPAT_32, ZYDIS_MACHINE_MODE_LONG_64),
                                gip,
                                data,
                                sizeof(data),
                                &instruction
                            )))
            {
                const bool stepOver =
                    MnemonicIs(instruction.info, "call") ||
                    MnemonicIs(instruction.info, "pushf") ||
                    IsRepeated(instruction.info);
                if(stepOver)
                {
                    SetBreakpoint(gip + instruction.info.length, [cbStep](const BreakpointInfo & info)
                    {
                        cbStep();
                    }, true, SoftwareType::ShortInt3);
                    return;
                }
            }
        }
        thread->StepInto(cbStep);
    }

    void Process::StepInternal(const StepCallback & cbStep)
    {
        Registers registers(thread->hThread, CONTEXT_CONTROL);
        registers.TrapFlag.Set();
        thread->isInternalStepping = true;

        // Check if we're currently stepping on a pushf instruction
        auto isPushf = false;
        {
            auto gip = registers.Gip();
            unsigned char data[16];
            if(MemReadSafe(gip, data, sizeof(data)))
            {
                ZydisDisassembledInstruction instruction;
                if(ZYAN_SUCCESS(ZydisDisassembleIntel(
                                    GleeArchValue(ZYDIS_MACHINE_MODE_LONG_COMPAT_32, ZYDIS_MACHINE_MODE_LONG_64),
                                    gip,
                                    data,
                                    sizeof(data),
                                    &instruction
                                )))
                {
                    isPushf = MnemonicIs(instruction.info, "pushf");
                }
            }
        }

        if(isPushf)
        {
            thread->cbInternalStep = [this, cbStep]()
            {
                // Remove the trap flag from the stack
                auto gsp = Registers(this->thread->hThread).Gsp();
                GleeBug::ptr data;
                if(MemReadUnsafe(gsp, &data, sizeof(data)))
                {
                    data &= ~(int)Registers::F::Trap;
                    MemWriteUnsafe(gsp, &data, sizeof(data));
                }

                cbStep();
            };
        }
        else
        {
            thread->cbInternalStep = cbStep;
        }
    }
};