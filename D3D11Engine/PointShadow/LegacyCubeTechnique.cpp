#include "../pch.h"
#include "LegacyCubeTechnique.h"

#include "../D3D11GraphicsEngine.h"
#include "../D3D11PfxRenderer.h"
#include "../D3D11PointLight.h"
#include "../D3D11ShadowMap.h"
#include "../Engine.h"
#include "../GothicAPI.h"
#include "../LightingResourceLog.h"
#include "../RenderToTextureBuffer.h"
#include "../WorldObjects.h"
#include "../zCVobLight.h"
#include "PointShadowBatch.h"
#include "PointShadowCasters.h"
#include "PointShadowPolicy.h"

using PointShadowCasters::CasterPass;
using PointShadowCasters::CubeRenderScope;

namespace {
    /** The pool hands back a RenderToDepthStencilBuffer even when its own creation failed; a cube missing
        any of its three parts comparison-samples as fully occluded, i.e. shades the light black. Reported
        once, since the acquire is retried every frame for as long as the light wants a cube. */
    bool ValidateCube( const RenderToDepthStencilBuffer* cube, std::string_view what, int resolution ) {
        const bool ok = cube && cube->GetTexture() && cube->GetDepthStencilView() && cube->GetShaderResView();
        static bool reported = false;
        if ( ok ) {
            reported = false;
            return true;
        }
        if ( !reported ) {
            reported = true;
            Logging::Err( "Lighting resource MISSING: {} {}^2 (texture {}, DSV {}, SRV {})", what, resolution,
                cube && cube->GetTexture() ? "ok" : "null",
                cube && cube->GetDepthStencilView() ? "ok" : "null",
                cube && cube->GetShaderResView() ? "ok" : "null" );
        }
        return false;
    }
}

// ---- LegacyCubeLightState -------------------------------------------------------------------------------

LegacyCubeLightState::~LegacyCubeLightState() {
    ReleaseResources();
}

void LegacyCubeLightState::ReleaseResources() {
    // This calls the custom deleter, returning the texture to the pool
    m_DepthCubemap.reset();
    ReleaseStaticAsideShadowMap();
    m_Light.SetCurrentResolution( 0 );
    m_Light.MarkNotDrawn();
}

ID3D11Texture2D* LegacyCubeLightState::GetCubeTexture() const {
    return m_DepthCubemap ? m_DepthCubemap->GetTexture().Get() : nullptr;
}

void LegacyCubeLightState::AcquireShadowMap( DepthStencilPool* pool, int resolution ) {
    if ( m_DepthCubemap && m_Light.GetShadowMapResolution() == resolution ) return;

    // If we have a map but it's the wrong size, return it to the pool first
    if ( m_DepthCubemap ) {
        ReleaseResources();
    }

    DepthStencilPool::Description desc;
    desc.Width = resolution;
    desc.Height = resolution;
    desc.Format = m_Info.DepthFormat;
    desc.DSVFormat = m_Info.DSVFormat;
    desc.SRVFormat = m_Info.SRVFormat;
    desc.ArraySize = m_Info.FacesPerLight;

    m_DepthCubemap = pool->Acquire( desc );
    if ( !ValidateCube( m_DepthCubemap.get(), "point-light shadow cube", resolution ) ) {
        m_DepthCubemap.reset();
        return;
    }
    m_Light.SetCurrentResolution( resolution );

    // don't reset DrawnOnce here, or NPCs won't show up in the first frame a shadow gets a different LOD
    m_Light.InitResources();
}

void LegacyCubeLightState::BindForSampling() {
    if ( !m_Light.IsReady() || !m_DepthCubemap ) return;
    m_DepthCubemap->BindToPixelShader( AsD3D11Engine( Engine::GraphicsEngine )->GetContext(), 3 );
}

