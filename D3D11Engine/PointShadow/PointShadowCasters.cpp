#include "../pch.h"
#include "PointShadowCasters.h"

#include "../D3D11GraphicsEngine.h"
#include "../Engine.h"
#include "../GothicAPI.h"
#include "../PointLightSlotSelector.h"
#include "../RenderToTextureBuffer.h"
#include "../WorldConverter.h"
#include "../WorldObjects.h"
#include "../oCVisFX.h"
#include "../zCVobLight.h"

extern bool RequiresNvidiaTiledShadowFaceFallback;
extern bool UseAbsoluteCubeSliceIndexing;

namespace {
    std::unordered_set<const zCVob*> vobsToExclude = {};
    std::move_only_function<bool( const zCVob* ) const> excludeVobsToExclude = []( const zCVob* vob )
    {
        return vobsToExclude.contains( vob );
    };

    void CollectVobTreeToExclude( const zCVob* vob ) {
        while ( vob && vobsToExclude.emplace( vob ).second ) {
            if ( auto vfx = vob->As<oCVisualFX>() ) {
                if ( auto origin = vfx->GetOrigin() ) {
                    vobsToExclude.emplace( origin );
                    CollectVobTreeToExclude( origin );
                }
            }

            vob = vob->GetVobParent();
        }
    }

    // This allows us to exclude ourselves from producing shadows.
    // This is needed for example in Returning, where the Belt-Light otherwise
    // draws a huge shadow from the player all around and
    void SetupVobsToExclude( const VobLightInfo* lightInfo ) {
        if ( Engine::GAPI->GetRendererState().RendererSettings.AllowSelfShadowingPointlights ) {
            return;
        }
        vobsToExclude.clear();

        CollectVobTreeToExclude( lightInfo->Vob );
    }

    const zCVob* GetOriginVob( const VobLightInfo* info ) {
        if ( Engine::GAPI->GetRendererState().RendererSettings.AllowSelfShadowingPointlights ) {
            return nullptr;
        }
        if ( !info->IsPFXVobLight ) {
            thread_local std::unordered_set<const zCVob*> seen{};
            seen.clear();

            zCVob* vob = info->Vob;
            while ( vob ) {
                if ( !seen.emplace( vob ).second ) {
                    break;
                }
                if ( auto visFx = vob->As<oCVisualFX>() ) {
                    if ( auto origin = visFx->GetOrigin(); origin && origin->As<oCItem>() ) {
                        return origin;
                    }
                } else if ( vob->As<oCItem>() ) {
                    return vob;
                }
                vob = vob->GetVobParent();
            }
        }
        return nullptr;
    }

    /** The per-face fallback: 6 single-slice DSVs instead of one layered draw routed by
        SV_RenderTargetArrayIndex - see RequiresNvidiaTiledShadowFaceFallback. */
    void RenderFacePasses( const PointShadowCasters::CubeRenderScope& scope,
        const PointShadowCasters::CasterPass& pass,
        const std::move_only_function<bool( const zCVob* ) const>* ignoreVob ) {

        D3D11GraphicsEngine* engine = AsD3D11Engine( Engine::GraphicsEngine );
        auto _ = engine->RecordGraphicsEvent( GE_NAME( "PointShadowCasters->RenderFacePasses" ) );

        const auto lightPos = pass.Light->Vob->GetPositionWorldXM();
        Frustum f;
        f.BuildCubemapFace( lightPos, pass.Range, 0 ); // cubemap frustum is a sphere. not per face.
        CameraReplacement cr;
        cr.frustum = f;
        XMStoreFloat3( &cr.PositionReplacement, lightPos );
        cr.ProjectionReplacement = scope.Proj();

        // No GS bound on this path, so skeletal draws must skip VS_ExSkeletalCube/VS_ExNodeCube (GS-dependent).
        engine->SetCubeFaceFallbackActive( true );

        for ( UINT face = 0; face < 6; ++face ) {
            cr.ViewReplacement = scope.View( face );
            Engine::GAPI->SetCameraReplacementPtr( &cr );

            const auto& faceDsv = pass.Target->GetDSVCubemapFace( face );
            if ( ignoreVob ) {
                engine->RenderShadowCube( lightPos, pass.Range, *pass.Target, faceDsv, nullptr, false,
                    pass.Light->IsIndoorVob, false, pass.VobCache, pass.MobCache, pass.WorldMeshCache,
                    pass.ClearDepth, pass.CasterMask, *ignoreVob );
            } else {
                engine->RenderShadowCube( lightPos, pass.Range, *pass.Target, faceDsv, nullptr, false,
                    pass.Light->IsIndoorVob, false, pass.VobCache, pass.MobCache, pass.WorldMeshCache,
                    pass.ClearDepth, pass.CasterMask );
            }
        }

        engine->SetCubeFaceFallbackActive( false );
        Engine::GAPI->SetCameraReplacementPtr( nullptr );
    }
}

namespace PointShadowCasters {

    bool AllowsDynamicCasters( const VobLightInfo* info ) {
        return PointLightSlotSelector::AllowsDynamicCasters( info );
    }

