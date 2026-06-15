#pragma once
#include "pch.h"
#include "HookedFunctions.h"
#include "zCPolygon.h"
#include "Engine.h"
#include "GothicAPI.h"
#include "zCTimer.h"
#include "zCMaterial.h"
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

    zCTexture* GetVisTexture( zTParticle* pfx ) {
        // Gothic 2 shares the same material object between different pfx'es
        // which can be problematic when there are different animated textures on screen
#ifdef BUILD_GOTHIC_2_6_fix
        zCTexture* texture = *reinterpret_cast<zCTexture**>(THISPTR_OFFSET( GothicMemoryLocations::zCParticleEmitter::Offset_VisTexture ));
        if ( !texture ) {
            return texture;
        }

        // Use original dx7 material
        DWORD s_partMeshQuad = *reinterpret_cast<DWORD*>( 0x8D9230 );
        DWORD poly = *reinterpret_cast<DWORD*>(*reinterpret_cast<DWORD*>( s_partMeshQuad + GothicMemoryLocations::zCMesh::Offset_Polygons ));
        zCMaterial* mat = *reinterpret_cast<zCMaterial**>( poly + GothicMemoryLocations::zCPolygon::Offset_Material );
        if ( mat ) {
            *reinterpret_cast<zCTexture**>( mat + GothicMemoryLocations::zCMaterial::Offset_Texture ) = texture;
            *reinterpret_cast<float*>( mat + 0x54 ) = *reinterpret_cast<float*>(THISPTR_OFFSET( GothicMemoryLocations::zCParticleEmitter::Offset_VisTexAniFPS )) * 0.001f;
            *reinterpret_cast<int*>( mat + 0x5C ) = (GetVisTexAniIsLooping() == 0);
            *reinterpret_cast<int*>( mat + 0x4C ) = 0;
            return mat->GetAniTexture();
        }
        return texture;
#else
        zCTexture* texture = *reinterpret_cast<zCTexture**>(THISPTR_OFFSET( GothicMemoryLocations::zCParticleEmitter::Offset_VisTexture ));
        if ( texture ) {
            unsigned char flags = *reinterpret_cast<unsigned char*>( reinterpret_cast<DWORD>( texture ) + GothicMemoryLocations::zCTexture::Offset_Flags );
            if ( flags & GothicMemoryLocations::zCTexture::Mask_FlagIsAnimated ) {
                int animationFrames = reinterpret_cast<int*>( reinterpret_cast<DWORD>( texture ) + GothicMemoryLocations::zCTexture::Offset_AniFrames )[0];
                if ( animationFrames <= 0 )
                    return texture;

                float texAni = pfx->TexAniFrame;
                if ( texAni >= animationFrames ) {
                    if ( GetVisTexAniIsLooping() ) {
                        do {
                            texAni -= animationFrames;
                        } while ( texAni >= animationFrames );
                    } else {
                        texAni = (animationFrames - 1.f);
                    }
                    pfx->TexAniFrame = texAni;
                }
                reinterpret_cast<int*>( reinterpret_cast<DWORD>( texture ) + GothicMemoryLocations::zCTexture::Offset_ActAniFrame )[0] = static_cast<int>(floorf( texAni ));

                zCTexture* tex = texture;
                int activeAnimationFrame = reinterpret_cast<int*>( reinterpret_cast<DWORD>( texture ) + GothicMemoryLocations::zCTexture::Offset_ActAniFrame )[0];
                for ( int i = 0; i < activeAnimationFrame; ++i ) {
                    zCTexture* activeAnimationFrame = reinterpret_cast<zCTexture**>( reinterpret_cast<DWORD>( tex ) + GothicMemoryLocations::zCTexture::Offset_NextFrame )[0];
                    if ( !activeAnimationFrame )
                        return tex;

                    tex = activeAnimationFrame;
                }
                return tex;
            }
        }
        return texture;
#endif
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

    int GetVisTexAniIsLooping() {
        return *reinterpret_cast<int*>(THISPTR_OFFSET( GothicMemoryLocations::zCParticleEmitter::Offset_VisTexAniIsLooping ));
    }

#ifndef BUILD_GOTHIC_1_08k
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
        DetourAttachTyped( &HookedFunctions::OriginalFunctions.original_zCParticleFXDestructor, Hooked_Destructor  );
    }

    static void __fastcall Hooked_Destructor( zCParticleFX* thisptr, void* unknwn ) {
        hook_infunc

            // Notify the world
            Engine::GAPI->OnParticleFXDeleted( thisptr );

        hook_outfunc

        HookedFunctions::OriginalFunctions.original_zCParticleFXDestructor( thisptr );
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

    void UpdateParticleFX() {
        reinterpret_cast<void( __fastcall* )( zCParticleFX* )>( GothicMemoryLocations::zCParticleFX::UpdateParticleFX )( this );
    }

    void CheckDependentEmitter() {
        reinterpret_cast<void( __fastcall* )( zCParticleFX* )>( GothicMemoryLocations::zCParticleFX::CheckDependentEmitter )( this );
    }

    static zCStaticPfxList* GetStaticPFXList() {
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

