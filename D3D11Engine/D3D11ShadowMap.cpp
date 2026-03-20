#include "D3D11ShadowMap.h"
#include <algorithm>
#include <cmath>
#include <DirectXMath.h>

// TODO: Remove circular dependencies
#include "D3D11Effect.h"
#include "D3D11GShader.h"
#include "D3D11PfxRenderer.h"
#include "D3D11ShaderManager.h"
#include "D3D11GraphicsEngine.h"
#include "zCCamera.h"
#include "zCVob.h"
#include "oCGame.h"
#include "GMesh.h"
#include "zCVobLight.h"
#include "zCBspTree.h"
// ^---------------------------------

using namespace DirectX;

extern bool FeatureLevel10Compatibility;

const float NUM_FRAME_SHADOW_UPDATES = 2;  // Fraction of lights to update per frame
const int NUM_MIN_FRAME_SHADOW_UPDATES = 4;  // Minimum lights to update per frame
const int MAX_IMPORTANT_LIGHT_UPDATES = 1;

void CalculateTemporalInterpolatedPosition(
    const XMVECTOR currentDir,
    XMVECTOR& previousDir,
    XMVECTOR& outDir,
    float frequency ) {
    // Calculate interpolation factor based on SmoothShadowFrequency
        // Higher frequency = faster updates = less smoothing
        // Lower frequency = slower updates = more smoothing (less flickering)
        // The frequency is inverted to get a blend factor: lower frequency = more blending

    // Blend factor: at frequency 500 (default), we want moderate smoothing
    // At frequency 100, we want heavy smoothing (slow updates)
    // At frequency 2000+, we want minimal smoothing (fast updates)
    // Using an exponential-ish curve for better control
    const float blendFactor = std::clamp( frequency / 10000.0f, 0.001f, 0.5f );

    // Smoothly interpolate from previous direction to current direction
    // This creates gradual shadow movement instead of discrete jumps
    XMVECTOR dir = XMVectorLerp( previousDir, currentDir, blendFactor );
    dir = XMVector3Normalize( dir );

    // Update the stored previous direction for next frame
    previousDir = dir;

    // Additionally apply quantization for sub-texel stability
    // This snaps the direction to discrete steps to prevent micro-flickering
    XMVECTOR scale = XMVectorReplicate( frequency );
    dir = XMVectorDivide(
        _mm_cvtepi32_ps( _mm_cvtps_epi32( XMVectorMultiply( dir, scale ) ) ),
        scale
    );
    outDir = XMVector3Normalize( dir );
}

static void CalculateCascadeMatrices(
    CameraReplacement& outCR,
    const std::vector<float>& splits,
    size_t cascadeIdx,
    size_t numCascades,
    float farPlane,
    FXMVECTOR lightPos,
    FXMVECTOR lookAt,
    FXMVECTOR upDir,
    GXMVECTOR shadowCameraPos,
    UINT shadowMapSize )
{
    // Cascade-spezifische Größe basierend auf Split-Verhältnis
    float splitRatio = splits[cascadeIdx + 1] / splits[numCascades];
    float cascadeSize = farPlane * std::sqrt( splitRatio );
    // cascadeSize = std::max( cascadeSize, 500.0f );

    // Round cascade size to fixed increments to prevent floating-point variations
    // This ensures the shadow map covers the same world-space area consistently
    constexpr float sizeQuantization = 64.0f;
    cascadeSize = std::ceil( cascadeSize / sizeQuantization ) * sizeQuantization;

    // Berechne View-Matrix für diese Cascade
    XMMATRIX lightView = XMMatrixLookAtLH( lightPos, lookAt, upDir );

    // *** TEXEL SNAPPING ***
    // Berechne die Größe eines Texels in World-Space
    float texelSize = cascadeSize / static_cast<float>(shadowMapSize);

    // Use a slightly larger texel size for snapping to reduce edge swimming
    float snapSize = texelSize// * 2.0f
        ;

    // Transformiere die Shadow-Kamera-Position in Light-Space
    XMVECTOR lightSpaceOrigin = XMVector3Transform( shadowCameraPos, lightView );
    XMFLOAT3 lightSpaceOriginF;
    XMStoreFloat3( &lightSpaceOriginF, lightSpaceOrigin );

    // Snappe auf Texel-Grenzen (using larger snap size for stability)
    lightSpaceOriginF.x = std::floor( lightSpaceOriginF.x / snapSize ) * snapSize;
    lightSpaceOriginF.y = std::floor( lightSpaceOriginF.y / snapSize ) * snapSize;

    // Berechne den Offset und wende ihn auf die View-Matrix an
    XMVECTOR snappedOrigin = XMLoadFloat3( &lightSpaceOriginF );
    XMVECTOR originalOrigin = XMVector3Transform( shadowCameraPos, lightView );
    XMVECTOR snapOffset = XMVectorSubtract( snappedOrigin, originalOrigin );

    // Erstelle Offset-Matrix
    XMFLOAT3 snapOffsetF;
    XMStoreFloat3( &snapOffsetF, snapOffset );
    XMMATRIX offsetMatrix = XMMatrixTranslation( snapOffsetF.x, snapOffsetF.y, 0.0f );

    // Kombiniere View mit Offset
    XMMATRIX snappedLightView = XMMatrixMultiply( lightView, offsetMatrix );

    float cullingMargin = texelSize * 2.0f;

    const XMMATRIX crViewRepl = XMMatrixTranspose( snappedLightView );
    const XMMATRIX crProjRepl = XMMatrixTranspose( XMMatrixOrthographicLH(
        cascadeSize, cascadeSize, 1.0f, 20000.f ) );

    XMStoreFloat4x4( &outCR.ViewReplacement, crViewRepl );
    XMStoreFloat4x4( &outCR.ProjectionReplacement, crProjRepl );
    XMStoreFloat3( &outCR.PositionReplacement, lightPos );
    XMStoreFloat3( &outCR.LookAtReplacement, lookAt );

    outCR.frustum.BuildOrthographic( snappedLightView,
        // ensure some additional margin
        // due to blending cascades in PS_DS_AtmosphericScattering.hlsl->GetCascadeUVAndBounds(..)
        // else we might miss some shadows due to frustum culling
        // e.g. not visible in c0 but would be in c1, due to blending,
        // c1 won't use it's "full" color, but rather blend in slowly
        // causing pop-in
        cascadeSize + cullingMargin,
        cascadeSize + cullingMargin,
        1.0f,
        20000.f,
        Engine::GAPI->GetRendererState().RendererSettings.DebugSettings.ShadowCascades.ExtendBack,
        Engine::GAPI->GetRendererState().RendererSettings.DebugSettings.ShadowCascades.ExtendFront,
        Engine::GAPI->GetRendererState().RendererSettings.DebugSettings.ShadowCascades.ExtendSide );
}

D3D11ShadowMap::D3D11ShadowMap() {
    
}

D3D11ShadowMap::~D3D11ShadowMap() {}