    bool RestrictsCastersToWorld( const VobLightInfo* info ) {
        return !AllowsDynamicCasters( info );
    }

    bool UsesAbsoluteSliceIndexing( const RenderToDepthStencilBuffer* target ) {
        return UseAbsoluteCubeSliceIndexing && target && target->GetArrayDepthStencilView();
    }

    bool HasAnimatedCastersInRange( const VobLightInfo* info, float shadowRange ) {
        if ( !info || !info->Vob ) return false;
        if ( !Engine::GAPI->GetRendererState().RendererSettings.DrawSkeletalMeshes ) return false;
        // Mirrors the animated pass's per-vob tests, minus the indoor/outdoor one - being wrong there only
        // costs one empty overlay render.
        const XMVECTOR pos = info->Vob->GetPositionWorldXM();
        const XMVECTOR rangeSq = XMVectorReplicate( shadowRange * shadowRange );
        for ( const SkeletalVobInfo* vob : Engine::GAPI->GetAnimatedSkeletalMeshVobs() ) {
            if ( !vob || !vob->VisualInfo || !vob->Vob ) continue;
            if ( vob->Vob->GetVisualAlpha() && vob->Vob->GetVobTransparency() < 0.7f ) continue;   // ghosts
            if ( XMVector3Greater( XMVector3LengthSq( pos - vob->Vob->GetPositionWorldXM() ), rangeSq ) ) continue;
            return true;
        }
        return false;
    }

    CubeRenderScope::CubeRenderScope( VobLightInfo* info, float shadowRange ) {
        const XMFLOAT3 vobPos = info->Vob->GetPositionWorld();
        const XMVECTOR vEyePt = XMLoadFloat3( &vobPos );
        const XMVECTOR c_XM_Right = XMVectorSet( 1.f, 0.f, 0.f, 0.f );
        const XMVECTOR c_XM_Left = XMVectorSet( -1.f, 0.f, 0.f, 0.f );
        const XMVECTOR c_XM_Up = XMVectorSet( 0.f, 1.f, 0.f, 0.f );
        const XMVECTOR c_XM_Down = XMVectorSet( 0.f, -1.f, 0.f, 0.f );
        const XMVECTOR c_XM_Forward = XMVectorSet( 0.f, 0.f, 1.f, 0.f );
        const XMVECTOR c_XM_Backward = XMVectorSet( 0.f, 0.f, -1.f, 0.f );

        // Update indoor/outdoor-state
        info->IsIndoorVob = info->Vob->IsIndoorVob();

        XMVECTOR vLookDir;
        vLookDir = XMVectorAdd( c_XM_Right, vEyePt );
        XMStoreFloat4x4( &m_View[0], XMMatrixTranspose( XMMatrixLookAtLH( vEyePt, vLookDir, c_XM_Up ) ) );

        vLookDir = XMVectorAdd( c_XM_Left, vEyePt );
        XMStoreFloat4x4( &m_View[1], XMMatrixTranspose( XMMatrixLookAtLH( vEyePt, vLookDir, c_XM_Up ) ) );

        vLookDir = XMVectorAdd( c_XM_Up, vEyePt );
        XMStoreFloat4x4( &m_View[2], XMMatrixTranspose( XMMatrixLookAtLH( vEyePt, vLookDir, c_XM_Backward ) ) );

        vLookDir = XMVectorAdd( c_XM_Down, vEyePt );
        XMStoreFloat4x4( &m_View[3], XMMatrixTranspose( XMMatrixLookAtLH( vEyePt, vLookDir, c_XM_Forward ) ) );

        vLookDir = XMVectorAdd( c_XM_Forward, vEyePt );
        XMStoreFloat4x4( &m_View[4], XMMatrixTranspose( XMMatrixLookAtLH( vEyePt, vLookDir, c_XM_Up ) ) );

        vLookDir = XMVectorAdd( c_XM_Backward, vEyePt );
        XMStoreFloat4x4( &m_View[5], XMMatrixTranspose( XMMatrixLookAtLH( vEyePt, vLookDir, c_XM_Up ) ) );

        m_ZNear = 15.0f;
        m_ZFar = shadowRange * 2.0f;

        XMMATRIX proj = XMMatrixPerspectiveFovLH( XM_PIDIV2, 1.0f, m_ZNear, m_ZFar );
        proj = XMMatrixTranspose( proj );
        XMStoreFloat4x4( &m_Proj, proj );

        // The cube keeps the natural hyperbolic z of this projection, which PLS_PrepareShadowSampling
        // reconstructs from the same zNear/zFar - see Shaders/include/PointLightShadows.h.
        Engine::GAPI->GetRendererState().GraphicsState.FF_zNear = m_ZNear;
        Engine::GAPI->GetRendererState().GraphicsState.FF_zFar = m_ZFar;
        Engine::GAPI->GetRendererState().GraphicsState.SetGraphicsSwitch( GSWITCH_CUBE_SHADOW, true );

        m_SavedDepthClip = Engine::GAPI->GetRendererState().RasterizerState.DepthClipEnable;
        Engine::GAPI->GetRendererState().RasterizerState.DepthClipEnable = true;

        for ( int i = 0; i < 6; i++ ) {
            m_GCB.PCR_View[i] = m_View[i];
            XMStoreFloat4x4( &m_GCB.PCR_ViewProj[i], proj * XMLoadFloat4x4( &m_View[i] ) );
        }
    }