void LegacyCubeLightState::AcquireStaticAsideShadowMap( DepthStencilPool* pool, int resolution ) {
    if ( !pool || resolution <= 0 ) {
        return;
    }

    if ( m_StaticDepthCubemap
        && static_cast<int>( m_StaticDepthCubemap->GetSizeX() ) == resolution
        && static_cast<int>( m_StaticDepthCubemap->GetSizeY() ) == resolution ) {
        return;
    }

    ReleaseStaticAsideShadowMap();

    DepthStencilPool::Description desc;
    desc.Width = resolution;
    desc.Height = resolution;
    desc.Format = m_Info.DepthFormat;
    desc.DSVFormat = m_Info.DSVFormat;
    desc.SRVFormat = m_Info.SRVFormat;
    desc.ArraySize = m_Info.FacesPerLight;

    m_StaticDepthCubemap = pool->Acquire( desc );
    if ( !ValidateCube( m_StaticDepthCubemap.get(), "point-light static aside cube", resolution ) ) {
        m_StaticDepthCubemap.reset();
        return;
    }
    m_Light.DropStaticBake( PLR_ASIDE_BUFFER );
}

void LegacyCubeLightState::ReleaseStaticAsideShadowMap() {
    m_StaticDepthCubemap.reset();
    m_Light.DropStaticBake( PLR_ASIDE_BUFFER );
}

void LegacyCubeLightState::CopyStaticAsideToCube() const {
    if ( !m_StaticDepthCubemap || !m_DepthCubemap ) {
        return;
    }

    ID3D11Texture2D* srcTexture = m_StaticDepthCubemap->GetTexture().Get();
    ID3D11Texture2D* dstTexture = m_DepthCubemap->GetTexture().Get();
    auto context = AsD3D11Engine( Engine::GraphicsEngine )->GetContext();
    if ( !srcTexture || !dstTexture || !context ) {
        return;
    }

    for ( UINT face = 0; face < m_Info.FacesPerLight; ++face ) {
        const UINT srcSubresource = D3D11CalcSubresource( 0, face, 1 );
        const UINT dstSubresource = D3D11CalcSubresource( 0, face, 1 );
        context->CopySubresourceRegion( dstTexture, dstSubresource, 0, 0, 0, srcTexture, srcSubresource, nullptr );
    }
}

/** Draws the surrounding scene into this light's own cubemap */
void LegacyCubeLightState::Render( bool forceUpdate ) {
    if ( !m_Light.IsReady() )
        return;
    if ( !m_DepthCubemap )
        return;

    VobLightInfo* info = m_Light.GetLightInfo();
    const int shadowMode = PointShadowPolicy::CurrentShadowMode( info );
    if ( m_Light.GetLastShadowMode() != shadowMode ) {
        m_Light.HandleShadowModeChange( shadowMode );
        // Only PLS_UPDATE_DYNAMIC composites out of an aside cube; every other mode renders straight in.
        if ( shadowMode != GothicRendererSettings::PLS_UPDATE_DYNAMIC ) {
            ReleaseStaticAsideShadowMap();
        }
    }

    XMFLOAT3 vobPos = info->Vob->GetPositionWorld();
    const bool moved = !PointShadowPolicy::PositionEqualEps( m_Light.GetLastUpdatePosition(), vobPos );

    if ( moved ) {
        // Position changed, refresh our caches and invalidate the worldcache
        m_Light.ClearCasterCaches();
        m_Light.DropStaticBake( PLR_LIGHT_MOVED );
    }

    if ( shadowMode == GothicRendererSettings::PLS_STATIC_ONLY && !moved
        && m_Light.IsStaticShadowReady() && !m_Light.NotYetDrawn() ) {
        return;
    }

    if ( !m_Light.NeedsUpdate() && !m_Light.WantsUpdate() ) {
        if ( !forceUpdate )
            return; // Don't update when we don't need to
    }

    CubeRenderScope scope( info, m_Light.GetShadowRange() );
    m_Light.NoteDebugCubePlanes( scope.ZNear(), scope.ZFar() );
    RenderFullCubemap( scope );
    m_Light.NoteRendered( vobPos );
}