void D3D11ShadowMap::Init( Microsoft::WRL::ComPtr<ID3D11Device1>& device, Microsoft::WRL::ComPtr<ID3D11DeviceContext1>& context, int size ) {
    m_device = device;
    m_context = context;

    int s = std::min<int>( std::max<int>( size, 512 ), (FeatureLevel10Compatibility ? 8192 : 16384) );

    // Create sampler
    D3D11_SAMPLER_DESC samplerDesc = {};
    samplerDesc.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    samplerDesc.MipLODBias = 0;
    samplerDesc.MaxAnisotropy = 1;
    samplerDesc.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
    samplerDesc.MinLOD = -FLT_MAX;
    samplerDesc.MaxLOD = FLT_MAX;

    HRESULT hr;
    LE( m_device->CreateSamplerState( &samplerDesc, m_shadowmapSampler.GetAddressOf() ) );
    SetDebugName( m_shadowmapSampler.Get(), "ShadowmapSamplerState" );

    // Dummy cube RT used for fallback to satisfy pixel shader runs that expect a RTV bound
    m_dummyCubeRT = std::make_unique<RenderToTextureBuffer>( m_device.Get(), POINTLIGHT_SHADOWMAP_SIZE, POINTLIGHT_SHADOWMAP_SIZE, DXGI_FORMAT_ENGINE_DEFAULT, nullptr, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, 1, 6 );

    // Initialize the cascaded shadow map
    m_cascadedShadowMap = std::make_unique<D3D11CascadedShadowMapBuffer>();
    m_cascadedShadowMap->Init( m_device, s, MAX_CSM_CASCADES );

    for ( int i = 0; i < MAX_CSM_CASCADES; ++i ) {
        m_RenderQueues[i] = std::make_unique<D3D11RenderQueue>( device.Get(), context.Get() );
    }

    Resize( s );
}

void D3D11ShadowMap::Resize( int size ) {

    if ( !m_device ) return;

    int s = std::min<int>( std::max<int>( size, 512 ), (FeatureLevel10Compatibility ? 8192 : 16384) );

    // Resize the cascaded shadow map
    if ( m_cascadedShadowMap ) {
        m_cascadedShadowMap->Resize( s );
    }
}

void D3D11ShadowMap::BindToPixelShader( ID3D11DeviceContext1* context, UINT slot ) {
    // Bind the cascaded shadow map (Texture2DArray)
    if ( m_cascadedShadowMap ) {
        m_cascadedShadowMap->BindToPixelShader( context, slot );
    }
}

void D3D11ShadowMap::BindSampler( ID3D11DeviceContext1* context, UINT slot ) {
    if ( m_shadowmapSampler ) context->PSSetSamplers( slot, 1, m_shadowmapSampler.GetAddressOf() );
}

