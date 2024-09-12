#pragma once
#include "pch.h"
#include "HookedFunctions.h"
#include "zCPolygon.h"
#include "Engine.h"
#include "GothicAPI.h"
#include "zCTimer.h"
#include "zCPolyStrip.h"

class zSTRING;
class zCPolyStrip;
class zCMesh;
class zCProgMeshProto;

struct zTParticle {
    zTParticle* Next;

#ifdef BUILD_GOTHIC_2_6_fix
    XMFLOAT3 PositionLocal;
#endif
    XMFLOAT3 PositionWS;
    XMFLOAT3 Vel;
    float LifeSpan;
    float Alpha;
    float AlphaVel;
    XMFLOAT2 Size;
    XMFLOAT2 SizeVel;
    XMFLOAT3 Color;
    XMFLOAT3 ColorVel;

#ifdef BUILD_GOTHIC_1_08k
    float TexAniFrame;
#endif

    zCPolyStrip* PolyStrip; // TODO: Use this too
};

class zCParticleEmitter {
public:

    zCTexture* GetVisTexture() {
        return *reinterpret_cast<zCTexture**>(THISPTR_OFFSET( GothicMemoryLocations::zCParticleEmitter::Offset_VisTexture ));
    }

    zTRnd_AlphaBlendFunc GetVisAlphaFunc() {
        return *reinterpret_cast<zTRnd_AlphaBlendFunc*>(THISPTR_OFFSET( GothicMemoryLocations::zCParticleEmitter::Offset_VisAlphaBlendFunc ));
    }

    int GetVisIsQuadPoly() {
        return *reinterpret_cast<int*>(THISPTR_OFFSET( GothicMemoryLocations::zCParticleEmitter::Offset_VisIsQuadPoly ));
    }

    int GetVisAlignment() {
        return *reinterpret_cast<int*>(THISPTR_OFFSET( GothicMemoryLocations::zCParticleEmitter::Offset_VisAlignment ));
    }

    int GetVisShpRender() {
        return *reinterpret_cast<int*>(THISPTR_OFFSET( GothicMemoryLocations::zCParticleEmitter::Offset_VisShpRender ));
    }

    int GetVisShpType() {
        return *reinterpret_cast<int*>(THISPTR_OFFSET( GothicMemoryLocations::zCParticleEmitter::Offset_VisShpType ));
    }

#ifndef BUILD_GOTHIC_1_08k
    int GetVisTexAniIsLooping() {
        return *reinterpret_cast<int*>(THISPTR_OFFSET( GothicMemoryLocations::zCParticleEmitter::Offset_VisTexAniIsLooping ));
    }

    float GetVisAlphaStart() {
        return *reinterpret_cast<float*>(THISPTR_OFFSET( GothicMemoryLocations::zCParticleEmitter::Offset_VisAlphaStart ));
    }

    float GetAlphaDist() {
        return *reinterpret_cast<float*>(THISPTR_OFFSET( GothicMemoryLocations::zCParticleEmitter::Offset_AlphaDist ));
    }

    zCMesh* GetVisShpMesh() {
        return *reinterpret_cast<zCMesh**>(THISPTR_OFFSET( GothicMemoryLocations::zCParticleEmitter::Offset_VisShpMesh ));
    }

    zCProgMeshProto* GetVisShpProgMesh() {
        return *reinterpret_cast<zCProgMeshProto**>(THISPTR_OFFSET( GothicMemoryLocations::zCParticleEmitter::Offset_VisShpProgMesh ));
    }

    zCModel* GetVisShpModel() {
        return *reinterpret_cast<zCModel**>(THISPTR_OFFSET( GothicMemoryLocations::zCParticleEmitter::Offset_VisShpModel ));
    }
#else
    int GetVisTexAniIsLooping() {
        return 0;
    }

    float GetAlphaDist() {
        return 0;
    }

    float GetVisAlphaStart() {
        return 0;
    }

    zCMesh* GetVisShpMesh() {
        return *reinterpret_cast<zCMesh**>(THISPTR_OFFSET( GothicMemoryLocations::zCParticleEmitter::Offset_VisShpMesh ));
    }

    zCProgMeshProto* GetVisShpProgMesh() {
        return nullptr;
    }

    zCModel* GetVisShpModel() {
        return nullptr;
    }
#endif
};

class zCStaticPfxList {
public:
    void TouchPfx( zCParticleFX* pfx ) {
        reinterpret_cast<void( __fastcall* )( zCStaticPfxList*, int, zCParticleFX* )>
            ( GothicMemoryLocations::zCStaticPfxList::TouchPFX )( this, 0, pfx );
    }

