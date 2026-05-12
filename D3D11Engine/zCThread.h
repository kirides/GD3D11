#pragma once
#include "pch.h"
#include "HookedFunctions.h"
#include "Engine.h"
#include "GothicAPI.h"
#include "BaseGraphicsEngine.h"
#include "zCResourceManager.h"

class zCThread {
public:

    /** Hooks the functions of this Class */
    static void Hook() {
        DetourAttachTyped( &HookedFunctions::OriginalFunctions.original_zCThreadSuspendThread, hooked_SuspendThread  );
    }

    /** Reads config stuff */
    static int __fastcall hooked_SuspendThread( zCThread* t, void* unknwn ) {
        int* suspCount = t->GetSuspendCounter();

        if ( (*suspCount) > 0 )
            return 0;

        (*suspCount) += 1;

        while ( (*suspCount) )
            Sleep( 100 ); // Sleep as long as we are suspended

        return 1;
    }

    int* GetSuspendCounter() {
        return reinterpret_cast<int*>(THISPTR_OFFSET( GothicMemoryLocations::zCThread::Offset_SuspendCount ));
    }
};