XRESULT D3D11ShadowMap::PrepareRender()
{
    const XMVECTOR cameraPositionXm = Engine::GAPI->GetCameraPositionXM();
    XMFLOAT3 cameraPosition;
    XMStoreFloat3( &cameraPosition, cameraPositionXm );

    auto& settings = Engine::GAPI->GetRendererState().RendererSettings;

    // ********************************
    // Cascade Shadow Map Rendering (Simple Sequential Version)
    // ********************************

    zCCamera* camera = (zCCamera*)oCGame::GetGame()->_zCSession_camera;
    if ( !camera ) {
        return XR_SUCCESS;
    }
    camera->Activate();

    const float nearPlane = std::max( 1.0f, camera->GetNearPlane() );
    // Clamp far plane to avoid extreme shadow distances
    // 64000 (default for worldsize 4) * 0.6 = 38400
    // this looked good ough in testing, without many popping artifacts
    const float baseFarPlane = std::min( camera->GetFarPlane(), 38400.0f );

    // WorldShadowRangeScale als Multiplikator für die Schattenreichweite
    const float shadowRangeScale = settings.WorldShadowRangeScale;
    const float farPlane = baseFarPlane * std::max( 0.1f, shadowRangeScale );
    int numCascades = settings.NumShadowCascades;
    if ( numCascades > MAX_CSM_CASCADES || numCascades < 1 ) {
        numCascades = std::clamp( numCascades, 1, MAX_CSM_CASCADES );
        settings.NumShadowCascades = numCascades;
    }

    std::vector<float> splits;
    if ( settings.DebugSettings.ShadowCascades.Lambda > 0.0001f
        || settings.DebugSettings.ShadowCascades.Bias > 0.0001f ) {

        splits = ComputeCascadeSplits( nearPlane, farPlane, numCascades,
                                                         settings.DebugSettings.ShadowCascades.Lambda,
                                                         settings.DebugSettings.ShadowCascades.Bias );
    } else {
        splits = ComputeCascadeSplits( nearPlane, farPlane, numCascades, lambdaBiasTable[numCascades].lambda, lambdaBiasTable[numCascades].bias );
    }

    splits[numCascades] = farPlane; // Let the last cascade reach the full far plane

    m_CascadeSplits.clear();
    m_CascadeSplits.insert( m_CascadeSplits.begin(), splits.begin(), splits.end() );

    // Get current light direction from atmosphere
    XMVECTOR currentDir = XMLoadFloat3( Engine::GAPI->GetSky()->GetAtmosphereCB().AC_LightPos.toXMFLOAT3() );
    currentDir = XMVector3Normalize( currentDir );

    // *** TEMPORAL SMOOTHING FOR LIGHT DIRECTION ***
    // Use static variables to maintain state across frames for smooth shadow transitions

    static struct alignas(16) {
        XMVECTOR PreviousLightDir;
        XMVECTOR OldPosition;
        XMVECTOR LightDir;
        XMVECTOR Position;
        bool initialized;
    } lastCascadeData = {};

    static struct {
        size_t frameCount;
        std::array<CameraReplacement, MAX_CSM_CASCADES> PreviousCascadeCRs;
    } perFrameCascadeData = {};

    static XMVECTOR s_previousLightDir = currentDir;
    static bool s_lightDirInitialized = false;

    XMVECTOR dir;

    if ( settings.SmoothShadowCameraUpdate ) {
        // Initialize on first frame
        if ( !s_lightDirInitialized ) {
            s_previousLightDir = currentDir;
            s_lightDirInitialized = true;
        }

        CalculateTemporalInterpolatedPosition(
            currentDir,
            s_previousLightDir,
            dir,
            std::max( 1.0f, settings.SmoothShadowFrequency ) );
    } else {
        dir = currentDir;
        s_previousLightDir = currentDir;
        s_lightDirInitialized = true;
    }

    if ( !lastCascadeData.initialized ) {
        lastCascadeData.PreviousLightDir = currentDir;
        lastCascadeData.initialized = true;
    }

    CalculateTemporalInterpolatedPosition(
        currentDir,
        lastCascadeData.PreviousLightDir,
        lastCascadeData.LightDir,
        500.0f );

    static XMVECTOR oldP = XMVectorZero();
    XMVECTOR WorldShadowCP;
    // Update position
    // Try to update only if the camera went 200 units away from the last position
    // This prevents "shaking" when the player is strafing or moving just a tiny bit
    float len;
    XMStoreFloat( &len, XMVector3LengthSq( oldP - cameraPositionXm ) );
    constexpr float distSq = 64.f * 64.f;
    if ( (len < distSq) ) {
        WorldShadowCP = oldP;
    } else {
        oldP = cameraPositionXm;
        WorldShadowCP = oldP;
    }
    XMStoreFloat3( &m_WorldShadowPos, WorldShadowCP );

    XMStoreFloat( &len, XMVector3LengthSq( lastCascadeData.OldPosition - cameraPositionXm ) );
    // for the last cascade, we snap greater distances to avoid shimmering when moving
    if ( (len < (160.f * 160.f)) ) {
        lastCascadeData.Position = lastCascadeData.OldPosition;
    } else {
        lastCascadeData.OldPosition = cameraPositionXm;
        lastCascadeData.Position = cameraPositionXm;
    }

    // Indoor check
    static zTBspMode lastBspMode = zBSP_MODE_OUTDOOR;

    // Array für alle Cascade-Matrizen
    bool isOutdoor = Engine::GAPI->GetLoadedWorldInfo()->BspTree->GetBspTreeMode() == zBSP_MODE_OUTDOOR;

    const FXMVECTOR p = WorldShadowCP + dir * 10000.0f;
    const FXMVECTOR lookAt = WorldShadowCP;

    const XMVECTOR lastCascadeP = lastCascadeData.Position + lastCascadeData.LightDir * 10000.0f;
    const XMVECTOR lastCascadeLookAt = lastCascadeData.Position;

    static const XMVECTORF32 c_XM_Up = { { { 0, 1, 0, 0 } } };

    if ( !isOutdoor ) {
        if ( settings.EnableShadows && lastBspMode == zBSP_MODE_OUTDOOR ) {
            // Clear all cascade DSVs
            for ( size_t cascadeIdx = 0; cascadeIdx < MAX_CSM_CASCADES; ++cascadeIdx ) {
                if ( auto dsv = GetCascadeDSV( static_cast<UINT>( cascadeIdx ) ) ) {
                    m_context->ClearDepthStencilView( dsv, D3D11_CLEAR_DEPTH, 0.0f, 0 );
                }
            }
            lastBspMode = zBSP_MODE_INDOOR;
        }

        // Setze Default für Indoor
        for ( int i = 0; i < numCascades; ++i ) {
            if ( numCascades > 1 && i == numCascades - 1 ) {
                const auto p = lastCascadeP;
                const auto lookAt = lastCascadeLookAt;

                XMStoreFloat4x4( &m_CascadeCRs[i].ViewReplacement, XMMatrixTranspose( XMMatrixLookAtLH( p, lookAt, c_XM_Up ) ) );
                XMStoreFloat4x4( &m_CascadeCRs[i].ProjectionReplacement, XMMatrixTranspose( XMMatrixOrthographicLH(
                    farPlane, farPlane, 1.0f, 20000.f ) ) );
                XMStoreFloat3( &m_CascadeCRs[i].PositionReplacement, p );
                XMStoreFloat3( &m_CascadeCRs[i].LookAtReplacement, lookAt );
            } else {
                XMStoreFloat4x4( &m_CascadeCRs[i].ViewReplacement, XMMatrixTranspose( XMMatrixLookAtLH( p, lookAt, c_XM_Up ) ) );
                XMStoreFloat4x4( &m_CascadeCRs[i].ProjectionReplacement, XMMatrixTranspose( XMMatrixOrthographicLH(
                    farPlane, farPlane, 1.0f, 20000.f ) ) );
                XMStoreFloat3( &m_CascadeCRs[i].PositionReplacement, p );
                XMStoreFloat3( &m_CascadeCRs[i].LookAtReplacement, lookAt );
            }
        }
    } else {
        lastBspMode = zBSP_MODE_OUTDOOR;

        // Increment frame counter for temporal cascade updates
        perFrameCascadeData.frameCount++;
        bool lazyCascadeUpdate = settings.DebugSettings.ShadowCascades.LazyCascadeUpdate;

        for ( int cascadeIdx = 0; cascadeIdx < numCascades; ++cascadeIdx ) {
            // pre-calculate all cascade matrices, to be able to frustum-cull anything that is not in this or the next cascade.

            bool isLastCascade = (numCascades > 1 && cascadeIdx == numCascades - 1);

            bool shouldUpdateCascade = true;
            if ( lazyCascadeUpdate && cascadeIdx == 2 ) {
                // pre-last cascade updates every 2nd frame which is 30 FPS = 15 updates per second
                shouldUpdateCascade = (perFrameCascadeData.frameCount % 2) == 0;
            } else if ( lazyCascadeUpdate && cascadeIdx == MAX_CSM_CASCADES-1 ) {
                // final cascade updates every 3rd frame which is 30 FPS = 10 updates per second
                // it covers the whole world, so this can help improve avg fps.
                shouldUpdateCascade = (perFrameCascadeData.frameCount % 3) == 0;
            }
            m_ShouldUpdateCascade[cascadeIdx] = shouldUpdateCascade;

            if ( shouldUpdateCascade || !m_CascadeCRs[cascadeIdx].frustum.IsValid()) {
                CalculateCascadeMatrices(
                    m_CascadeCRs[cascadeIdx],
                    splits,
                    cascadeIdx,
                    numCascades,
                    farPlane,
                    isLastCascade ? lastCascadeP : p,
                    isLastCascade ? lastCascadeLookAt : lookAt,
                    c_XM_Up,
                    isLastCascade ? lastCascadeData.Position : WorldShadowCP,
                    GetSizeX() );
            }
        }
    }

    // Collect all VOBs inside our shadow draw distance (last frustum)
    
    static std::vector<VobInfo*> potentialCasters;
    static std::vector<VobLightInfo*> _1;
    static std::vector<SkeletalVobInfo*> _2;
    potentialCasters.reserve(1024);
    potentialCasters.clear();

    {
        RndCullContext ctx;
        LegacyRenderQueueProxy q(potentialCasters, _1, _2);

        ctx.queue = &q;
        ctx.frustum = m_CascadeCRs[numCascades-1].frustum;
        ctx.cameraPosition = m_CascadeCRs[numCascades-1].PositionReplacement;
        ctx.stage = RenderStage::STAGE_DRAW_SHADOWS;
        ctx.drawDistances.OutdoorVobs = settings.ShadowDrawDistance;
        ctx.drawDistances.OutdoorVobsSmall = settings.ShadowDrawDistance;
        
        Engine::GAPI->CollectVisibleVobs( ctx );
    }
    
    auto invView = XMMatrixTranspose(XMLoadFloat4x4(&zCCamera::GetCamera()->GetTransformDX( zCCamera::ETransformType::TT_VIEW_INV )));
    auto camPos = invView.r[3];
    XMVECTOR camForward = XMVector3Normalize( invView.r[2]);
    
    for ( int i = 0; i < numCascades; ++i ) {
        m_RenderQueues[i]->Reset();
    }

    for (auto vob : potentialCasters ) {
        
        auto boundingSphere = Frustum::BSphereFromzTBBox3D(vob->Vob->GetBBox());
        if ( numCascades > 0 && m_CascadeCRs[0].frustum.Intersects( boundingSphere ) )
            m_RenderQueues[0]->GetVobs().push_back( vob );

        if ( numCascades > 1 && m_ShouldUpdateCascade[1] && m_CascadeCRs[1].frustum.Intersects( boundingSphere ) )
            m_RenderQueues[1]->GetVobs().push_back( vob );

        if ( numCascades > 2 && m_ShouldUpdateCascade[2] && m_CascadeCRs[2].frustum.Intersects( boundingSphere ) )
            m_RenderQueues[2]->GetVobs().push_back( vob );

        if ( numCascades > 3 && m_ShouldUpdateCascade[3] && m_CascadeCRs[3].frustum.Intersects( boundingSphere ) )
            m_RenderQueues[3]->GetVobs().push_back( vob );
    }

    return XR_SUCCESS;
}

