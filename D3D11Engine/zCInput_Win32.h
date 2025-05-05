#pragma once
#include "pch.h"
#include "HookedFunctions.h"
#include "ImGuiShim.h"
#include "Engine.h"
#include "GothicAPI.h"

class zCInput_Win32 {
public:
    /** Hooks the functions of this Class */
    static void Hook() {
#if (defined( BUILD_GOTHIC_2_6_fix) || (defined(BUILD_GOTHIC_1_08k) && !defined(BUILD_1_12F)))
        // TODO: Also implement for other gothics
        DetourAttach( &reinterpret_cast<PVOID&>(HookedFunctions::OriginalFunctions.original_zCInput_Win32__GetKey), hooked_GetKey );
#endif
    }

    static uint16_t __fastcall hooked_GetKey( zCInput_Win32* thisptr, void* pUnkn, int repeat, int delayed ) {
        if ( Engine::ImGuiHandle && Engine::ImGuiHandle->IsActive ) {
            // no input to gothic while settings is open
            return 0;
        }
        
        auto ret = HookedFunctions::OriginalFunctions.original_zCInput_Win32__GetKey( thisptr, repeat, delayed );
        return ret;
    }
};
