#include "../pch.h"
#include "PointShadowCasters.h"

#include "../D3D11GraphicsEngine.h"
#include "../Engine.h"
#include "../GothicAPI.h"
#include "../PointLightSlotSelector.h"
#include "../RenderToTextureBuffer.h"
#include "../WorldObjects.h"
#include "../oCVisFX.h"
#include "../zCVobLight.h"
#include "PointShadowBatch.h"

extern bool UseAbsoluteCubeSliceIndexing;

namespace {
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

    void CollectExcludedVobs( const VobLightInfo* info, std::vector<const zCVob*>& out ) {
        // Keeps e.g. the Returning belt light from throwing a huge shadow of its own wearer all around.
        if ( !GetOriginVob( info ) ) return;

        const size_t first = out.size();
        auto seen = [&]( const zCVob* vob ) { return std::find( out.begin() + first, out.end(), vob ) != out.end(); };
        for ( const zCVob* vob = info->Vob; vob && !seen( vob ); vob = vob->GetVobParent() ) {
            out.push_back( vob );
            if ( auto vfx = vob->As<oCVisualFX>() ) {
                if ( const zCVob* origin = vfx->GetOrigin(); origin && !seen( origin ) ) {
                    out.push_back( origin );
                }
            }
        }
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

        // The cube keeps the natural hyperbolic z of this projection, which PLS_PrepareShadowSampling
        // reconstructs from the same zNear/zFar - see Shaders/include/PointLightShadows.h.
        XMMATRIX proj = XMMatrixPerspectiveFovLH( XM_PIDIV2, 1.0f, m_ZNear, m_ZFar );
        proj = XMMatrixTranspose( proj );
        XMStoreFloat4x4( &m_Proj, proj );

        for ( int i = 0; i < 6; i++ ) {
            m_GCB.PCR_View[i] = m_View[i];
            XMStoreFloat4x4( &m_GCB.PCR_ViewProj[i], proj * XMLoadFloat4x4( &m_View[i] ) );
        }
    }

    void QueueStatic( const CubeRenderScope& scope, CasterPass pass ) {
        // PFX lights can ride anything, NPCs included, and have no reliable self-exclusion, so their category
        // is world-mesh-only; that also keeps static lights off the expensive VOB/MOB set.
        pass.CasterMask = RestrictsCastersToWorld( pass.Light )
            ? SHADOW_CASTER_WORLD
            : SHADOW_CASTER_WORLD | SHADOW_CASTER_VOBS | SHADOW_CASTER_MOBS;
        PointShadowBatch::Queue( scope, pass );
    }

    void QueueAnimated( const CubeRenderScope& scope, CasterPass pass ) {
        pass.CasterMask = SHADOW_CASTER_ANIMATED;
        // The animated overlay never reuses the cached caster lists - it re-collects this frame's movers.
        pass.VobCache = nullptr;
        pass.MobCache = nullptr;
        pass.WorldMeshCache = nullptr;
        PointShadowBatch::Queue( scope, pass );
    }

    void QueueAll( const CubeRenderScope& scope, CasterPass pass ) {
        // Keeps QueueStatic's world-only restriction.
        pass.CasterMask = RestrictsCastersToWorld( pass.Light ) ? SHADOW_CASTER_WORLD : SHADOW_CASTER_ALL;
        // Never reuses the world-mesh candidate cache - it always re-collects the whole scene fresh.
        pass.WorldMeshCache = nullptr;
        PointShadowBatch::Queue( scope, pass );
    }

} // namespace PointShadowCasters
