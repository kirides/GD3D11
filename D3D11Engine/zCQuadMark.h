#pragma once
#include "pch.h"
#include "HookedFunctions.h"
#include "zCPolygon.h"
#include "Engine.h"
#include "GothicAPI.h"
#include "zSTRING.h"

class zCQuadMark {
public:

    /** Hooks the functions of this Class */
    static void Hook() {
        DetourAttach( &reinterpret_cast<PVOID&>(HookedFunctions::OriginalFunctions.original_zCQuadMarkCreateQuadMark), Hooked_CreateQuadMark );
        //DetourAttach( &reinterpret_cast<PVOID&>(HookedFunctions::OriginalFunctions.original_zCQuadMarkConstructor), Hooked_Constructor );
        DetourAttach( &reinterpret_cast<PVOID&>(HookedFunctions::OriginalFunctions.original_zCQuadMarkDestructor), Hooked_Destructor );
    }

    static void __fastcall Hooked_CreateQuadMark( zCQuadMark* thisptr, void* unknwn, zCPolygon* poly, const float3& position, const float2& size, struct zTEffectParams* params ) {
        if ( thisptr->GetDontRepositionConnectedVob() )
            return; // Don't create quad-marks for particle-effects because it's kinda slow at the moment
        // And even for the original game using some emitters? (L'Hiver Light, Swampdragon)

        HookedFunctions::OriginalFunctions.original_zCQuadMarkCreateQuadMark( thisptr, poly, position, size, params );

        hook_infunc

            QuadMarkInfo* info = Engine::GAPI->GetQuadMarkInfo( thisptr );
            WorldConverter::UpdateQuadMarkInfo( info, thisptr, position );
            if ( !info->Mesh ) {
                Engine::GAPI->RemoveQuadMark( thisptr );
            }

        hook_outfunc
    }

    static void __fastcall Hooked_Constructor( void* thisptr, void* unknwn ) {
        HookedFunctions::OriginalFunctions.original_zCQuadMarkConstructor( thisptr );
    }

    static void __fastcall Hooked_Destructor( zCQuadMark* thisptr, void* unknwn ) {
        HookedFunctions::OriginalFunctions.original_zCQuadMarkDestructor( thisptr );

        hook_infunc

            Engine::GAPI->RemoveQuadMark( thisptr );

        hook_outfunc
    }

    zCMesh* GetQuadMesh() {
        return *reinterpret_cast<zCMesh**>(THISPTR_OFFSET( GothicMemoryLocations::zCQuadMark::Offset_QuadMesh ));
    }

    zCMaterial* GetMaterial() {
        return *reinterpret_cast<zCMaterial**>(THISPTR_OFFSET( GothicMemoryLocations::zCQuadMark::Offset_Material ));
    }

    zCVob* GetConnectedVob() {
        return *reinterpret_cast<zCVob**>(THISPTR_OFFSET( GothicMemoryLocations::zCQuadMark::Offset_ConnectedVob ));
    }

    /** This gets only set for quad-marks created by a particle-effect. */
    int GetDontRepositionConnectedVob() {
        return *reinterpret_cast<int*>(THISPTR_OFFSET( GothicMemoryLocations::zCQuadMark::Offset_DontRepositionConnectedVob ));
    }
};