// Computes cascade splits using a interpolation between uniform and logarithmic splits, additionally modified by a bias factor.
// Returns vector with (numCascades + 1) entries: [nearPlane, split1, split2, ..., farPlane]
std::vector<float> D3D11ShadowMap::ComputeCascadeSplits( float nearPlane, float farPlane, size_t numCascades, float lambda, float bias ) {
    if ( numCascades == 0 ) return { nearPlane, farPlane };

    lambda = std::clamp( lambda, 0.0f, 1.0f );

    std::vector<float> splits;
    splits.reserve( numCascades + 1 );
    splits.push_back( nearPlane );

    for ( size_t i = 1; i <= numCascades; ++i ) {
        // Calculate the linear fraction (0.0 to 1.0)
        float linearFraction = static_cast<float>(i) / static_cast<float>(numCascades);

        // Apply the BIAS (Power Function).
        // If bias > 1 (e.g., 2.0), this pushes values closer to 0, making near cascades smaller.
        float si = std::pow( linearFraction, bias );

        // apply logarithmic and uniform split calculations
        float logSplit = nearPlane * std::pow( farPlane / nearPlane, si );
        float uniformSplit = nearPlane + (farPlane - nearPlane) * si;

        // Interpolate
        float d = lambda * logSplit + (1.0f - lambda) * uniformSplit;

        splits.push_back( d );
    }

    return splits;
}

XRESULT D3D11ShadowMap::DrawPointlightShadows( std::vector<VobLightInfo*>& lights ) {
    auto& settings = Engine::GAPI->GetRendererState().RendererSettings;
    if (settings.EnablePointlightShadows <= 0) {
        return XR_SUCCESS;
    }
    
    auto graphicsEngine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    auto _ = graphicsEngine->RecordGraphicsEvent( L"DrawPointlightShadows" );

    static const XMVECTORF32 xmFltMax = { { { FLT_MAX, FLT_MAX, FLT_MAX, FLT_MAX } } };
    graphicsEngine->SetDefaultStates();

    // ********************************
    // Draw world shadows
    // ********************************
    const XMVECTOR cameraPositionXm = Engine::GAPI->GetCameraPositionXM();
    XMFLOAT3 cameraPosition;
    XMStoreFloat3( &cameraPosition, cameraPositionXm );
    FXMVECTOR vPlayerPosition =
        Engine::GAPI->GetPlayerVob() != nullptr
        ? Engine::GAPI->GetPlayerVob()->GetPositionWorldXM()
        : xmFltMax;

    bool partialShadowUpdate = settings.PartialDynamicShadowUpdates;

    // Draw pointlight shadows
    std::list<VobLightInfo*> importantUpdates;

    for ( auto const& light : lights ) {
        // Create shadowmap in case we should have one but haven't got it yet
        if ( !light->LightShadowBuffers && light->UpdateShadows ) {
            graphicsEngine->CreateShadowedPointLight( &light->LightShadowBuffers, light );
        }

        if ( light->LightShadowBuffers ) {
            // Check if this lights even needs an update
            bool needsUpdate = static_cast<D3D11PointLight*>(light->LightShadowBuffers)->NeedsUpdate();
            bool isInited = static_cast<D3D11PointLight*>(light->LightShadowBuffers)->IsInited();

            // Add to the updatequeue if it does
            if ( isInited && (needsUpdate || light->UpdateShadows) ) {
                // Always update the light if the light itself moved
                if ( partialShadowUpdate && !needsUpdate ) {
                    // Only add once. This list should never be very big, so it should
                    // be ok to search it like this This needs to be done to make sure a
                    // light will get updated only once and won't block the queue
                    if ( std::find( graphicsEngine->FrameShadowUpdateLights.begin(),
                                    graphicsEngine->FrameShadowUpdateLights.end(),
                                    light ) == graphicsEngine->FrameShadowUpdateLights.end() ) {
                        // Always render the closest light to the playervob, so the player
                        // doesn't flicker when moving
                        float d;
                        XMStoreFloat( &d, XMVector3LengthSq( light->Vob->GetPositionWorldXM() - vPlayerPosition ) );

                        float range = light->Vob->GetLightRange();
                        if ( d < range * range &&
                            importantUpdates.size() < MAX_IMPORTANT_LIGHT_UPDATES ) {
                            importantUpdates.emplace_back( light );
                        } else {
                            graphicsEngine->FrameShadowUpdateLights.emplace_back( light );
                        }
                    }
                } else {
                    // Always render the closest light to the playervob, so the player
                    // doesn't flicker when moving
                    float d;
                    XMStoreFloat( &d, XMVector3LengthSq( light->Vob->GetPositionWorldXM() - vPlayerPosition ) );

                    float range = light->Vob->GetLightRange() * 1.5f;

                    // If the engine said this light should be updated, then do so. If
                    // the light said this
                    if ( needsUpdate || d < range * range )
                        importantUpdates.emplace_back( light );
                }
            }
        }
    }

    // Render the closest light
    for ( auto const& importantUpdate : importantUpdates ) {
        static_cast<D3D11PointLight*>( importantUpdate->LightShadowBuffers )->RenderCubemap( importantUpdate->UpdateShadows );
        importantUpdate->UpdateShadows = false;
    }

    // Update only a fraction of lights, but at least some
    int n = std::max(
        (UINT)NUM_MIN_FRAME_SHADOW_UPDATES,
        (UINT)(graphicsEngine->FrameShadowUpdateLights.size() / NUM_FRAME_SHADOW_UPDATES) );
    while ( !graphicsEngine->FrameShadowUpdateLights.empty() ) {
        auto light = graphicsEngine->FrameShadowUpdateLights.front();
        if ( !light ) {
            graphicsEngine->FrameShadowUpdateLights.pop_front();
            continue;
        }
        D3D11PointLight* l = static_cast<D3D11PointLight*>(light->LightShadowBuffers);
        if ( !l ) {
            graphicsEngine->FrameShadowUpdateLights.pop_front();
            continue;
        }
        // Check if we have to force this light to update itself (NPCs moving around, for example)
        bool force = light->UpdateShadows;
        light->UpdateShadows = false;

        l->RenderCubemap( force );
        graphicsEngine->DebugPointlight = l;

        graphicsEngine->FrameShadowUpdateLights.pop_front();

        // Only update n lights
        n--;
        if ( n <= 0 ) break;
    }
    return XR_SUCCESS;
}

