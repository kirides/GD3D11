#pragma once
#include "pch.h"
#include "HookedFunctions.h"
#include "zCPolygon.h"
#include "Engine.h"
#include "GothicAPI.h"
#include "zCVob.h"
#include "zViewTypes.h"

enum oCNPCFlags : int
{
    NPC_FLAG_FRIEND = (1 << 0),
    NPC_FLAG_IMMORTAL = (1 << 1),
    NPC_FLAG_GHOST = (1 << 2)
};

class oCNPC : public zCVob {
public:
    static const zCClassDef* GetStaticClassDef() {
        return reinterpret_cast<const zCClassDef*>(GothicMemoryLocations::zCClassDef::oCNpc);
    }

    /** Hooks the functions of this Class */
    static void Hook() {
        DetourAttach( &reinterpret_cast<PVOID&>(HookedFunctions::OriginalFunctions.original_oCNPCEnable), hooked_oCNPCEnable );
        DetourAttach( &reinterpret_cast<PVOID&>(HookedFunctions::OriginalFunctions.original_oCNPCDisable), hooked_oCNPCDisable );
        DetourAttach( &reinterpret_cast<PVOID&>(HookedFunctions::OriginalFunctions.original_oCNPCInitModel), hooked_oCNPCInitModel );
    }

    static void __fastcall hooked_oCNPCInitModel( zCVob* thisptr, void* unknwn ) {
        HookedFunctions::OriginalFunctions.original_oCNPCInitModel( thisptr );

        hook_infunc

            if ( /*((zCVob *)thisptr)->GetVisual() || */Engine::GAPI->GetSkeletalVobByVob( thisptr ) ) {
                // This may causes the vob to be added and removed multiple times, but makes sure we get all changes of armor
                Engine::GAPI->OnRemovedVob( thisptr, thisptr->GetHomeWorld() );
                Engine::GAPI->OnAddVob( thisptr, thisptr->GetHomeWorld() );
            }

        hook_outfunc
    }

    /** Reads config stuff */
    static void __fastcall hooked_oCNPCEnable( zCVob* thisptr, void* unknwn, XMFLOAT3& position ) {
        HookedFunctions::OriginalFunctions.original_oCNPCEnable( thisptr, position );

        hook_infunc

            // Re-Add if needed
            Engine::GAPI->OnRemovedVob( thisptr, thisptr->GetHomeWorld() );
            Engine::GAPI->OnAddVob( thisptr, thisptr->GetHomeWorld() );

        hook_outfunc
    }

    static void __fastcall hooked_oCNPCDisable( oCNPC* thisptr, void* unknwn ) {
        hook_infunc

            // Remove vob from world
            if ( !thisptr->IsAPlayer() ) // Never disable the player vob
                Engine::GAPI->OnRemovedVob( thisptr, thisptr->GetHomeWorld() );

        hook_outfunc

        HookedFunctions::OriginalFunctions.original_oCNPCDisable( thisptr );
    }

    void ResetPos( const XMFLOAT3& pos ) {
        reinterpret_cast<void( __fastcall* )( oCNPC*, int, const XMFLOAT3& )>( GothicMemoryLocations::oCNPC::ResetPos )( this, 0, pos );
    }

    int IsAPlayer() {
        return (this == oCGame::GetPlayer());
    }
    zSTRING GetName( int i = 0 ) {
        zSTRING str;
        reinterpret_cast<void( __fastcall* )( oCNPC*, int, zSTRING&, int )>( GothicMemoryLocations::oCNPC::GetName )( this, 0, str, i );
        return str;
    }
#ifndef BUILD_SPACER
    bool HasFlag( oCNPCFlags flag ) {
        return reinterpret_cast<bool( __fastcall* )( oCNPC*, int, oCNPCFlags )>( GothicMemoryLocations::oCNPC::HasFlag )( this, 0, flag );
    }
#else
    bool HasFlag( oCNPCFlags ) {
        return false;
    }
#endif
};

