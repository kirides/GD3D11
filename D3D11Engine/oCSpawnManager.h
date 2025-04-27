#pragma once
#include "pch.h"
#include "HookedFunctions.h"
#include "Engine.h"
#include "GothicAPI.h"
#include "BaseGraphicsEngine.h"
#include "oCGame.h"
#include "zCVob.h"
#include "oCNPC.h"

class oCSpawnManager {
public:

    /** Hooks the functions of this Class */
    static void Hook() {
        DetourAttach( &reinterpret_cast<PVOID&>(HookedFunctions::OriginalFunctions.original_oCSpawnManagerSpawnNpc), hooked_oCSpawnManagerSpawnNpc );

        //DetourAttach( &reinterpret_cast<PVOID&>(HookedFunctions::OriginalFunctions.original_oCSpawnManagerCheckInsertNpc), hooked_oCSpawnManagerCheckInsertNpc );
        //DetourAttach( &reinterpret_cast<PVOID&>(HookedFunctions::OriginalFunctions.original_oCSpawnManagerCheckRemoveNpc), hooked_oCSpawnManagerCheckRemoveNpc );
    }

    /** Reads config stuff */
    static void __fastcall hooked_oCSpawnManagerSpawnNpc( zCVob* thisptr, void* unknwn, oCNPC* npc, const XMFLOAT3& position, float f ) {
        HookedFunctions::OriginalFunctions.original_oCSpawnManagerSpawnNpc( thisptr, npc, position, f );

        hook_infunc

            if ( npc->GetSleepingMode() != 0 || npc->IsAPlayer() ) {
                Engine::GAPI->OnRemovedVob( npc, npc->GetHomeWorld() );
                Engine::GAPI->OnAddVob( npc, npc->GetHomeWorld() );
            }

        hook_outfunc
    }

    static int __fastcall hooked_oCSpawnManagerCheckRemoveNpc( void* thisptr, void* unknwn, oCNPC* npc ) {
        Engine::GAPI->SetCanClearVobsByVisual();
        auto res = HookedFunctions::OriginalFunctions.original_oCSpawnManagerCheckRemoveNpc( thisptr, npc );
        Engine::GAPI->SetCanClearVobsByVisual( false );
        return res;
    }

    /** Reads config stuff */
    static void __fastcall hooked_oCSpawnManagerCheckInsertNpc( void* thisptr, void* unknwn ) {
        HookedFunctions::OriginalFunctions.original_oCSpawnManagerCheckInsertNpc( thisptr );
    }
};