XRESULT D3D11ShadowMap::DrawWorldShadow( )
{
    auto graphicsEngine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    auto _ = graphicsEngine->RecordGraphicsEvent( L"DrawWorldShadow" );
    auto& settings = Engine::GAPI->GetRendererState().RendererSettings;
    
    int numCascades = settings.NumShadowCascades;
    bool isOutdoor = Engine::GAPI->GetLoadedWorldInfo()->BspTree->GetBspTreeMode() == zBSP_MODE_OUTDOOR;

    if ( isOutdoor ) {
        for ( int cascadeIdx = 0; cascadeIdx < numCascades; ++cascadeIdx ) {
            // only update every Nth frame for higher cascades to save performance
            bool shouldUpdateCascade = m_ShouldUpdateCascade[cascadeIdx];

            if ( !shouldUpdateCascade ) continue;

            // Render diese Cascade using the new CascadedShadowMap
            Engine::GAPI->SetCameraReplacementPtr( &m_CascadeCRs[cascadeIdx] );

            // Build render params
            RenderShadowmapsParams renderParams = {};
            renderParams.CameraPosition = m_WorldShadowPos;
            renderParams.Target = nullptr;
            renderParams.CullFront = true;
            renderParams.DontCull = false;
            renderParams.DSVOverwrite = GetCascadeDSV( static_cast<UINT>(cascadeIdx) );
            renderParams.DebugRTV = nullptr;
            renderParams.CascadeIndex = static_cast<int>(cascadeIdx);
            renderParams.CascadeSplits = m_CascadeSplits;
            renderParams.CascadeCameraReplacements = &m_CascadeCRs;

            RenderShadowmaps( renderParams );

            Engine::GAPI->SetCameraReplacementPtr( nullptr );
            m_RenderQueues[cascadeIdx]->Reset();
        }
    }

    // Restore gothics camera
    Engine::GAPI->SetCameraReplacementPtr( nullptr );
    
    return XR_SUCCESS;
}

XRESULT D3D11ShadowMap::DrawRainShadowmap() {
    // Draw rainmap, if raining
    if ( Engine::GAPI->GetSceneWetness() > 0.00001f ) {
        auto graphicsEngine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
        auto _ = graphicsEngine->RecordGraphicsEvent( L"DrawRainShadowmap" );

        graphicsEngine->Effects->DrawRainShadowmap();
    }
    return XR_SUCCESS;
}