    zCParticleFX* PfxListHead;
    zCParticleFX* PfxListTail;
    int	NumInPfxList;
};

class zCParticleFX {
public:
    /** Hooks the functions of this Class */
    static void Hook() {
        DetourAttach( &reinterpret_cast<PVOID&>(HookedFunctions::OriginalFunctions.original_zCParticleFXDestructor), Hooked_Destructor );
    }

    static void __fastcall Hooked_Destructor( zCParticleFX* thisptr, void* unknwn ) {
        hook_infunc
            // Notify the world
            if ( Engine::GAPI )
                Engine::GAPI->OnParticleFXDeleted( thisptr );

        HookedFunctions::OriginalFunctions.original_zCParticleFXDestructor( thisptr );

        hook_outfunc
    }

    static float SinEase( float value ) {
        return (sin( value * XM_PI - XM_PIDIV2 ) + 1.f) / 2.f;
    }

    static float SinSmooth( float value ) {
        if ( value < 0.5f )
            return SinEase( value * 2 );
        else
            return 1.0f - SinEase( (value - 0.5f) * 2 );
    }

    zCMesh* GetPartMeshQuad() {
        return *reinterpret_cast<zCMesh**>(GothicMemoryLocations::zCParticleFX::OBJ_s_partMeshQuad);
    }

    zCParticleEmitter* GetEmitter() {
        return *reinterpret_cast<zCParticleEmitter**>(THISPTR_OFFSET( GothicMemoryLocations::zCParticleFX::Offset_Emitters ));
    }

    zTParticle* GetFirstParticle() {
        return *reinterpret_cast<zTParticle**>(THISPTR_OFFSET( GothicMemoryLocations::zCParticleFX::Offset_FirstParticle ));
    }

    void SetFirstParticle( zTParticle* particle ) {
        *reinterpret_cast<zTParticle**>(THISPTR_OFFSET( GothicMemoryLocations::zCParticleFX::Offset_FirstParticle )) = particle;
    }

    zCVob* GetConnectedVob() {
        return *reinterpret_cast<zCVob**>(THISPTR_OFFSET( GothicMemoryLocations::zCParticleFX::Offset_ConnectedVob ));
    }

    float GetTimeScale() {
        return *reinterpret_cast<float*>(THISPTR_OFFSET( GothicMemoryLocations::zCParticleFX::Offset_TimeScale ));
    }

    float* GetPrivateTotalTime() {
        return reinterpret_cast<float*>(THISPTR_OFFSET( GothicMemoryLocations::zCParticleFX::Offset_PrivateTotalTime ));
    }

    int UpdateParticleFX() {
        return reinterpret_cast<int( __fastcall* )( zCParticleFX* )>( GothicMemoryLocations::zCParticleFX::UpdateParticleFX )( this );
    }

    void CheckDependentEmitter() {
        reinterpret_cast<void( __fastcall* )( zCParticleFX* )>( GothicMemoryLocations::zCParticleFX::CheckDependentEmitter )( this );
    }

    zCStaticPfxList* GetStaticPFXList() {
        return reinterpret_cast<zCStaticPfxList*>(GothicMemoryLocations::zCParticleFX::OBJ_s_pfxList);
    }

    void SetLocalTimeF( float t ) {
        *reinterpret_cast<float*>(THISPTR_OFFSET( GothicMemoryLocations::zCParticleFX::Offset_LocalFrameTimeF )) = t;
    }

    void UpdateTime() {
        SetLocalTimeF( GetTimeScale() * zCTimer::GetTimer()->frameTimeFloat );
    }

    void CreateParticlesUpdateDependencies() {
        reinterpret_cast<void( __fastcall* )( zCParticleFX* )>( GothicMemoryLocations::zCParticleFX::CreateParticlesUpdateDependencies )( this );
    }

    void UpdateParticle( zTParticle* p ) {
        reinterpret_cast<void( __fastcall* )( zCParticleFX*, int, zTParticle* )>
            ( GothicMemoryLocations::zCParticleFX::UpdateParticle )( this, 0, p );
    }

    void SetVisualUsedBy( zCVob* vob ) {
        reinterpret_cast<void( __fastcall* )( zCParticleFX*, int, zCVob* )>
            ( GothicMemoryLocations::zCParticleFX::SetVisualUsedBy )( this, 0, vob );
    }

    int GetVisualDied() {
        return reinterpret_cast<int( __fastcall* )( zCParticleFX* )>( GothicMemoryLocations::zCParticleFX::GetVisualDied )( this );
    }
};

