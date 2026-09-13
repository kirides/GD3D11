#pragma once
#include "pch.h"
#include "zTypes.h"
#include "HookedFunctions.h"
#include "Engine.h"
#include "GothicAPI.h"

class zCRndD3D {
public:

    /** Hooks the functions of this Class */
    static void Hook() {
        DetourAttachTyped( &HookedFunctions::OriginalFunctions.original_zCRnd_D3D_DrawLineZ, hooked_zCRndD3DDrawLineZ  );
        DetourAttachTyped( &HookedFunctions::OriginalFunctions.original_zCRnd_D3D_DrawLine, hooked_zCRndD3DDrawLine  );

        DetourAttachTyped( &HookedFunctions::OriginalFunctions.original_zCRnd_D3D_DrawPoly, hooked_zCRndD3DDrawPoly  );
        DetourAttachTyped( &HookedFunctions::OriginalFunctions.original_zCRnd_D3D_DrawPolySimple, hooked_zCRndD3DDrawPolySimple  );

        DetourAttachTyped( &HookedFunctions::OriginalFunctions.original_zCRnd_D3D_CacheInSurface, hooked_zCSurfaceCache_D3DCacheInSurface  );
        DetourAttachTyped( &HookedFunctions::OriginalFunctions.original_zCRnd_D3D_CacheOutSurface, hooked_zCSurfaceCache_D3DCacheOutSurface  );

        DetourAttachTyped( &HookedFunctions::OriginalFunctions.original_zCRnd_D3D_RenderScreenFade, hooked_zCCameraRenderScreenFade  );
        DetourAttachTyped( &HookedFunctions::OriginalFunctions.original_zCRnd_D3D_RenderCinemaScope, hooked_zCCameraRenderCinemaScope  );
    }

    /** Disable caching surfaces to let engine create new surfaces for every textures */
    static int __fastcall hooked_zCSurfaceCache_D3DCacheInSurface( void* thisptr, void* _EDX, void* surface, void* slotindex ) {
        return FALSE;
    }

    static void* __fastcall hooked_zCSurfaceCache_D3DCacheOutSurface( void* thisptr, void* _EDX, void* slotindex ) {
        return nullptr;
    }

    /** Draws a straight line from xyz1 to xyz2 */
    static void __fastcall hooked_zCRndD3DDrawLineZ( void* thisptr, void* unknwn, float x1, float y1, float z1VSInv, float x2, float y2, float z2VSInv, zColor color ) {
        if ( color.bgra.alpha == 0 ) {
            color.bgra.alpha = 255;
        }
        auto lineRenderer = Engine::GraphicsEngine->GetLineRenderer();
        if ( lineRenderer ) {
            auto& proj = Engine::GAPI->GetProjectionMatrix();
            float actualz1 = proj._33 + proj._34 * z1VSInv;
            float actualz2 = proj._33 + proj._34 * z2VSInv;
            lineRenderer->AddLineScreenSpace( LineVertex( XMFLOAT3( x1, y1, actualz1 ), color.dword, z1VSInv ), LineVertex( XMFLOAT3( x2, y2, actualz2 ), color.dword, z2VSInv ) );
        }
    }

    static void __fastcall hooked_zCRndD3DDrawLine( void* thisptr, void* unknwn, float x1, float y1, float x2, float y2, zColor color ) {
        if ( color.bgra.alpha == 0 ) {
            color.bgra.alpha = 255;
        }
        if ( Engine::GraphicsEngine->UseUIRenderer2D() ) {
            Engine::GraphicsEngine->GetUIRenderer2D().AddLine( x1, y1, x2, y2, color.dword );
            return;
        }
        auto lineRenderer = Engine::GraphicsEngine->GetLineRenderer();
        if ( lineRenderer ) {
            lineRenderer->AddLineScreenSpace( LineVertex( XMFLOAT3( x1, y1, 1.f ), color.dword, 1.f ), LineVertex( XMFLOAT3( x2, y2, 1.f ), color.dword, 1.f ) );
        }
    }