XRESULT D3D11ShadowMap::DrawPointlightLights(
    std::vector<VobLightInfo*>& lights,
    RenderToTextureBuffer& color,
    RenderToTextureBuffer& normals,
    RenderToTextureBuffer& specular,    
    RenderToTextureBuffer& depthCopy    
    ) {
    auto graphicsEngine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    auto _ = graphicsEngine->RecordGraphicsEvent( L"DrawPointlightLights" );
    auto& settings = Engine::GAPI->GetRendererState().RendererSettings;

    XMMATRIX view = Engine::GAPI->GetViewMatrixXM();
    Engine::GAPI->SetViewTransformXM( view );
    view = XMMatrixTranspose( view );

    graphicsEngine->SetActiveVertexShader( "VS_ExPointLight" );
    graphicsEngine->SetActivePixelShader( "PS_DS_PointLight" );

    auto psPointLight = graphicsEngine->GetShaderManager().GetPShader( "PS_DS_PointLight" );
    auto psPointLightDynShadow = graphicsEngine->GetShaderManager().GetPShader( "PS_DS_PointLightDynShadow" );

    Engine::GAPI->GetRendererState().BlendState.SetAdditiveBlending();
    if ( settings.LimitLightIntesity ) {
        Engine::GAPI->GetRendererState().BlendState.BlendOp = GothicBlendStateInfo::BO_BLEND_OP_MAX;
    }
    Engine::GAPI->GetRendererState().BlendState.SetDirty();

    Engine::GAPI->GetRendererState().DepthState.DepthWriteEnabled = false;
    Engine::GAPI->GetRendererState().DepthState.SetDirty();

    Engine::GAPI->GetRendererState().RasterizerState.CullMode = GothicRasterizerStateInfo::CM_CULL_BACK;
    Engine::GAPI->GetRendererState().RasterizerState.SetDirty();

    graphicsEngine->SetupVS_ExMeshDrawCall();
    graphicsEngine->SetupVS_ExConstantBuffer();

    // Copy this, so we can access depth in the pixelshader and still use the buffer for culling
    graphicsEngine->CopyDepthStencil();

    // Set the main rendertarget
    m_context->OMSetRenderTargets( 1, graphicsEngine->GetHDRBackBuffer().GetRenderTargetView().GetAddressOf(), graphicsEngine->GetDepthBuffer()->GetDepthStencilView().Get() );
    
    DS_PointLightConstantBuffer plcb = {};

    {
        auto& proj = Engine::GAPI->GetProjectionMatrix();
        plcb.PL_ProjParams = float4( 1.0f / proj._11, 1.0f / proj._22, proj._43, proj._33 );
    }
    XMStoreFloat4x4( &plcb.PL_InvView, XMMatrixInverse( nullptr, XMLoadFloat4x4( &Engine::GAPI->GetRendererState().TransformState.TransformView ) ) );

    plcb.PL_ViewportSize = Engine::GraphicsEngine->GetResolution();

    color.BindToPixelShader( m_context.Get(), 0 );
    normals.BindToPixelShader( m_context.Get(), 1 );
    specular.BindToPixelShader( m_context.Get(), 7 );
    depthCopy.BindToPixelShader( m_context.Get(), 2 );

    // Draw all lights
    for ( auto const& light : lights ) {
        zCVobLight* vob = light->Vob;

        // Reset state from CollectVisibleVobs
        light->VisibleInRenderPass = false;

        if ( !vob->IsEnabled() ) continue;

        // Set right shader
        if ( settings.EnablePointlightShadows > 0 ) {
            if ( light->LightShadowBuffers && static_cast<D3D11PointLight*>(light->LightShadowBuffers)->IsInited() ) {
                if ( graphicsEngine->GetActivePS() != psPointLightDynShadow ) {
                    // Need to update shader for shadowed pointlight
                    graphicsEngine->SetActivePS( psPointLightDynShadow )->Apply();
                }
            } else if ( graphicsEngine->GetActivePS() != psPointLight ) {
                // Need to update shader for usual pointlight
                graphicsEngine->SetActivePS( psPointLight )->Apply();
            }
        }

        // Animate the light
        vob->DoAnimation();

        plcb.PL_Color = float4( vob->GetLightColor() );
        plcb.PL_Range = vob->GetLightRange();
        plcb.Pl_PositionWorld = vob->GetPositionWorld();
        plcb.PL_Outdoor = light->IsIndoorVob ? 0.0f : 1.0f;

        float dist;
        XMStoreFloat( &dist, XMVector3Length( XMLoadFloat3( plcb.Pl_PositionWorld.toXMFLOAT3() ) - Engine::GAPI->GetCameraPositionXM() ) );

        // Gradually fade in the lights
        if ( dist + plcb.PL_Range <
            settings.VisualFXDrawRadius ) {
            // float fadeStart =
            // settings.VisualFXDrawRadius -
            // plcb.PL_Range;
            float fadeEnd =
                settings.VisualFXDrawRadius;

            float fadeFactor = std::min(
                1.0f,
                std::max( 0.0f, ((fadeEnd - (dist + plcb.PL_Range)) / plcb.PL_Range) ) );
            plcb.PL_Color.x *= fadeFactor;
            plcb.PL_Color.y *= fadeFactor;
            plcb.PL_Color.z *= fadeFactor;
            // plcb.PL_Color.w *= fadeFactor;
        }

        // Make the lights a little bit brighter
        float lightFactor = 1.2f;

        plcb.PL_Color.x *= lightFactor;
        plcb.PL_Color.y *= lightFactor;
        plcb.PL_Color.z *= lightFactor;

        // Need that in view space
        FXMVECTOR Pl_PositionWorld = XMLoadFloat3( plcb.Pl_PositionWorld.toXMFLOAT3() );
        XMStoreFloat3( plcb.Pl_PositionView.toXMFLOAT3(),
            XMVector3TransformCoord( Pl_PositionWorld, view ) );

        XMStoreFloat3( plcb.PL_LightScreenPos.toXMFLOAT3(),
            XMVector3TransformCoord( Pl_PositionWorld, XMLoadFloat4x4( &Engine::GAPI->GetProjectionMatrix() ) ) );

        if ( dist < plcb.PL_Range ) {
            if ( Engine::GAPI->GetRendererState().DepthState.DepthBufferEnabled ) {
                Engine::GAPI->GetRendererState().DepthState.DepthBufferEnabled = false;
                Engine::GAPI->GetRendererState().RasterizerState.CullMode = GothicRasterizerStateInfo::CM_CULL_FRONT;
                Engine::GAPI->GetRendererState().DepthState.SetDirty();
                Engine::GAPI->GetRendererState().RasterizerState.SetDirty();
                graphicsEngine->UpdateRenderStates();
            }
        } else {
            if ( !Engine::GAPI->GetRendererState().DepthState.DepthBufferEnabled ) {
                Engine::GAPI->GetRendererState().DepthState.DepthBufferEnabled = true;
                Engine::GAPI->GetRendererState().RasterizerState.CullMode = GothicRasterizerStateInfo::CM_CULL_BACK;
                Engine::GAPI->GetRendererState().DepthState.SetDirty();
                Engine::GAPI->GetRendererState().RasterizerState.SetDirty();
                graphicsEngine->UpdateRenderStates();
            }
        }

        plcb.PL_LightScreenPos.x = plcb.PL_LightScreenPos.x / 2.0f + 0.5f;
        plcb.PL_LightScreenPos.y = plcb.PL_LightScreenPos.y / -2.0f + 0.5f;

        // Apply the constantbuffer to vs and PS
        graphicsEngine->GetActivePS()->GetConstantBuffer()[0]->UpdateBuffer( &plcb );
        graphicsEngine->GetActivePS()->GetConstantBuffer()[0]->BindToPixelShader( 0 );
        graphicsEngine->GetActivePS()->GetConstantBuffer()[0]->BindToVertexShader(
            1 );  // Bind this instead of the usual per-instance buffer

        if ( settings.EnablePointlightShadows > 0 ) {
            // Bind shadowmap, if possible
            if ( light->LightShadowBuffers )
                static_cast<D3D11PointLight*>(light->LightShadowBuffers)->OnRenderLight();
        }

        // Draw the mesh
        graphicsEngine->InverseUnitSphereMesh->DrawMesh();

        Engine::GAPI->GetRendererState().RendererInfo.FrameDrawnLights++;
    }
    
    return XR_SUCCESS;
}

XRESULT D3D11ShadowMap::DrawLighting( 
    std::vector<VobLightInfo*>& lights,
    RenderToTextureBuffer& color,
    RenderToTextureBuffer& normals,
    RenderToTextureBuffer& specular,    
    RenderToTextureBuffer& depthCopy) {
    auto graphicsEngine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    auto& settings = Engine::GAPI->GetRendererState().RendererSettings;

    graphicsEngine->SetDefaultStates();

    // Draw pointlight shadows
    DrawPointlightShadows(lights);

    DrawWorldShadow();

    graphicsEngine->SetDefaultStates();

    DrawRainShadowmap();

    Engine::GAPI->SetFarPlane(static_cast<float>(settings.SectionDrawRadius) * WORLD_SECTION_SIZE );
    
    DrawPointlightLights(lights, color, normals, specular, depthCopy);

    DrawWorldLights();

    m_context->OMSetRenderTargets( 1, graphicsEngine->GetHDRBackBuffer().GetRenderTargetView().GetAddressOf(),
        graphicsEngine->GetDepthBuffer()->GetDepthStencilView().Get() );

    return XR_SUCCESS;
}