/** Renders all cubemap faces at once, using the geometry shader */
void LegacyCubeLightState::RenderFullCubemap( const CubeRenderScope& scope ) {
    D3D11GraphicsEngine* engine = AsD3D11Engine( Engine::GraphicsEngine );
    auto _ = engine->RecordGraphicsEvent( GE_NAME( "LegacyCube->RenderFullCubemap" ) );

    if ( !m_DepthCubemap ) {
        return;
    }

    VobLightInfo* info = m_Light.GetLightInfo();
    const int shadowMode = PointShadowPolicy::CurrentShadowMode( info );

    CasterPass pass;
    pass.Light = info;
    pass.Range = m_Light.GetShadowRange();
    pass.Target = m_DepthCubemap.get();
    pass.TargetIsSharedArray = false;   // its own cube, so the NVIDIA per-face fallback never applies
    pass.ClearDepth = true;
    pass.VobCache = &m_Light.VobCache;
    pass.MobCache = &m_Light.SkeletalVobCache;
    pass.WorldMeshCache = &m_Light.WorldMeshCache;

    if ( !m_Light.IsStaticShadowReady() && shadowMode == GothicRendererSettings::PLS_STATIC_ONLY ) {
        PointShadowCasters::QueueStatic( scope, pass );
        m_Light.MarkStaticBakeReady();
        return;
    }

    if ( shadowMode == GothicRendererSettings::PLS_UPDATE_DYNAMIC ) {
        DepthStencilPool* dsPool = engine->GetPfxRenderer()->GetDepthStencilPool();
        AcquireStaticAsideShadowMap( dsPool, m_Light.GetShadowMapResolution() );

        if ( !m_Light.IsStaticShadowReady() ) {
            if ( m_StaticDepthCubemap ) {
                CasterPass aside = pass;
                aside.Target = m_StaticDepthCubemap.get();
                PointShadowCasters::QueueStatic( scope, aside );
                m_Light.MarkStaticBakeReady();
            } else {
                // No aside buffer, we can't cache the static shadows.
                PointShadowCasters::QueueStatic( scope, pass );
            }
        }

        if ( m_StaticDepthCubemap ) {
            // The aside bake draws in phase 0, so the copy and the movers wait for it.
            PointShadowBatch::AtPhaseBoundary( [this] { CopyStaticAsideToCube(); } );
        }

        // A world-mesh-only light has no animated casters to add.
        if ( !m_Light.RestrictsCastersToWorld() ) {
            CasterPass animated = pass;
            animated.ClearDepth = false;   // composited on top of the static depth copied in at the boundary
            animated.Phase = 1;
            PointShadowCasters::QueueAnimated( scope, animated );
        }
        return;
    }

    if ( shadowMode == GothicRendererSettings::PLS_FULL ) {
        ReleaseStaticAsideShadowMap();
        m_Light.DropStaticBake( PLR_NO_CACHE );
        PointShadowCasters::QueueAll( scope, pass );
    }
}

// ---- LegacyCubeTechnique --------------------------------------------------------------------------------

std::unique_ptr<IPointShadowLightState> LegacyCubeTechnique::CreateLightState( D3D11PointLight& light ) {
    return std::make_unique<LegacyCubeLightState>( light, m_Info );
}

void LegacyCubeTechnique::BindPerLightSampling( D3D11PointLight& light ) {
    if ( LegacyCubeLightState* state = light.AsLegacy() ) state->BindForSampling();
}

bool LegacyCubeTechnique::ProvidesShadowFor( const D3D11PointLight& light ) const {
    LegacyCubeLightState* state = light.AsLegacy();
    return state && state->HasCube();
}

void LegacyCubeTechnique::OnLightVobRemoved( const zCVob* lightVob ) {
    // Take out of the shadowupdate queue - nothing is going to render it now.
    std::erase_if( m_UpdateQueue, [lightVob]( const VobLightInfo* li ) { return li->Vob == lightVob; } );
}

/** Tiled lighting off: no shared cube arrays and so no slots to select from. Every light owns an unbounded
    DepthStencilPool cubemap instead, gated by distance and drained through a small per-frame budget. */