    void CubeRenderScope::BindCubeCB( unsigned int sliceBase ) const {
        D3D11GraphicsEngine* engine = AsD3D11Engine( Engine::GraphicsEngine );

        // The ring is per-frame, so this re-allocates per pass rather than holding one from the ctor.
        CubemapGSConstantBuffer gcb = m_GCB;
        gcb.PCR_SliceBase = sliceBase;

        ConstantBufferAllocation viewMatricesCB = engine->AllocateDynamicCB( &gcb, sizeof( gcb ) );
        engine->BindDynamicCBToVertexShader( 3, viewMatricesCB ); // Layered vertex shader
        engine->BindDynamicCBToGeometryShader( 2, viewMatricesCB ); // Cubemap geometry shader
    }

    CubeRenderScope::~CubeRenderScope() {
        Engine::GAPI->GetRendererState().RasterizerState.DepthClipEnable = m_SavedDepthClip;
        Engine::GAPI->GetRendererState().GraphicsState.SetGraphicsSwitch( GSWITCH_CUBE_SHADOW, false );
    }

    void Render( const CubeRenderScope& scope, const CasterPass& pass ) {
        if ( !pass.Light || !pass.Light->Vob || !pass.Target ) return;

        D3D11GraphicsEngine* engine = AsD3D11Engine( Engine::GraphicsEngine );

        const bool excludeSelf = GetOriginVob( pass.Light ) != nullptr;
        if ( excludeSelf ) {
            SetupVobsToExclude( pass.Light );
        }

        // Already keeps every view at FirstArraySlice=0, so it replaces the fallback instead of stacking.
        const bool absoluteSlice = UsesAbsoluteSliceIndexing( pass.Target );
        scope.BindCubeCB( absoluteSlice ? pass.Target->GetBaseArraySlice() : 0u );

        if ( !absoluteSlice && RequiresNvidiaTiledShadowFaceFallback && pass.TargetIsSharedArray ) {
            RenderFacePasses( scope, pass, excludeSelf ? &excludeVobsToExclude : nullptr );
        } else if ( excludeSelf ) {
            engine->RenderShadowCube( pass.Light->Vob->GetPositionWorldXM(), pass.Range, *pass.Target,
                nullptr, nullptr, false, pass.Light->IsIndoorVob, false,
                pass.VobCache, pass.MobCache, pass.WorldMeshCache, pass.ClearDepth, pass.CasterMask,
                excludeVobsToExclude );
        } else {
            engine->RenderShadowCube( pass.Light->Vob->GetPositionWorldXM(), pass.Range, *pass.Target,
                nullptr, nullptr, false, pass.Light->IsIndoorVob, false,
                pass.VobCache, pass.MobCache, pass.WorldMeshCache, pass.ClearDepth, pass.CasterMask );
        }

        if ( excludeSelf ) {
            vobsToExclude.clear();
        }
    }

    void RenderStatic( const CubeRenderScope& scope, CasterPass pass ) {
        // PFX-driven lights (candles/torches/campfires - oCVisualFX-owned rather than a static level light)
        // can be parented anywhere in the vob tree, including onto NPCs/the player. GetOriginVob's
        // self-exclusion only walks the oCItem-origin chain, so a PFX light whose origin ISN'T an item has no
        // reliable way to exclude its own carrier from the caster set - that's what used to make e.g. a
        // belt-mounted light draw a huge shadow from the player all around. Restricting a light's category to
        // world-mesh-only casters (see AllowsDynamicCasters/PointlightShadowCasterFlags) sidesteps that class
        // of bug for PFX lights entirely, and separately keeps static lights off the expensive VOB/MOB set.
        pass.CasterMask = RestrictsCastersToWorld( pass.Light )
            ? SHADOW_CASTER_WORLD
            : SHADOW_CASTER_WORLD | SHADOW_CASTER_VOBS | SHADOW_CASTER_MOBS;
        Render( scope, pass );
    }

    void RenderAnimated( const CubeRenderScope& scope, CasterPass pass ) {
        pass.CasterMask = SHADOW_CASTER_ANIMATED;
        // The animated overlay never reuses the cached caster lists - it re-collects this frame's movers.
        pass.VobCache = nullptr;
        pass.MobCache = nullptr;
        pass.WorldMeshCache = nullptr;
        Render( scope, pass );
    }

    void RenderAll( const CubeRenderScope& scope, CasterPass pass ) {
        // Keeps RenderStatic's world-only restriction.
        pass.CasterMask = RestrictsCastersToWorld( pass.Light ) ? SHADOW_CASTER_WORLD : SHADOW_CASTER_ALL;
        // Never reuses the world-mesh candidate cache - it always re-collects the whole scene fresh.
        pass.WorldMeshCache = nullptr;
        Render( scope, pass );
    }

} // namespace PointShadowCasters
