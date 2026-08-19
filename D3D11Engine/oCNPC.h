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

struct TNpcSlot {
    zSTRING name;
    int inInventory;
    int tmpLevel;
    zSTRING itemName;
    zCVob* vob;
    int _rest;
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
        DetourAttachTyped( &HookedFunctions::OriginalFunctions.original_oCNPCEnable, hooked_oCNPCEnable  );
        DetourAttachTyped( &HookedFunctions::OriginalFunctions.original_oCNPCDisable, hooked_oCNPCDisable  );
        DetourAttachTyped( &HookedFunctions::OriginalFunctions.original_oCNPCInitModel, hooked_oCNPCInitModel  );
    }

    static void __fastcall hooked_oCNPCInitModel( zCVob* thisptr, void* unknwn ) {
        HookedFunctions::OriginalFunctions.original_oCNPCInitModel( thisptr );

        hook_infunc

            // oCNpc::InitModel only rewrites the zCModel's mesh library in place (RemoveMeshLibAll +
            // ApplyMeshLib on armor equip/unequip) - the vob's visual identity doesn't change, so there's
            // no need to tear the vob out of the BSP/light/instancing state via Remove+Add. Re-run the
            // extraction and re-assign it explicitly (mirroring OnAddVob) rather than relying on
            // LoadzCModelData reusing the same cached SkeletalMeshVisualInfo - OnVisualDeleted can null
            // out and drop that cache entry (a genuine SetVisual model swap), and if that raced ahead of
            // this call, re-pointing VisualInfo is what keeps the already-registered vob from going stuck
            // on a null/stale visual instead of picking up the freshly re-created one.
            if ( SkeletalVobInfo* svi = Engine::GAPI->GetSkeletalVobByVob( thisptr ) ) {
                // Forces head/held-item attachments to re-extract against the settled model instead of
                // trusting the per-frame pointer-identity check, which fast repeated visual swaps can fool.
                WorldConverter::ReleaseAllNodeAttachments( svi->NodeAttachments );

                svi->VisualInfo = Engine::GAPI->LoadzCModelData( static_cast<oCNPC*>(thisptr) );
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
    
    TNpcSlot* GetInvSlot(const zSTRING& name) {
        return reinterpret_cast<TNpcSlot*( __fastcall* )( oCNPC*, int, const zSTRING& )>( GothicMemoryLocations::oCNPC::GetInvSlot_zString )( this, 0, name );
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

