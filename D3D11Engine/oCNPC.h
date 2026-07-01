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

class oCNPC;
struct oCNpc_States {
#ifdef BUILD_GOTHIC_1_CLASSIC
    bool IsInState(int state) const {
        return 0 != reinterpret_cast<int( __thiscall* )( const oCNpc_States*, int )>( GothicMemoryLocations::oCNpc_States::IsInState )( this, state );
    }
#endif
};

class oCNPC : public zCVob {
public:
    static const zCClassDef* GetStaticClassDef() {
        return reinterpret_cast<const zCClassDef*>(GothicMemoryLocations::zCClassDef::oCNpc);
    }

    /** Hooks the functions of this Class */
    static void Hook() {
        HookedFunctions::OriginalFunctions.original_oCNPCEnable.Detour( hooked_oCNPCEnable  );
        HookedFunctions::OriginalFunctions.original_oCNPCDisable.Detour( hooked_oCNPCDisable  );
        HookedFunctions::OriginalFunctions.original_oCNPCInitModel.Detour( hooked_oCNPCInitModel  );
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

    zCVob* GetFocusVob() const {
#if defined(BUILD_GOTHIC_2_6_fix) || defined(BUILD_GOTHIC_1_CLASSIC)
        return *reinterpret_cast<zCVob**>(THISPTR_OFFSET( GothicMemoryLocations::oCNPC::Offset_focus_vob ));
#else
        return nullptr;
#endif
    }

    void ResetPos( const XMFLOAT3& pos ) {
        reinterpret_cast<void( __fastcall* )( oCNPC*, int, const XMFLOAT3& )>( GothicMemoryLocations::oCNPC::ResetPos )( this, 0, pos );
    }

    int IsAPlayer() {
        return (this == *reinterpret_cast<oCNPC**>(GothicMemoryLocations::oCGame::Var_Player));
    }
    zSTRING GetName( int i = 0 ) {
        zSTRING str;
        reinterpret_cast<void( __fastcall* )( oCNPC*, int, zSTRING&, int )>( GothicMemoryLocations::oCNPC::GetName )( this, 0, str, i );
        return str;
    }
    
#ifdef BUILD_GOTHIC_1_CLASSIC
    oCNpc_States* GetStates() {
        return reinterpret_cast<oCNpc_States*>(THISPTR_OFFSET( GothicMemoryLocations::oCNPC::Offset_states ));
    }
#endif
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

