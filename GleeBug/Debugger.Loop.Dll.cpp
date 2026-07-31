#include "Debugger.h"

namespace GleeBug
{
    void Debugger::loadDllEvent(const LOAD_DLL_DEBUG_INFO & loadDll)
    {
        // Query DEP policy here: ntdll has already called ZwSetInformationProcess(0x22)
        // by the time the first DLL-load event fires, so this is the first reliable
        // opportunity.  (createProcessEvent fires before ntdll runs; that reading is
        // provisional and will be overwritten here with the definitive value.)
        queryDep();

        //call the debug event callback
        cbLoadDllEvent(loadDll);

        //close the file handle
        if(loadDll.hFile)
            CloseHandle(loadDll.hFile);
    }

    void Debugger::unloadDllEvent(const UNLOAD_DLL_DEBUG_INFO & unloadDll)
    {
        //call the debug event callback
        cbUnloadDllEvent(unloadDll);
    }
};