/** Renders the shadowmaps for the sun */
void D3D11ShadowMap::RenderShadowmaps( const RenderShadowmapsParams& params ) {

    // We now assume that "target" always is something else than the world shadowmap
    UINT targetSize = !params.Target
        ? m_cascadedShadowMap->GetSize()
        : params.Target->GetSizeX();

    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> dsvOverwrite = params.DSVOverwrite;
    if ( params.Target && !dsvOverwrite.Get() ) dsvOverwrite = params.Target->GetDepthStencilView().Get();
    const bool isNotWorldShadowMap = params.Target != nullptr;

    // todo: remove this dependency at some point
    auto graphicsEngine = (D3D11GraphicsEngine*)Engine::GraphicsEngine;
    auto _ = graphicsEngine->RecordGraphicsEvent( L"RenderShadowmaps" );

    D3D11_VIEWPORT oldVP;
    UINT n = 1;
    m_context->RSGetViewports( &n, &oldVP );

    // Apply new viewport
    D3D11_VIEWPORT vp;
    vp.TopLeftX = 0;
    vp.TopLeftY = 0;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    vp.Width = static_cast<float>(targetSize);
    vp.Height = vp.Width;
    m_context->RSSetViewports( 1, &vp );

    // Set the rendering stage
    D3D11ENGINE_RENDER_STAGE oldStage = graphicsEngine->GetRenderingStage();
    graphicsEngine->SetRenderingStage( DES_SHADOWMAP );

    // Clear and Bind the shadowmap

    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
    m_context->PSSetShaderResources( 3, 1, srv.GetAddressOf() );

    if ( !params.DebugRTV.Get() ) {
        m_context->OMSetRenderTargets( 0, nullptr, dsvOverwrite.Get() );
        Engine::GAPI->GetRendererState().BlendState.ColorWritesEnabled = false;
    } else {
        m_context->OMSetRenderTargets( 1, params.DebugRTV.GetAddressOf(), dsvOverwrite.Get() );
        Engine::GAPI->GetRendererState().BlendState.ColorWritesEnabled = true;
    }
    Engine::GAPI->GetRendererState().BlendState.SetDirty();

    // Dont render shadows from the sun when it isn't on the sky
    if ( isNotWorldShadowMap ||
        (Engine::GAPI->GetSky()->GetAtmoshpereSettings().LightDirection.y >
            0 &&  // Only stop rendering if the sun is down on main-shadowmap
            // TODO: Take this out of here!
            Engine::GAPI->GetRendererState().RendererSettings.DrawShadowGeometry &&
            Engine::GAPI->GetRendererState().RendererSettings.EnableShadows) ) {
        m_context->ClearDepthStencilView( dsvOverwrite.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0 );

        // Draw the world mesh without textures        

        XMVECTOR cameraPosition = XMLoadFloat3( &params.CameraPosition );
        int timerLabelIndex = std::clamp(params.CascadeIndex, 0, MAX_CSM_CASCADES-1);
        static const char* timer_labels_cascades[MAX_CSM_CASCADES]
        {
            "Cascade 0",
            "Cascade 1",
            "Cascade 2",
            "Cascade 3",
        };
        auto _1 = START_TIMING(timer_labels_cascades[timerLabelIndex]);
        graphicsEngine->DrawWorldAroundForWorldShadow( cameraPosition, 2, params );

    } else {
        if ( Engine::GAPI->GetSky()->GetAtmoshpereSettings().LightDirection.y <= 0 ) {
            m_context->ClearDepthStencilView( dsvOverwrite.Get(), D3D11_CLEAR_DEPTH, 0.0f,
                0 );  // Always shadow in the night
        } else {
            m_context->ClearDepthStencilView(
                dsvOverwrite.Get(), D3D11_CLEAR_DEPTH, 1.0f,
                0 );  // Clear shadowmap when shadows not enabled
        }
    }

    // Restore state
    graphicsEngine->SetRenderingStage( oldStage );
    m_context->RSSetViewports( 1, &oldVP );

    Engine::GAPI->SetFarPlane(
        Engine::GAPI->GetRendererState().RendererSettings.SectionDrawRadius *
        WORLD_SECTION_SIZE );
}

XRESULT D3D11ShadowMap::DrawWorldLights()
{
    auto graphicsEngine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    auto _ = graphicsEngine->RecordGraphicsEvent( L"DrawWorldLights" );
    auto& settings = Engine::GAPI->GetRendererState().RendererSettings;

    Engine::GAPI->GetRendererState().BlendState.BlendOp = GothicBlendStateInfo::BO_BLEND_OP_ADD;
    Engine::GAPI->GetRendererState().BlendState.SetDirty();

    Engine::GAPI->GetRendererState().DepthState.DepthBufferCompareFunc = GothicDepthBufferStateInfo::CF_COMPARISON_ALWAYS;
    Engine::GAPI->GetRendererState().DepthState.SetDirty();

    Engine::GAPI->GetRendererState().RasterizerState.CullMode = GothicRasterizerStateInfo::CM_CULL_NONE;
    Engine::GAPI->GetRendererState().RasterizerState.SetDirty();

    // Modify light when raining
    float rain = Engine::GAPI->GetRainFXWeight();
    float wetness = Engine::GAPI->GetSceneWetness();

    XMMATRIX view = XMMatrixTranspose( Engine::GAPI->GetViewMatrixXM() );

    bool isSnow = oCGame::GetGame()
        && oCGame::GetGame()->_zCSession_world
        && oCGame::GetGame()->_zCSession_world->GetSkyControllerOutdoor()
        && oCGame::GetGame()->_zCSession_world->GetSkyControllerOutdoor()->GetWeatherType() == zTWEATHER_SNOW;

    // Switch global light shader when raining
    if ( wetness > 0.0f && !isSnow ) {
        // Same shader, just has a DEFINE set to enable rain-related effects
        graphicsEngine->SetActivePixelShader( "PS_DS_AtmosphericScattering_Rain" );
    } else {
        graphicsEngine->SetActivePixelShader( "PS_DS_AtmosphericScattering" );
    }

    graphicsEngine->SetActiveVertexShader( "VS_PFX" );

    graphicsEngine->SetupVS_ExMeshDrawCall();

    GSky* sky = Engine::GAPI->GetSky();
    graphicsEngine->GetActivePS()->GetConstantBuffer()[1]->UpdateBuffer( &sky->GetAtmosphereCB() );
    graphicsEngine->GetActivePS()->GetConstantBuffer()[1]->BindToPixelShader( 1 );

    auto& proj = Engine::GAPI->GetProjectionMatrix();
    DS_ScreenQuadConstantBuffer scb = {};
    scb.SQ_ProjParams = float4( 1.0f / proj._11, 1.0f / proj._22, proj._43, proj._33 );
    XMStoreFloat4x4( &scb.SQ_InvView, XMMatrixInverse( nullptr, XMLoadFloat4x4( &Engine::GAPI->GetRendererState().TransformState.TransformView ) ) );
    scb.SQ_View = Engine::GAPI->GetRendererState().TransformState.TransformView;

    static uint32_t frameCounter = 0;
    if ( proj._13 != 0 && proj._23 != 0) {
        // only when we have jitter in the frame
        scb.SQ_FrameIndex = frameCounter++;
    }

    XMStoreFloat3( scb.SQ_LightDirectionVS.toXMFLOAT3(),
        XMVector3TransformNormal( XMLoadFloat3( sky->GetAtmosphereCB().AC_LightPos.toXMFLOAT3() ), view ) );

    float3 sunColor =
        settings.SunLightColor;

    float sunStrength = Toolbox::lerp(
        settings.SunLightStrength,
        settings.RainSunLightStrength,
        std::min( 1.0f, rain * 2.0f ) );// Scale the darkening-factor faster here, so it
    // matches more with the increasing fog-density

    scb.SQ_LightColor = float4( sunColor.x, sunColor.y, sunColor.z, sunStrength );

    // CSM: Alle Cascade-Matrizen setzen

    for ( size_t cascadeIdx = 0; cascadeIdx < MAX_CSM_CASCADES; ++cascadeIdx ) {
        XMStoreFloat4x4( &scb.SQ_ShadowViewProj[cascadeIdx],
            XMLoadFloat4x4( &m_CascadeCRs[cascadeIdx].ProjectionReplacement ) *
                XMLoadFloat4x4( &m_CascadeCRs[cascadeIdx].ViewReplacement )
        );
    }

    scb.SQ_ShadowmapSize = static_cast<float>( this->GetSizeX() );

    // Get rain matrix
    
    XMStoreFloat4x4( &scb.SQ_RainViewProj,
        XMLoadFloat4x4( &graphicsEngine->Effects->GetRainShadowmapCameraRepl().ProjectionReplacement ) *
        XMLoadFloat4x4( &graphicsEngine->Effects->GetRainShadowmapCameraRepl().ViewReplacement )
    );

    scb.SQ_ShadowStrength = settings.ShadowStrength;
    scb.SQ_ShadowAOStrength = settings.ShadowAOStrength;
    scb.SQ_WorldAOStrength = settings.WorldAOStrength;
    scb.SQ_ShadowSoftness = settings.ShadowSoftness;
    scb.SQ_LightSize = 0.04f; // PCSS light size in shadow UV space

    // Modify lightsettings when indoor
    if ( auto bspTree = Engine::GAPI->GetLoadedWorldInfo()->BspTree )
        if ( bspTree->GetBspTreeMode() == zBSP_MODE_INDOOR ) {
            // TODO: fix caves in Gothic 1 being way too dark. Remove this workaround.
#if BUILD_GOTHIC_1_08k
            // Kirides: Nah, just make it dark enough.
            if ( Engine::GAPI->GetLoadedWorldInfo()->WorldName == "ORCTEMPEL" )
                scb.SQ_ShadowStrength = 0.15f;
            else
                scb.SQ_ShadowStrength = 0.3f;
#else
            // Turn off shadows
            scb.SQ_ShadowStrength = 0.0f;
#endif

            // Only use world AO
            scb.SQ_WorldAOStrength = 1.0f;
            // Darken the lights
            scb.SQ_LightColor = float4( 1, 1, 1, DEFAULT_INDOOR_VOB_AMBIENT.x );
        }

    graphicsEngine->GetActivePS()->GetConstantBuffer()[0]->UpdateBuffer( &scb );
    graphicsEngine->GetActivePS()->GetConstantBuffer()[0]->BindToPixelShader( 0 );

    PFXVS_ConstantBuffer vscb;
    vscb.PFXVS_ProjParams = scb.SQ_ProjParams;
    graphicsEngine->GetActiveVS()->GetConstantBuffer()[0]->UpdateBuffer( &vscb );
    graphicsEngine->GetActiveVS()->GetConstantBuffer()[0]->BindToVertexShader( 0 );

    // CSM: Bind the cascade array to a single slot (Texture2DArray)
    BindToPixelShader( m_context.Get(), TX_ShadowmapArray );

    if ( graphicsEngine->Effects->GetRainShadowmap() )
        graphicsEngine->Effects->GetRainShadowmap()->BindToPixelShader( m_context.Get(), TX_RainShadowmap );

    this->BindSampler( m_context.Get(), 2 );

    m_context->PSSetShaderResources( TX_ReflectionCube, 1, graphicsEngine->ReflectionCube2.GetAddressOf() );

    graphicsEngine->GetDistortionTexture()->BindToPixelShader( TX_Distortion );

    // CSM: Nur 1x rendern!
    graphicsEngine->GetPfxRenderer()->DrawFullScreenQuad();

    // Reset state
    static ID3D11ShaderResourceView* nullSrv[] = { nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr };
    m_context->PSSetShaderResources( 3, std::size( nullSrv ), nullSrv );

    return XR_SUCCESS;
}