XRESULT LegacyCubeTechnique::DrawShadows( std::vector<VobLightInfo*>& lights ) {
    auto& settings = Engine::GAPI->GetRendererState().RendererSettings;
    auto graphicsEngine = AsD3D11Engine( Engine::GraphicsEngine );

    // Never on the first absent frame, or a blinking light could never finish a bake that sticks.
    constexpr int kPointLightSlotRetentionFrames = 120;
    for ( auto& it : Engine::GAPI->VobLightMap ) {
        if ( !it.second->LightShadowBuffers ) continue;
        if ( D3D11PointLight* pl = dynamic_cast<D3D11PointLight*>( it.second->LightShadowBuffers.get() ) ) {
            LegacyCubeLightState* state = pl->AsLegacy();
            if ( !state ) continue;
            const bool visible = it.second->Vob->IsEnabled() && it.second->VisibleInFrame;
            if ( state->NoteAbsence( visible, kPointLightSlotRetentionFrames ) ) {
                state->ReleaseResources();
            }
        }
    }

    if ( settings.EnablePointlightShadows <= 0 ) return XR_SUCCESS;

    auto _ = graphicsEngine->RecordGraphicsEvent( GE_NAME( "DrawPointlightShadows" ) );

    static const XMVECTORF32 xmFltMax = { { { FLT_MAX, FLT_MAX, FLT_MAX, FLT_MAX } } };
    graphicsEngine->SetDefaultStates();

    const XMVECTOR vPlayerPosition =
        Engine::GAPI->GetPlayerVob() != nullptr
        ? Engine::GAPI->GetPlayerVob()->GetPositionWorldXM()
        : xmFltMax;

    const bool partialShadowUpdate = settings.PartialDynamicShadowUpdates;
    const bool staticOnlyMode = settings.EnablePointlightShadows == GothicRendererSettings::PLS_STATIC_ONLY;

    static std::vector<std::pair<float, VobLightInfo*>> importantUpdates;
    importantUpdates.clear();

    DepthStencilPool* dsPool = graphicsEngine->GetPfxRenderer()->GetDepthStencilPool();

    auto classifyLight = [&]( VobLightInfo* light, D3D11PointLight* pl, float distSq ) {
        if ( !pl->IsInited() ) return;
        if ( pl->NeedsUpdate() || light->UpdateShadows ) {
            importantUpdates.emplace_back( distSq, light );
        } else if ( partialShadowUpdate && !staticOnlyMode ) {
            if ( std::find( m_UpdateQueue.begin(), m_UpdateQueue.end(), light ) == m_UpdateQueue.end() ) {
                m_UpdateQueue.emplace_back( light );
            }
        } else if ( staticOnlyMode ) {
            auto queued = std::find( m_UpdateQueue.begin(), m_UpdateQueue.end(), light );
            if ( queued != m_UpdateQueue.end() ) {
                m_UpdateQueue.erase( queued );
            }
        }
    };

    // Drops a light out of both update paths - nothing is going to re-render its cube this frame.
    auto dequeueLight = [&]( VobLightInfo* light ) {
        auto it = std::find( m_UpdateQueue.begin(), m_UpdateQueue.end(), light );
        if ( it != m_UpdateQueue.end() ) {
            m_UpdateQueue.erase( it );
        }
        auto importantIt = std::find_if( importantUpdates.begin(), importantUpdates.end(),
            [&]( const auto& entry ) { return entry.second == light; } );
        if ( importantIt != importantUpdates.end() ) {
            importantUpdates.erase( importantIt );
        }
    };

    struct LegacyAcquire { VobLightInfo* light; D3D11PointLight* pl; float distSq; };
    static std::vector<LegacyAcquire> legacyAcquires;
    legacyAcquires.clear();

    for ( auto const& light : lights ) {
        if ( !light->Vob->IsEnabled() || !light->VisibleInFrame ) {
            continue;
        }
        // Create shadowmap in case we should have one but haven't got it yet
        if ( !light->LightShadowBuffers && light->UpdateShadows ) {
            BaseShadowedPointLight* bpl = nullptr;
            graphicsEngine->CreateShadowedPointLight( &bpl, light, /*dynamic light*/ true );
            static bool s_createReported = false;
            LightingLog::RequireOnce( bpl, s_createReported,
                "D3D11PointLight for a shadow-casting light (CreateShadowedPointLight returned nothing)" );
            light->LightShadowBuffers.reset( bpl );
        }

        D3D11PointLight* pl = dynamic_cast<D3D11PointLight*>( light->LightShadowBuffers.get() );
        if ( !pl ) continue;
        pl->EnsureState();
        LegacyCubeLightState* state = pl->AsLegacy();
        if ( !state ) continue;

        // Quantized and grow-only, so DoAnimation re-animating the range cannot re-bake the cube. The
        // selector owns this on the tiled path; here the light tracks it itself.
        constexpr float kShadowRangeQuantum = 128.0f;
        const float range = light->Vob->GetLightRange();
        pl->SetShadowRange( std::ceil( range / kShadowRangeQuantum ) * kShadowRangeQuantum );

        const float d = XMVectorGetX( XMVector3LengthSq( light->Vob->GetPositionWorldXM() - vPlayerPosition ) );
        const float distVeryCloseSq = (range * 0.8f) * (range * 0.8f);
        if ( d < distVeryCloseSq && !staticOnlyMode ) {
            light->UpdateShadows = true;
        }

        // range*9 alone puts a candle's horizon at ~13 m, which the range clamp reads as switched OFF.
        constexpr float kMinShadowDist = 3000.0f;   // Gothic world units (~100 = 1 m)
        const float maxShadowDist = std::max( pl->GetShadowRange() * 9.0f, kMinShadowDist );
        if ( d < maxShadowDist * maxShadowDist ) {
            if ( !state->HasCube() || pl->GetShadowMapResolution() != SHADOW_CUBE_SIZE ) {
                legacyAcquires.push_back( { light, pl, d } );
                continue;
            }
            classifyLight( light, pl, d );
        } else if ( state->HasAnyShadowMap() ) {
            state->ReleaseResources();
            dequeueLight( light );
        }
    }

    std::sort( legacyAcquires.begin(), legacyAcquires.end(), []( const LegacyAcquire& a, const LegacyAcquire& b ) {
        return a.distSq < b.distSq;
    } );
    for ( auto& c : legacyAcquires ) {
        LegacyCubeLightState* state = c.pl->AsLegacy();
        state->ReleaseResources();
        state->AcquireShadowMap( dsPool, SHADOW_CUBE_SIZE );
        c.light->UpdateShadows = true;
        classifyLight( c.light, c.pl, c.distSq );
    }
    // No fixed pools to report on here; zero Max hides the row entirely.
    auto& info = Engine::GAPI->GetRendererState().RendererInfo;
    info.PointLightSlotsMax = 0;
    info.PointLightStaticSlotsMax = 0;
    info.PointLightSlotsStarved = 0;

    // Render the immediate priority lights - but never more than a handful in one frame; overflow drains
    // through the round-robin below.
    std::sort( importantUpdates.begin(), importantUpdates.end(), []( const auto& a, const auto& b ) {
        return a.first < b.first;
    } );

    constexpr int maxImportantUpdates = 8;
    int importantDone = 0;
    for ( auto const& [distSq, importantUpdate] : importantUpdates ) {
        if ( importantDone >= maxImportantUpdates ) {
            if ( std::find( m_UpdateQueue.begin(), m_UpdateQueue.end(), importantUpdate ) == m_UpdateQueue.end() ) {
                m_UpdateQueue.emplace_back( importantUpdate );
            }
            continue;
        }

        auto* pl = static_cast<D3D11PointLight*>( importantUpdate->LightShadowBuffers.get() );
        if ( LegacyCubeLightState* state = pl->AsLegacy() ) state->Render( importantUpdate->UpdateShadows );
        importantUpdate->UpdateShadows = false;
        importantDone++;
    }

    // Process Background Queue (Round-Robin)
    constexpr int kBackgroundUpdateBudget = 2;
    int updateBudget = kBackgroundUpdateBudget;

    while ( !m_UpdateQueue.empty() && updateBudget > 0 ) {
        auto light = m_UpdateQueue.front();
        m_UpdateQueue.pop_front();

        if ( !light ) continue;

        D3D11PointLight* l = static_cast<D3D11PointLight*>( light->LightShadowBuffers.get() );
        if ( !l ) continue;
        LegacyCubeLightState* state = l->AsLegacy();
        if ( !state ) continue;

        if ( staticOnlyMode && l->IsStaticShadowReady() && !l->NeedsUpdate() ) {
            light->UpdateShadows = false;
            continue;
        }
        bool force = light->UpdateShadows;
        light->UpdateShadows = false;

        // FORCE the render! It waited in line for its turn, it must draw.
        state->Render( force );
        graphicsEngine->DebugPointlight = l;

        --updateBudget;
    }

    return XR_SUCCESS;
}
