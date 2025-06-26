#pragma once
#include "Engine.h"
#include "GothicAPI.h"
#include "HookedFunctions.h"
#include "zCPolyStrip.h"

class zCFlash {
public:
    /** Hooks the functions of this Class */
    static void Hook() {
        DetourAttach( &reinterpret_cast<PVOID&>(HookedFunctions::OriginalFunctions.original_zCFlashSetVisualUsedBy), Hooked_SetVisualUsedBy );
        DetourAttach( &reinterpret_cast<PVOID&>(HookedFunctions::OriginalFunctions.original_zCFlashDestructor), Hooked_Destructor );
    }

    // We use SetVisualUsedBy instead of Create here because we need connect visual to vob to avoid leaks
    static void __fastcall Hooked_SetVisualUsedBy( zCFlash* thisptr, void* unknwn, zCVob* vob ) {
        HookedFunctions::OriginalFunctions.original_zCFlashSetVisualUsedBy( thisptr, vob );

        hook_infunc

            Engine::GAPI->AddFlash( thisptr, vob );

        hook_outfunc
    }

    static void __fastcall Hooked_Destructor( zCFlash* thisptr, void* unknwn ) {
        HookedFunctions::OriginalFunctions.original_zCFlashDestructor( thisptr );

        hook_infunc

            Engine::GAPI->RemoveFlash( thisptr );

        hook_outfunc
    }

    void RenderBolt( DWORD pBolt, std::vector<zCPolyStrip*>& polyStrips ) {
        polyStrips.emplace_back( reinterpret_cast<zCPolyStrip*>(pBolt) );

        DWORD* children = *reinterpret_cast<DWORD**>(pBolt + GothicMemoryLocations::zCFlash::Offset_ChildrenTable);
        int childrenSize = *reinterpret_cast<int*>(pBolt + GothicMemoryLocations::zCFlash::Offset_ChildrenSize);
        for ( int i = 0; i < childrenSize; ++i ) {
            RenderBolt( children[i], polyStrips );
        }
    }

    bool RenderFlash( std::vector<zCPolyStrip*>& polyStrips ) {
        float lifeTime = *reinterpret_cast<float*>( THISPTR_OFFSET( GothicMemoryLocations::zCFlash::Offset_LifeTime ) );
        if ( lifeTime > 1.0f )
            return true;

        reinterpret_cast<void( __fastcall* )(zCFlash*, int, float)>( GothicMemoryLocations::zCFlash::Update )(this, 0, 70.f);
        RenderBolt( *reinterpret_cast<DWORD*>( THISPTR_OFFSET( GothicMemoryLocations::zCFlash::Offset_BoltB ) ), polyStrips );
        RenderBolt( *reinterpret_cast<DWORD*>( THISPTR_OFFSET( GothicMemoryLocations::zCFlash::Offset_BoltA ) ), polyStrips );
        return false;
    }

    /** Returns the world-position of this visual */
    FXMVECTOR XM_CALLCONV GetStartPositionWorld() const {
        return XMVectorSet( *reinterpret_cast<float*>(THISPTR_OFFSET( GothicMemoryLocations::zCFlash::Offset_StartPosX )),
            *reinterpret_cast<float*>(THISPTR_OFFSET( GothicMemoryLocations::zCFlash::Offset_StartPosY )),
            *reinterpret_cast<float*>(THISPTR_OFFSET( GothicMemoryLocations::zCFlash::Offset_StartPosZ )),
            0.0f );
    }

    FXMVECTOR XM_CALLCONV GetEndPositionWorld() const {
        return XMVectorSet( *reinterpret_cast<float*>(THISPTR_OFFSET( GothicMemoryLocations::zCFlash::Offset_EndPosX )),
            *reinterpret_cast<float*>(THISPTR_OFFSET( GothicMemoryLocations::zCFlash::Offset_EndPosY )),
            *reinterpret_cast<float*>(THISPTR_OFFSET( GothicMemoryLocations::zCFlash::Offset_EndPosZ )),
            0.0f );
    }
};