    static void __fastcall hooked_zCRndD3DDrawPoly( void* thisptr, void* unknwn, zCPolygon* poly ) {
        // Check if it's a call from zCPolyStrip render(), if so
        // prevent original function from execution, since we only need
        // zCPolyStrip render() to do pre-render computations
        // Relevant assembly parts (G2):

        // DrawPoly call inside zCPolyStrip render():
        // .text:005BE18C                 push    edi
        // .text:005BE18D                 call    dword ptr[eax + 10h]
        // .text:005BE190

        // DrawPoly function:
        // .text:0064B260 ; void __thiscall zCRnd_D3D::DrawPoly(zCRnd_D3D *this, struct zCPolygon *)
        // .text:0064B260 ? DrawPoly@zCRnd_D3D@@UAEXPAVzCPolygon@@@Z proc near

        hook_infunc

            void* polyStripReturnPointer = reinterpret_cast<void*>(GothicMemoryLocations::zCPolyStrip::RenderDrawPolyReturn);
            if ( _ReturnAddress() != polyStripReturnPointer ) {
                HookedFunctions::OriginalFunctions.original_zCRnd_D3D_DrawPoly( thisptr, poly );
            }

        hook_outfunc
    }

    static void __fastcall hooked_zCRndD3DDrawPolySimple( void* thisptr, void* unknwn, zCTexture* texture, zTRndSimpleVertex* vertices, int numVertices ) {
        hook_infunc

            // The sky pass keeps the fixed-function path; D3D12 draws it into the HDR scene target.
            if ( Engine::GraphicsEngine->UseUIRenderer2D()
                && Engine::GAPI->GetRendererState().RendererInfo.RenderStage != STAGE_DRAW_SKY ) {
                Engine::GraphicsEngine->GetUIRenderer2D().AddPolygon( texture, vertices, numVertices, ReadPolygonState( thisptr ) );
            } else {
                HookedFunctions::OriginalFunctions.original_zCRnd_D3D_DrawPolySimple( thisptr, texture, vertices, numVertices );
            }

        hook_outfunc
    }

    /** The zCRnd_D3D::xd3d_actStatus fields DrawPolySimple reads. */
    static UIPolygonState ReadPolygonState( void* renderer ) {
        const DWORD status = reinterpret_cast<DWORD>( renderer ) + GothicMemoryLocations::zCRndD3D::Offset_ActStatus;
        UIPolygonState state;
        state.Bilinear = *reinterpret_cast<int*>( status + GothicMemoryLocations::zCRndD3D::ActStatus_Offset_Filter ) != 0;
        state.AlphaFunc = *reinterpret_cast<int*>( status + GothicMemoryLocations::zCRndD3D::ActStatus_Offset_AlphaFunc );
        state.AlphaSourceConstant = *reinterpret_cast<int*>( status + GothicMemoryLocations::zCRndD3D::ActStatus_Offset_AlphaSource ) == 1; // zRND_ALPHA_SOURCE_CONSTANT
        state.AlphaFactor = *reinterpret_cast<float*>( status + GothicMemoryLocations::zCRndD3D::ActStatus_Offset_AlphaFactor );
        return state;
    }

    static void __fastcall hooked_zCCameraRenderScreenFade( void* thisptr ) {
        Engine::GraphicsEngine->FlushUI2D();
        Engine::GraphicsEngine->DrawScreenFade( thisptr );
    }

    static void __fastcall hooked_zCCameraRenderCinemaScope( void* thisptr ) {
        // Do nothing here
    }

    void ResetRenderState() {
        // Set render state values to some absurd high value so that they will be changed by engine for sure
        *reinterpret_cast<DWORD*>(THISPTR_OFFSET( GothicMemoryLocations::zCRndD3D::Offset_BoundTexture + ( /*TEX0*/0 * 4) )) = 0x00000000;
        *reinterpret_cast<DWORD*>(THISPTR_OFFSET( GothicMemoryLocations::zCRndD3D::Offset_RenderState + ( /*D3DRENDERSTATE_ALPHABLENDENABLE*/27 * 4 ) )) = 0xFFFFFFFF;
        *reinterpret_cast<DWORD*>(THISPTR_OFFSET( GothicMemoryLocations::zCRndD3D::Offset_RenderState + ( /*D3DRENDERSTATE_SRCBLEND*/19 * 4 ) )) = 0xFFFFFFFF;
        *reinterpret_cast<DWORD*>(THISPTR_OFFSET( GothicMemoryLocations::zCRndD3D::Offset_RenderState + ( /*D3DRENDERSTATE_DESTBLEND*/20 * 4 ) )) = 0xFFFFFFFF;
    }

    static zCRndD3D* GetRenderer() {
        return *reinterpret_cast<zCRndD3D**>(GothicMemoryLocations::GlobalObjects::zRenderer);
    }
};