/** Renders the shadowmaps for a pointlight */
void XM_CALLCONV D3D11ShadowMap::RenderShadowCube(
    FXMVECTOR position, float range,
    const RenderToDepthStencilBuffer& targetCube, Microsoft::WRL::ComPtr<ID3D11DepthStencilView> face,
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> debugRTV, bool cullFront, bool indoor, bool noNPCs,
    std::list<VobInfo*>* renderedVobs,
    std::list<SkeletalVobInfo*>* renderedMobs,
    std::map<MeshKey, WorldMeshInfo*, cmpMeshKey>* worldMeshCache ) {

    auto graphicsEngine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);

    D3D11_VIEWPORT oldVP;
    UINT n = 1;
    m_context->RSGetViewports( &n, &oldVP );

    // Apply new viewport
    D3D11_VIEWPORT vp;
    vp.TopLeftX = 0;
    vp.TopLeftY = 0;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    vp.Width = static_cast<float>(targetCube.GetSizeX());
    vp.Height = static_cast<float>(targetCube.GetSizeX());
    m_context->RSSetViewports( 1, &vp );

    bool useLayeredPath = false;
    if ( !face.Get() ) {
        if ( Engine::GAPI->GetRendererState().RendererSettings.DebugSettings.FeatureSet.UseLayeredRendering ) {
            useLayeredPath = true;
            face = targetCube.GetDepthStencilView().Get();

            // Set layered shader
            graphicsEngine->SetActiveVertexShader( "VS_ExLayered" );
        } else {
            // Set cubemap shader
            graphicsEngine->SetActiveGShader( "GS_Cubemap" );
            graphicsEngine->GetActiveGS().get()->Apply();
            face = targetCube.GetDepthStencilView().Get();

            graphicsEngine->SetActiveVertexShader( "VS_ExCube" );
        }
    }

    // Set the rendering stage
    D3D11ENGINE_RENDER_STAGE oldStage = graphicsEngine->GetRenderingStage();
    graphicsEngine->SetRenderingStage( DES_SHADOWMAP_CUBE );

    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
    m_context->PSSetShaderResources( 3, 1, srv.GetAddressOf() );

    if ( !debugRTV.Get() ) {
        m_context->OMSetRenderTargets( 0, nullptr, face.Get() );

        Engine::GAPI->GetRendererState().BlendState.ColorWritesEnabled =
            true;  // Should be false, but needs to be true for SV_Depth to work
        Engine::GAPI->GetRendererState().BlendState.SetDirty();
    } else {
        m_context->OMSetRenderTargets( 1, debugRTV.GetAddressOf(), face.Get() );

        Engine::GAPI->GetRendererState().BlendState.ColorWritesEnabled = true;
        Engine::GAPI->GetRendererState().BlendState.SetDirty();
    }

    // Always render shadowcube when dynamic shadows are enabled
    m_context->ClearDepthStencilView( face.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0 );

    // Draw the world mesh without textures
    if ( useLayeredPath ) {
        graphicsEngine->DrawWorldAround_Layered( position, range, cullFront, indoor, noNPCs, renderedVobs,
            renderedMobs, worldMeshCache );
    } else {
        graphicsEngine->DrawWorldAround( position, range, cullFront, indoor, noNPCs, renderedVobs,
            renderedMobs, worldMeshCache );
    }

    // Restore state
    graphicsEngine->SetRenderingStage( oldStage );
    m_context->RSSetViewports( 1, &oldVP );
    m_context->GSSetShader( nullptr, nullptr, 0 );
    graphicsEngine->SetActiveVertexShader( "VS_Ex" );

    Engine::GAPI->SetFarPlane(
        Engine::GAPI->GetRendererState().RendererSettings.SectionDrawRadius *
        WORLD_SECTION_SIZE );

    graphicsEngine->SetRenderingStage( DES_MAIN );
}
