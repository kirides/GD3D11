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
        DetourAttach( &reinterpret_cast<PVOID&>(HookedFunctions::OriginalFunctions.original_zCRnd_D3D_DrawLineZ), hooked_zCRndD3DDrawLineZ );
        DetourAttach( &reinterpret_cast<PVOID&>(HookedFunctions::OriginalFunctions.original_zCRnd_D3D_DrawLine), hooked_zCRndD3DDrawLine );

        DetourAttach( &reinterpret_cast<PVOID&>(HookedFunctions::OriginalFunctions.original_zCRnd_D3D_DrawPoly), hooked_zCRndD3DDrawPoly );
        DetourAttach( &reinterpret_cast<PVOID&>(HookedFunctions::OriginalFunctions.original_zCRnd_D3D_DrawPolySimple), hooked_zCRndD3DDrawPolySimple );

        DetourAttach( &reinterpret_cast<PVOID&>(HookedFunctions::OriginalFunctions.original_zCRnd_D3D_CacheInSurface), hooked_zCSurfaceCache_D3DCacheInSurface );
        DetourAttach( &reinterpret_cast<PVOID&>(HookedFunctions::OriginalFunctions.original_zCRnd_D3D_CacheOutSurface), hooked_zCSurfaceCache_D3DCacheOutSurface );

        DetourAttach( &reinterpret_cast<PVOID&>(HookedFunctions::OriginalFunctions.original_zCRnd_D3D_RenderScreenFade), hooked_zCCameraRenderScreenFade );
        DetourAttach( &reinterpret_cast<PVOID&>(HookedFunctions::OriginalFunctions.original_zCRnd_D3D_RenderCinemaScope), hooked_zCCameraRenderCinemaScope );
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

    static void __fastcall hooked_zCRndD3DDrawPolySimple( void* thisptr, void* unknwn, zCTexture* texture, zTRndSimpleVertex* zTRndSimpleVertex, int iVal ) {
        hook_infunc

            HookedFunctions::OriginalFunctions.original_zCRnd_D3D_DrawPolySimple( thisptr, texture, zTRndSimpleVertex, iVal );

        hook_outfunc
    }

    static void __fastcall hooked_zCCameraRenderScreenFade( void* thisptr ) {
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
