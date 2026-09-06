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
#include "D3D11PipelineStateCache.h"
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
    dir = XMVectorMultiply( dir, scale );
    dir = XMVectorRound( dir );
    dir = XMVectorDivide( dir, scale );
    outDir = XMVector3Normalize( dir );
}

/// <summary>
/// Aligned to Bounding Sphere
/// </summary>
static void CalculateCascadeMatrices(
    CameraReplacement& outCR,
    const Frustum& playerFrustum,
    const std::vector<float>& splits,
    size_t cascadeIdx,
    size_t numCascades,
    float farPlane,
    FXMVECTOR lightPosOrig,
    FXMVECTOR lookAtOrig,
    FXMVECTOR upDirOrig,
    GXMVECTOR shadowCameraPosFallback,
    UINT shadowMapSize )
{
    XMVECTOR lightDir = XMVector3Normalize( XMVectorSubtract( lookAtOrig, lightPosOrig ) );

    XMVECTOR upDir = upDirOrig;
    if ( std::abs( XMVectorGetX( XMVector3Dot( lightDir, upDir ) ) ) > 0.999f ) {
        upDir = XMVectorSet( 0.0f, 0.0f, 1.0f, 0.0f );
    }

    XMVECTOR frustumCenter;

    float splitNear = splits[cascadeIdx];
    float splitFar = splits[cascadeIdx + 1];

    if ( !playerFrustum.IsValid() || !playerFrustum.SupportsCulling() ) {
        LogError() << "ShadowMap: Invalid Player Frustum!";
    }

    auto corners = playerFrustum.GetSliceCorners( splitNear, splitFar );

    // Calculate the OPTIMAL center of the frustum slice for a minimal bounding sphere
    XMVECTOR nearCenter = XMVectorZero();
    for ( int i = 0; i < 4; ++i ) nearCenter = XMVectorAdd( nearCenter, XMLoadFloat3( &corners[i] ) );
    nearCenter = XMVectorScale( nearCenter, 0.25f );

    XMVECTOR farCenter = XMVectorZero();
    for ( int i = 4; i < 8; ++i ) farCenter = XMVectorAdd( farCenter, XMLoadFloat3( &corners[i] ) );
    farCenter = XMVectorScale( farCenter, 0.25f );

    XMVECTOR viewDir = XMVector3Normalize( XMVectorSubtract( farCenter, nearCenter ) );
    float L = XMVectorGetX( XMVector3Length( XMVectorSubtract( farCenter, nearCenter ) ) );

    float nearRadiusSq = XMVectorGetX( XMVector3LengthSq( XMVectorSubtract( XMLoadFloat3( &corners[0] ), nearCenter ) ) );
    float farRadiusSq = XMVectorGetX( XMVector3LengthSq( XMVectorSubtract( XMLoadFloat3( &corners[4] ), farCenter ) ) );

    // Slide the center along the view axis to the exact point where Near and Far distances equal out
    float optimalX = (L * L + farRadiusSq - nearRadiusSq) / (2.0f * L);
    optimalX = std::clamp( optimalX, 0.0f, L );

    frustumCenter = XMVectorAdd( nearCenter, XMVectorScale( viewDir, optimalX ) );

    // Calculate the true bounding sphere radius covering all corners
    float invariantRadius = 0.0f;
    for ( int i = 0; i < 8; ++i ) {
        XMVECTOR corner = XMLoadFloat3( &corners[i] );
        XMVECTOR distVec = XMVector3Length( XMVectorSubtract( corner, frustumCenter ) );
        invariantRadius = std::max( invariantRadius, XMVectorGetX( distVec ) );
    }

    // Round the radius to fixed increments to prevent floating-point micro-scaling
    // which can happen due to slight FOV/Aspect ratio rounding.
    invariantRadius = std::ceil( invariantRadius * 16.0f ) / 16.0f;
    float radius = invariantRadius;

    float cascadeSize = invariantRadius * 2.0f;

    float texelSize = cascadeSize / static_cast<float>(shadowMapSize);

    // Establish a GLOBAL, unmoving light-space grid by using the World Origin (0,0,0)
    // By anchoring to XMVectorZero(), the grid never shifts as the player moves.
    XMMATRIX tempLightView = XMMatrixLookToLH( XMVectorZero(), lightDir, upDir );

    // Transform the moving frustum center into this global light-space grid
    XMVECTOR centerLS = XMVector3TransformCoord( frustumCenter, tempLightView );

    // Snap the X and Y coordinates to the exact size of a shadow texel.
    float snappedX = std::floor( XMVectorGetX( centerLS ) / texelSize ) * texelSize;
    float snappedY = std::floor( XMVectorGetY( centerLS ) / texelSize ) * texelSize;
    float centerZ = XMVectorGetZ( centerLS );

    XMVECTOR snappedCenterLS = XMVectorSet( snappedX, snappedY, centerZ, 1.0f );

    // Transform the snapped center back into world-space
    XMMATRIX tempLightViewInv = XMMatrixInverse( nullptr, tempLightView );
    XMVECTOR snappedCenterWorld = XMVector3TransformCoord( snappedCenterLS, tempLightViewInv );

    // -----------------------------------------------------------

    // Build the final light view matrix looking at the snapped center
    float pullBackDistance = std::max( 10000.0f, radius * 2.0f );
    XMVECTOR lightPos = XMVectorSubtract( snappedCenterWorld, XMVectorScale( lightDir, pullBackDistance ) );
    XMMATRIX lightView = XMMatrixLookToLH( lightPos, lightDir, upDir );

    // Z-Bounds (Clipping against Scene to prevent overdraw)

    // Find the exact Light-Space Z-bounds of the frustum slice
    float minLightZ = FLT_MAX;
    float maxLightZ = -FLT_MAX;
    for ( const auto& corner : corners ) {
        XMVECTOR vLS = XMVector3TransformCoord( XMLoadFloat3( &corner ), lightView );
        float z = XMVectorGetZ( vLS );
        minLightZ = std::min( minLightZ, z );
        maxLightZ = std::max( maxLightZ, z );
    }

    // --- Dynamic Pullback Calculation ---
    // Calculate how directly overhead the light is. 
    // 1.0 = straight down (noon), 0.0 = completely horizontal (horizon)
    float lightDotUp = std::abs( XMVectorGetX( XMVector3Dot( lightDir, upDirOrig ) ) );
    lightDotUp = std::max( lightDotUp, 0.05f ); // Prevent division by zero near the horizon

    // Assuming a max shadow caster height of ~6000 units (60 meters) above the frustum.
    // The shallower the angle, the longer the shadow, so we increase the pullback.
    float dynamicPullback = 4000.0f / lightDotUp;

    // Clamp to sensible extremes:
    // Min ~2000 units (high noon, just enough for tall objects directly overhead)
    // Max ~15000 units (sunset, catching long shadows from distant mountains)
    dynamicPullback = std::clamp( dynamicPullback, 2000.0f, 15000.0f );

    float orthoNear = std::max( 1.0f, minLightZ - dynamicPullback );
    float orthoFar = maxLightZ + 5000.0f;

    // --- Scene Bounds Optimization ---
    if ( auto worldInfo = Engine::GAPI->GetLoadedWorldInfo() ) {
        if ( auto bspTree = worldInfo->BspTree ) {
            zTBBox3D sceneBox = bspTree->GetRootNode()->BBox3D;
            std::array<XMFLOAT3, 8> sceneCorners = {
                XMFLOAT3( sceneBox.Min.x, sceneBox.Min.y, sceneBox.Min.z ), XMFLOAT3( sceneBox.Max.x, sceneBox.Min.y, sceneBox.Min.z ),
                XMFLOAT3( sceneBox.Min.x, sceneBox.Max.y, sceneBox.Min.z ), XMFLOAT3( sceneBox.Max.x, sceneBox.Max.y, sceneBox.Min.z ),
                XMFLOAT3( sceneBox.Min.x, sceneBox.Min.y, sceneBox.Max.z ), XMFLOAT3( sceneBox.Max.x, sceneBox.Min.y, sceneBox.Max.z ),
                XMFLOAT3( sceneBox.Min.x, sceneBox.Max.y, sceneBox.Max.z ), XMFLOAT3( sceneBox.Max.x, sceneBox.Max.y, sceneBox.Max.z )
            };

            float sceneMinZ = FLT_MAX;
            float sceneMaxZ = -FLT_MAX;
            for ( const auto& corner : sceneCorners ) {
                XMVECTOR vLS = XMVector3TransformCoord( XMLoadFloat3( &corner ), lightView );
                float z = XMVectorGetZ( vLS );
                sceneMinZ = std::min( sceneMinZ, z );
                sceneMaxZ = std::max( sceneMaxZ, z );
            }

            // Pushes the near plane further back if the scene geometry requires it
            orthoNear = std::min( orthoNear, sceneMinZ - 100.0f );

            // Tighten Far Plane so we don't shoot miles past the level boundaries when looking down
            orthoFar = std::min( orthoFar, sceneMaxZ + 500.0f );
        }
    }

    const XMMATRIX crProjRepl = XMMatrixTranspose( XMMatrixOrthographicLH( 
        cascadeSize, cascadeSize, orthoNear, orthoFar ) );

    XMStoreFloat4x4( &outCR.ViewReplacement, XMMatrixTranspose( lightView ) );
    XMStoreFloat4x4( &outCR.ProjectionReplacement, crProjRepl );
    XMStoreFloat3( &outCR.PositionReplacement, lightPos );

    XMVECTOR lookAt = XMVectorAdd( lightPos, lightDir );
    XMStoreFloat3( &outCR.LookAtReplacement, lookAt );

    float cullingMargin = texelSize * 2.0f;
    outCR.frustum.BuildOrthographic( lightView,
        cascadeSize + cullingMargin,
        cascadeSize + cullingMargin,
        orthoNear,
        orthoFar,
        Engine::GAPI->GetRendererState().RendererSettings.DebugSettings.ShadowCascades.ExtendBack,
        Engine::GAPI->GetRendererState().RendererSettings.DebugSettings.ShadowCascades.ExtendFront,
        Engine::GAPI->GetRendererState().RendererSettings.DebugSettings.ShadowCascades.ExtendSide );
}

D3D11ShadowMap::D3D11ShadowMap() {
    
}

D3D11ShadowMap::~D3D11ShadowMap() {}

bool D3D11ShadowMap::ShouldUseAtlas() const {
    const auto& settings = Engine::GAPI->GetRendererState().RendererSettings;
    // FL10 always needs atlas fallback. On FL11+, this can be toggled at runtime.
    return FeatureLevel10Compatibility || settings.DebugSettings.FeatureSet.UseShadowAtlas;
}

void D3D11ShadowMap::RecreateShadowSampler() {
    if ( !m_device ) return;

    // Create sampler
    D3D11_SAMPLER_DESC samplerDesc = {};
    samplerDesc.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR;
    // Atlas path uses CLAMP to prevent seam bleeding at cascade boundaries (it also
    // insets sampling by half a texel per sub-rect).
    // Texture array path uses BORDER with a white (1.0 = far) border: each cascade is its
    // own slice with its own [0,1], so PCF/PCSS taps that spill past a cascade edge read
    // the border depth of 1.0 and compare as "lit" instead of WRAPping to the opposite
    // side of the slice (which fabricated shadow and produced dark bars between cascades).
    auto addressMode = m_useAtlas ? D3D11_TEXTURE_ADDRESS_CLAMP : D3D11_TEXTURE_ADDRESS_BORDER;
    samplerDesc.AddressU = addressMode;
    samplerDesc.AddressV = addressMode;
    samplerDesc.AddressW = addressMode;
    samplerDesc.MipLODBias = 0;
    samplerDesc.MaxAnisotropy = 1;
    samplerDesc.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
    samplerDesc.BorderColor[0] = 1.0f;
    samplerDesc.BorderColor[1] = 1.0f;
    samplerDesc.BorderColor[2] = 1.0f;
    samplerDesc.BorderColor[3] = 1.0f;
    samplerDesc.MinLOD = -FLT_MAX;
    samplerDesc.MaxLOD = FLT_MAX;

    m_shadowmapSampler.Reset();
    HRESULT hr;
    LE( m_device->CreateSamplerState( &samplerDesc, m_shadowmapSampler.GetAddressOf() ) );
    SetDebugName( m_shadowmapSampler.Get(), "ShadowmapSamplerState" );
}

int D3D11ShadowMap::AtlasCascade0Size( int requestedSize, UINT numCascades ) {
    // Multi-cascade atlases pack every cascade into ONE texture of (2*S) x (1.5*S), so an
    // "8192" setting would mean a 16384x12288 surface. Cap the per-cascade size instead.
    const int cap = ( numCascades <= 1 ) ? ( FeatureLevel10Compatibility ? 8192 : 16384 ) : MAX_ATLAS_CASCADE_SIZE;
    return std::min<int>( std::max<int>( requestedSize, 512 ), cap );
}

void D3D11ShadowMap::EnsureShadowMapBackend( int size ) {
    if ( !m_device ) return;

    const auto& settings = Engine::GAPI->GetRendererState().RendererSettings;
    const UINT numCascades = static_cast<UINT>( std::clamp<int>( settings.NumShadowCascades, 1, std::min(4, MAX_CSM_CASCADES) ) );

    bool desiredUseAtlas = ShouldUseAtlas();
    int clampedSize = std::min<int>( std::max<int>( size, 512 ), (FeatureLevel10Compatibility ? 8192 : 16384) );

    if ( desiredUseAtlas != m_useAtlas ) {
        // Switch backend at runtime.
        m_useAtlas = desiredUseAtlas;
        if ( m_useAtlas ) {
            m_cascadedShadowMap.reset();
        } else {
            m_shadowAtlas.reset();
        }

        // Sampler addressing depends on atlas/array path.
        RecreateShadowSampler();

        // SHADOW_ATLAS is a compile-time shader macro; reload relevant shaders when mode flips.
        auto* graphicsEngine = reinterpret_cast<D3D11GraphicsEngine*>( Engine::GraphicsEngine );
        if ( graphicsEngine ) {
            graphicsEngine->ReloadShaders( ShaderCategory::LightsAndShadows );
        }
    }

    // Both backends no-op when size and cascade count already match, so this is safe to poll.
    if ( m_useAtlas ) {
        const int atlasCascade0Size = AtlasCascade0Size( clampedSize, numCascades );
        if ( atlasCascade0Size < clampedSize && m_lastLoggedAtlasCap != clampedSize ) {
            m_lastLoggedAtlasCap = clampedSize;
            LogInfo() << "ShadowAtlas: requested " << clampedSize << " capped to " << atlasCascade0Size
                << " per cascade (" << numCascades << " cascades share one texture)";
        }
        if ( !m_shadowAtlas ) {
            m_shadowAtlas = std::make_unique<D3D11ShadowAtlas>();
            m_shadowAtlas->Init( m_device, atlasCascade0Size, numCascades );
        } else {
            m_shadowAtlas->Resize( atlasCascade0Size, numCascades );
        }
    } else {
        if ( !m_cascadedShadowMap ) {
            m_cascadedShadowMap = std::make_unique<D3D11CascadedShadowMapBuffer>();
        }
        m_cascadedShadowMap->Init( m_device, clampedSize, numCascades );
    }
}

void D3D11ShadowMap::WaitShadowCullingComplete()
{
    ZoneScopedN( "WaitShadowCullingComplete" );
    std::lock_guard<LockableBase( std::mutex )> lock( m_CullingJobsMutex );
    for ( auto& job : m_ShadowCullingJobs ) {
        if ( job.valid() ) {
            job.wait();
        }
    }
}

void D3D11ShadowMap::Init( Microsoft::WRL::ComPtr<ID3D11Device1>& device, Microsoft::WRL::ComPtr<ID3D11DeviceContext1>& context, int size ) {
    m_device = device;
    m_context = context;

    int s = std::min<int>( std::max<int>( size, 512 ), (FeatureLevel10Compatibility ? 8192 : 16384) );

    m_useAtlas = ShouldUseAtlas();
    RecreateShadowSampler();

    // Dummy cube RT used for fallback to satisfy pixel shader runs that expect a RTV bound
    m_dummyCubeRT = std::make_unique<RenderToTextureBuffer>( m_device.Get(), 16, 16, DXGI_FORMAT_ENGINE_DEFAULT, nullptr, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, 1, 6 );

    EnsureShadowMapBackend( s );

    for ( int i = 0; i < MAX_CSM_CASCADES; ++i ) {
        m_RenderQueues[i] = std::make_unique<D3D11RenderQueue>( device.Get(), context.Get() );
    }

    Resize( s );

    if ( !FeatureLevel10Compatibility ) {
        m_TiledDeferred = std::make_unique<D3D11TiledDeferredShading>();
        m_TiledDeferred->Init( device, context );
    }
}

void D3D11ShadowMap::Resize( int size ) {

    if ( !m_device ) return;

    const int maxSize = (FeatureLevel10Compatibility ? 8192 : 16384);
    const int s = std::min<int>( std::max<int>( size, 512 ), maxSize );
    const auto& settings = Engine::GAPI->GetRendererState().RendererSettings;
    const UINT numCascades = static_cast<UINT>( std::clamp<int>( settings.NumShadowCascades, 1, std::min( 4, MAX_CSM_CASCADES ) ) );

    EnsureShadowMapBackend( s );

    m_lastNumCascades = static_cast<int>( numCascades );
    // Fresh depth surfaces hold garbage; the next frame has to fill every cascade before
    // lazy updates may start freezing any of them.
    m_ForceFullCascadeUpdate = true;
}

void D3D11ShadowMap::BindToPixelShader( ID3D11DeviceContext1* context, UINT slot ) {
    if ( m_useAtlas ) {
        if ( m_shadowAtlas ) m_shadowAtlas->BindToPixelShader( context, slot );
    } else {
        if ( m_cascadedShadowMap ) m_cascadedShadowMap->BindToPixelShader( context, slot );
    }
}

void D3D11ShadowMap::BindSampler( ID3D11DeviceContext1* context, UINT slot ) {
    if ( m_shadowmapSampler ) context->PSSetSamplers( slot, 1, m_shadowmapSampler.GetAddressOf() );
}

void D3D11ShadowMap::BindSamplerToCS( ID3D11DeviceContext1* context, UINT slot ) {
    if ( m_shadowmapSampler ) context->CSSetSamplers( slot, 1, m_shadowmapSampler.GetAddressOf() );
}

XRESULT D3D11ShadowMap::PrepareRender()
{
    ZoneScopedN("D3D11ShadowMap::PrepareRender");
    // Check if shadowmap resources need to be recreated due to setting changes
    {
        auto& settings = Engine::GAPI->GetRendererState().RendererSettings;
        // Resolutions are restricted to {512,1024,2048,4096,8192} (see the ImGui combos in ImGuiShim.cpp) — 8192
        // is the hard ceiling for both feature levels now, so there's no FeatureLevel10Compatibility distinction.
        const int desiredSize = std::min<int>( std::max<int>( settings.ShadowMapSize, 512 ), 8192 );
        const int desiredCascades = std::clamp( settings.NumShadowCascades, 1, MAX_CSM_CASCADES );
        // In atlas mode the per-cascade size is capped below ShadowMapSize, so compare against
        // what the atlas would actually allocate - comparing to desiredSize never matched and
        // re-created the whole atlas every frame.
        const int effectiveSize = ShouldUseAtlas()
            ? AtlasCascade0Size( desiredSize, static_cast<UINT>( desiredCascades ) )
            : desiredSize;

        if ( GetSizeX() != effectiveSize
            || m_useAtlas != ShouldUseAtlas()
            || m_lastNumCascades != desiredCascades ) {
            LogInfo() << "Shadowmap config changed, resizing to " << desiredSize << "x" << desiredSize;
            Resize( desiredSize );
            settings.ShadowMapSize = desiredSize;
        }
    }

    zCCamera* camera = (zCCamera*)oCGame::GetGame()->_zCSession_camera;
    if ( !camera ) {
        return XR_SUCCESS;
    }
    camera->Activate();
    const XMVECTOR cameraPositionXm = Engine::GAPI->GetCameraPositionXM();
    XMFLOAT3 cameraPosition;
    XMStoreFloat3( &cameraPosition, cameraPositionXm );

    auto& settings = Engine::GAPI->GetRendererState().RendererSettings;

    // ********************************
    // Cascade Shadow Map Rendering (Simple Sequential Version)
    // ********************************

    const float nearPlane = std::max( 1.0f, camera->GetNearPlane() );
    // Clamp far plane to avoid extreme shadow distances
    const float baseFarPlane = std::min( camera->GetFarPlane(), 12000.0f ); // ~120 meters, fine with Fog enabled.

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

    // Get current light direction from atmosphere.
    // PrepareRender() runs before DrawSky() this frame, so GetAtmosphereCB() would otherwise still hold
    // last frame's values; force a refresh here so AC_LightPos is never stale/zero (matches the D3D12
    // path, see D3D12ShadowMap::ComputeCascadeMatrices). Does not render, just computes atmosphere data.
    Engine::GAPI->GetSky()->RenderSky();

    XMVECTOR currentDir = XMLoadFloat3( &Engine::GAPI->GetSky()->GetAtmosphereCB().AC_LightPos );
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

    const XMVECTOR p = WorldShadowCP + dir * 10000.0f;
    const XMVECTOR lookAt = WorldShadowCP;

    const XMVECTOR lastCascadeP = lastCascadeData.Position + lastCascadeData.LightDir * 10000.0f;
    const XMVECTOR lastCascadeLookAt = lastCascadeData.Position;

    static const XMVECTORF32 c_XM_Up = { { { 0, 1, 0, 0 } } };

    if ( !isOutdoor ) {
        if ( settings.EnableShadows && lastBspMode == zBSP_MODE_OUTDOOR ) {
            // Clear all cascade DSVs
            if ( m_useAtlas && m_shadowAtlas ) {
                // Atlas: single DSV, clear once
                if ( auto dsv = m_shadowAtlas->GetDepthStencilView() ) {
                    m_context->ClearDepthStencilView( dsv, D3D11_CLEAR_DEPTH, 0.0f, 0 );
                }
            } else {
                for ( size_t cascadeIdx = 0; cascadeIdx < MAX_CSM_CASCADES; ++cascadeIdx ) {
                    if ( auto dsv = GetCascadeDSV( static_cast<UINT>( cascadeIdx ) ) ) {
                        m_context->ClearDepthStencilView( dsv, D3D11_CLEAR_DEPTH, 0.0f, 0 );
                    }
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
        // Works on both paths: DrawWorldShadow only clears the rects it is about to redraw, so a
        // frozen cascade keeps the depth it already holds.
        bool lazyCascadeUpdate = settings.DebugSettings.ShadowCascades.LazyCascadeUpdate;

        Frustum playerFrustum = Frustum::AlwaysContainingFrustum();
        if ( auto cam = (zCCamera*)oCGame::GetGame()->_zCSession_camera ) {
            const auto& view = cam->trafoView; // Column-Major, needs Transpose for DxMath
            const auto& proj = cam->trafoProjection; // Row-Major, does not need transpose.
            playerFrustum.BuildPerspective(
                XMMatrixTranspose( XMLoadFloat4x4( &view ) ),
                XMLoadFloat4x4( &proj )
            );
        }

        for ( int cascadeIdx = 0; cascadeIdx < numCascades; ++cascadeIdx ) {
            // pre-calculate all cascade matrices, to be able to frustum-cull anything that is not in this or the next cascade.

            bool isLastCascade = (numCascades > 1 && cascadeIdx == numCascades - 1);

            bool shouldUpdateCascade = true;
            // A cascade holding a per-frame-deformed caster (spinning windmill sails and friends) must never
            // be frozen - see NoteCascadeAnimatedCasters.
            if ( lazyCascadeUpdate && !m_ForceFullCascadeUpdate && !m_CascadeHasAnimatedCaster[cascadeIdx] ) {
                if ( cascadeIdx == 2 ) {
                    // pre-last cascade updates every 2nd frame which is 30 FPS = 15 updates per second
                    shouldUpdateCascade = (perFrameCascadeData.frameCount % 5) == 0;
                } else if ( cascadeIdx == MAX_CSM_CASCADES-1 ) {
                    // final cascade updates every 3rd frame which is 30 FPS = 10 updates per second
                    // it covers the whole world, so this can help improve avg fps.
                    shouldUpdateCascade = (perFrameCascadeData.frameCount % 10) == 0;
                }
            } 
            m_ShouldUpdateCascade[cascadeIdx] = shouldUpdateCascade;

            if ( shouldUpdateCascade || !m_CascadeCRs[cascadeIdx].frustum.IsValid()) {
                CalculateCascadeMatrices(
                    m_CascadeCRs[cascadeIdx],
                    playerFrustum,
                    splits,
                    cascadeIdx,
                    numCascades,
                    farPlane,
                    isLastCascade ? lastCascadeP : p,
                    isLastCascade ? lastCascadeLookAt : lookAt,
                    c_XM_Up,
                    isLastCascade ? lastCascadeData.Position : WorldShadowCP,
                    GetCascadePixelSize( cascadeIdx ) );
            }
        }
        m_ForceFullCascadeUpdate = false;
    }

    if ( settings.ThreadedShadowCulling ) {
        std::lock_guard<LockableBase( std::mutex )> lock( m_CullingJobsMutex );
        m_ShadowCullingJobs.clear();

        for ( size_t i = 0; i < static_cast<size_t>(numCascades); i++ ) {
            m_RenderQueues[i]->Reset();
            if ( !m_ShouldUpdateCascade[i] ) {
                continue; // Skip culling for this cascade if we're not updating it this frame
            }

            m_ShadowCullingJobs.push_back( Engine::WorkerThreadPool->enqueue( []( const std::stop_token& token, D3D11ShadowMap* _this, size_t idx ) {
                if ( token.stop_requested() ) {
                    return;
                }
                ZoneScoped;
                ZoneNameF( "Shadow Cascade %zu", idx );
                const auto& rs = Engine::GAPI->GetRendererState().RendererSettings;
                
                const float shadowDistance = 8000 + (12000.0f * std::max( 0.1f, rs.WorldShadowRangeScale ));
                
                RndCullContext ctx;
                ctx.queue = _this->m_RenderQueues[idx].get();
                ctx.frustum = _this->m_CascadeCRs[idx].frustum;
                ctx.cameraPosition = _this->m_WorldShadowPos;
                ctx.stage = RenderStage::STAGE_DRAW_SHADOWS;
                ctx.drawDistances.OutdoorVobs = std::max(20000.0f, shadowDistance);
                ctx.drawDistances.OutdoorVobsSmall = std::max(20000.0f, shadowDistance);
                ctx.drawDistances.IndoorVobs = std::max(20000.0f, shadowDistance);
                ctx.drawDistances.VisualFX = 0.0f;
                ctx.drawDistancesSq.OutdoorVobs = ctx.drawDistances.OutdoorVobs * ctx.drawDistances.OutdoorVobs;
                ctx.drawDistancesSq.OutdoorVobsSmall = ctx.drawDistances.OutdoorVobsSmall * ctx.drawDistances.OutdoorVobsSmall;
                ctx.drawDistancesSq.IndoorVobs = ctx.drawDistances.IndoorVobs * ctx.drawDistances.IndoorVobs;
                ctx.drawDistancesSq.VisualFX = 0.0f;

                ctx.drawFlags.DrawVOBs = rs.DrawVOBs;
                ctx.drawFlags.DrawMobs = rs.DrawMobs;
                ctx.drawFlags.EnableDynamicLighting = rs.EnableDynamicLighting;
                ctx.drawFlags.EnableOcclusionCulling = false; // shadows do not use the players view frustum for culling, so occlusion culling would be inaccurate and cause popping.
                ctx.drawFlags.CullVobs = rs.DebugSettings.Culling.CullVobs;
                ctx.drawFlags.CollectIndoorVobs = false;
                ctx.drawFlags.CollectMobs = false;
                ctx.drawFlags.CollectLights = false;

                Engine::GAPI->CollectVisibleVobs( ctx );

            }, this, i ).future );
        }
        
        return XR_SUCCESS;
    }

    // Build a conservative culling volume that covers all cascades rendered this frame.
    Frustum frustum = Frustum::AlwaysContainingFrustum();
    if ( isOutdoor && numCascades > 0 ) {
        int lastUpdatedCascade = 0;
        for ( int cascadeIdx = 0; cascadeIdx < numCascades; ++cascadeIdx ) {
            if ( m_ShouldUpdateCascade[cascadeIdx] ) {
                lastUpdatedCascade = cascadeIdx;
            }
        }

        std::array<XMFLOAT3, MAX_CSM_CASCADES * 8> combinedCorners = {};
        size_t combinedCornerCount = 0;

        static constexpr XMFLOAT3 ndcCorners[8] = {
            XMFLOAT3( -1.0f, -1.0f, 0.0f ), XMFLOAT3( 1.0f, -1.0f, 0.0f ),
            XMFLOAT3( -1.0f, 1.0f, 0.0f ),  XMFLOAT3( 1.0f, 1.0f, 0.0f ),
            XMFLOAT3( -1.0f, -1.0f, 1.0f ), XMFLOAT3( 1.0f, -1.0f, 1.0f ),
            XMFLOAT3( -1.0f, 1.0f, 1.0f ),  XMFLOAT3( 1.0f, 1.0f, 1.0f )
        };

        for ( int cascadeIdx = 0; cascadeIdx <= lastUpdatedCascade; ++cascadeIdx ) {
            if ( !m_CascadeCRs[cascadeIdx].frustum.IsValid() ) {
                continue;
            }

            const XMMATRIX view = XMMatrixTranspose( XMLoadFloat4x4( &m_CascadeCRs[cascadeIdx].ViewReplacement ) );
            const XMMATRIX proj = XMMatrixTranspose( XMLoadFloat4x4( &m_CascadeCRs[cascadeIdx].ProjectionReplacement ) );
            const XMMATRIX invViewProj = XMMatrixInverse( nullptr, XMMatrixMultiply( view, proj ) );

            for ( const XMFLOAT3& ndcCorner : ndcCorners ) {
                XMVECTOR worldCorner = XMVector3TransformCoord( XMLoadFloat3( &ndcCorner ), invViewProj );
                XMStoreFloat3( &combinedCorners[combinedCornerCount++], worldCorner );
            }
        }

        if ( combinedCornerCount > 0 ) {
            BoundingSphere combinedSphere;
            BoundingSphere::CreateFromPoints(
                combinedSphere,
                combinedCornerCount,
                combinedCorners.data(),
                sizeof( XMFLOAT3 ) );
            // Keep this conservative because shadow caster expansion can exceed strict cascade bounds.
            combinedSphere.Radius *= 1.2f;
            frustum.BuildCubemapFace( XMLoadFloat3( &combinedSphere.Center ), combinedSphere.Radius, 0 );
        }
    }

    static std::vector<VobInfo*> potentialCasters;
    static std::vector<VobLightInfo*> _1;
    static std::vector<SkeletalVobInfo*> _2;
    potentialCasters.reserve(1024);
    potentialCasters.clear();

    {
        RndCullContext ctx;
        LegacyRenderQueueProxy q(potentialCasters, _1, _2);

        const auto& rs = Engine::GAPI->GetRendererState().RendererSettings;
        const float shadowDistance = 8000 + (12000.0f * std::max( 0.1f, rs.WorldShadowRangeScale ));

        ctx.queue = &q;
        ctx.frustum = frustum;
        ctx.cameraPosition = m_WorldShadowPos;
        ctx.stage = RenderStage::STAGE_DRAW_SHADOWS;
        ctx.drawDistances.OutdoorVobs = std::max(20000.0f, shadowDistance);
        ctx.drawDistances.OutdoorVobsSmall = std::max(20000.0f, shadowDistance);
        ctx.drawDistances.IndoorVobs = std::max(20000.0f, shadowDistance);
        ctx.drawDistances.VisualFX = 0.0f;
        ctx.drawDistancesSq.OutdoorVobs = ctx.drawDistances.OutdoorVobs * ctx.drawDistances.OutdoorVobs;
        ctx.drawDistancesSq.OutdoorVobsSmall = ctx.drawDistances.OutdoorVobsSmall * ctx.drawDistances.OutdoorVobsSmall;
        ctx.drawDistancesSq.IndoorVobs = ctx.drawDistances.IndoorVobs * ctx.drawDistances.IndoorVobs;
        ctx.drawDistancesSq.VisualFX = 0.0f;

        ctx.drawFlags.DrawVOBs = rs.DrawVOBs;
        ctx.drawFlags.DrawMobs = rs.DrawMobs;
        ctx.drawFlags.EnableDynamicLighting = rs.EnableDynamicLighting;
        ctx.drawFlags.EnableOcclusionCulling = false; // shadows do not use the players view frustum for culling, so occlusion culling would be inaccurate and cause popping.
        ctx.drawFlags.CullVobs = rs.DebugSettings.Culling.CullVobs;
        ctx.drawFlags.CollectIndoorVobs = false;
        ctx.drawFlags.CollectMobs = false;
        ctx.drawFlags.CollectLights = false;
        
        Engine::GAPI->CollectVisibleVobs( ctx );
    }
    
    {
        ZoneScopedN("CascadeFrustumCulling");

        for ( int i = 0; i < numCascades; ++i ) {
            m_RenderQueues[i]->Reset();
        }

        if ( numCascades > 3 ) {
            for ( auto vob : potentialCasters ) {

                auto boundingSphere = Frustum::BSphereFromzTBBox3D( vob->Vob->GetBBox() );
                if ( m_CascadeCRs[0].frustum.Intersects( boundingSphere ) ) {
                    m_RenderQueues[0]->GetVobs().push_back( vob );
                    m_RenderQueues[1]->GetVobs().push_back( vob );
                    m_RenderQueues[2]->GetVobs().push_back( vob );
                    m_RenderQueues[3]->GetVobs().push_back( vob );
                    continue;
                }

                if ( /*m_ShouldUpdateCascade[1] && */m_CascadeCRs[1].frustum.Intersects( boundingSphere ) ) {
                    m_RenderQueues[1]->GetVobs().push_back( vob );
                    m_RenderQueues[2]->GetVobs().push_back( vob );
                    m_RenderQueues[3]->GetVobs().push_back( vob );
                    continue;
                }

                if ( m_ShouldUpdateCascade[2] && m_CascadeCRs[2].frustum.Intersects( boundingSphere ) ) {
                    m_RenderQueues[2]->GetVobs().push_back( vob );
                    m_RenderQueues[3]->GetVobs().push_back( vob );
                    continue;
                }

                if ( m_ShouldUpdateCascade[3] && m_CascadeCRs[3].frustum.Intersects( boundingSphere ) )
                    m_RenderQueues[3]->GetVobs().push_back( vob );
            }
        } else if ( numCascades > 2 ) {
            for ( auto vob : potentialCasters ) {
                auto boundingSphere = Frustum::BSphereFromzTBBox3D( vob->Vob->GetBBox() );
                if ( m_CascadeCRs[0].frustum.Intersects( boundingSphere ) ) {
                    m_RenderQueues[0]->GetVobs().push_back( vob );
                    m_RenderQueues[1]->GetVobs().push_back( vob );
                    m_RenderQueues[2]->GetVobs().push_back( vob );
                    continue;
                }

                if ( /*m_ShouldUpdateCascade[1] && */m_CascadeCRs[1].frustum.Intersects( boundingSphere ) ) {
                    m_RenderQueues[1]->GetVobs().push_back( vob );
                    m_RenderQueues[2]->GetVobs().push_back( vob );
                    continue;
                }
                if ( m_ShouldUpdateCascade[2] && m_CascadeCRs[2].frustum.Intersects( boundingSphere ) )
                    m_RenderQueues[2]->GetVobs().push_back( vob );
            }
        } else if ( numCascades > 1 ) {
            for ( auto vob : potentialCasters ) {
                auto boundingSphere = Frustum::BSphereFromzTBBox3D( vob->Vob->GetBBox() );
                if ( m_CascadeCRs[0].frustum.Intersects( boundingSphere ) )                     {
                    m_RenderQueues[0]->GetVobs().push_back( vob );
                    m_RenderQueues[1]->GetVobs().push_back( vob );
                    continue;
                }

                if ( /*m_ShouldUpdateCascade[1] && */m_CascadeCRs[1].frustum.Intersects( boundingSphere ) )
                    m_RenderQueues[1]->GetVobs().push_back( vob );
            }
        } else if ( numCascades > 0 ) {
            for ( auto vob : potentialCasters ) {
                auto boundingSphere = Frustum::BSphereFromzTBBox3D( vob->Vob->GetBBox() );
                if ( m_CascadeCRs[0].frustum.Intersects( boundingSphere ) )
                    m_RenderQueues[0]->GetVobs().push_back( vob );
            }
        }
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

void D3D11ShadowMap::ConfigurePointSlots() {
    // Idempotent: the pool sizes are compile-time constants, so only the first call actually sizes the tables.
    PointLightSlotSelector::Config cfg;
    cfg.MaxStaticSlots = MAX_STATIC_SHADOW_CUBEMAPS;
    cfg.MaxDynamicSlots = MAX_DYN_SHADOW_CUBEMAPS;
    m_PointSlots.Configure( cfg );
}


void D3D11ShadowMap::ReleasePointLightSlotFor( const zCVob* lightVob ) {
    m_PointSlots.ReleaseFor( reinterpret_cast<uint64_t>( lightVob ) );
}


void D3D11ShadowMap::ReconcileTiledSlots() {
    if ( !m_TiledDeferred ) return;
    // The WHOLE light map, not just this frame's candidates: a light whose slot was handed to somebody else
    // must let go of its end of it, and it can be anywhere at all when that happens.
    for ( auto& it : Engine::GAPI->VobLightMap ) {
        VobLightInfo* light = it.second;
        if ( !light || !light->Vob ) continue;
        D3D11PointLight* pl = light->LightShadowBuffers
            ? dynamic_cast<D3D11PointLight*>( light->LightShadowBuffers.get() ) : nullptr;
        if ( !pl ) continue;

        const uint64_t key = reinterpret_cast<uint64_t>( light->Vob );
        const int staticSlot = m_PointSlots.FindStaticSlotOf( key );
        if ( staticSlot < 0 ) {
            pl->ClearDynSlot();
            pl->ClearStaticSlot();
            continue;
        }

        if ( pl->GetStaticSlot() != staticSlot ) {
            RenderToDepthStencilBuffer* target = m_TiledDeferred->ClaimStaticSlot( staticSlot );
            if ( !target ) {
                // The array could not be created - give the slot back rather than hold it out of the pool.
                m_PointSlots.ReleaseFor( key );
                pl->ClearDynSlot();
                pl->ClearStaticSlot();
                continue;
            }
            pl->ClearStaticSlot();
            pl->ReleaseShadowMap();   // a tiled light never also holds a legacy per-light cubemap
            pl->SetStaticSlot( staticSlot, target, &m_PointSlots );
            pl->SetCurrentResolution( STATIC_SHADOW_CUBE_SIZE );
        }

        const int dynSlot = m_PointSlots.FindDynSlotOf( key );
        if ( dynSlot < 0 ) {
            pl->ClearDynSlot();
        } else if ( pl->GetDynSlot() != dynSlot ) {
            if ( RenderToDepthStencilBuffer* dynTarget = m_TiledDeferred->ClaimDynSlot( dynSlot ) ) {
                pl->SetDynSlot( dynSlot, dynTarget );
            } else {
                // No overlay array (creation declined): the light keeps its static cube and nothing else.
                m_PointSlots.ReleaseDynamicFor( key );
                pl->ClearDynSlot();
            }
        }
    }
}


XRESULT D3D11ShadowMap::DrawPointlightShadows( std::vector<VobLightInfo*>& lights ) {
    ZoneScopedN( "DrawPointlightShadows" );

    auto& settings = Engine::GAPI->GetRendererState().RendererSettings;
    if ( !m_TiledDeferred || !settings.EnableTiledLighting )
        return DrawPointlightShadowsLegacy( lights );

    ConfigurePointSlots();
    if ( settings.EnablePointlightShadows <= 0 ) {
        // Re-enabling mid-session must re-render from scratch, not sample however stale depth is left.
        m_PointSlots.Select( {}, GothicRendererSettings::PLS_DISABLED );
        ReconcileTiledSlots();
        return XR_SUCCESS;
    }

    auto graphicsEngine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    auto _ = graphicsEngine->RecordGraphicsEvent( GE_NAME( "DrawPointlightShadows" ) );
    graphicsEngine->SetDefaultStates();

    // Distance sweep of every registered light against the domes - deliberately NOT this frame's visible set,
    // so turning away from a light cannot cost it its cube.
    m_PointSlots.BuildCandidates( m_PointCandidates );
    m_PointSlots.Select( m_PointCandidates, settings.EnablePointlightShadows );

    // Every slot owner needs a renderer object and the right two targets before anything renders.
    for ( const PointLightSlotSelector::Assignment& a : m_PointSlots.GetAssignments() ) {
        VobLightInfo* light = a.light;
        if ( !light || !light->Vob ) continue;
        if ( !light->LightShadowBuffers ) {
            BaseShadowedPointLight* bpl = nullptr;
            graphicsEngine->CreateShadowedPointLight( &bpl, light, /*dynamic light*/ true );
            light->LightShadowBuffers.reset( bpl );
        }
        // The quantized, grow-only cube range the selector decided this bake with, folded in before the
        // render so the projection and the shader's depth compare use the same far plane.
        if ( auto* pl = dynamic_cast<D3D11PointLight*>( light->LightShadowBuffers.get() ) )
            pl->SetShadowRange( a.range );
    }
    ReconcileTiledSlots();

    // Render exactly what the frame budget granted; the assignments are in candidate (nearest-first) order.
    // Nothing here decides anything - see PointLightSlotSelector::Select.
    for ( const PointLightSlotSelector::Assignment& a : m_PointSlots.GetAssignments() ) {
        VobLightInfo* light = a.light;
        if ( !light || !light->LightShadowBuffers ) continue;
        light->UpdateShadows = false;   // the selector is the only scheduler now; drop Gothic's hint
        if ( !a.renderStatic && !a.renderDynamic ) continue;
        auto* pl = static_cast<D3D11PointLight*>( light->LightShadowBuffers.get() );
        if ( pl->GetStaticSlot() < 0 ) continue;   // ReconcileTiledSlots claimed no target for it
        pl->RenderTiledShadow( a.renderStatic, a.renderDynamic );
        graphicsEngine->DebugPointlight = pl;
    }

    return XR_SUCCESS;
}


/** Tiled lighting off: no shared cube arrays and so no slots to select from. Every light owns an unbounded
    DepthStencilPool cubemap instead, gated by distance and drained through a small per-frame budget. */
XRESULT D3D11ShadowMap::DrawPointlightShadowsLegacy( std::vector<VobLightInfo*>& lights ) {
    auto& settings = Engine::GAPI->GetRendererState().RendererSettings;

    // Never on the first absent frame, or a blinking light could never finish a bake that sticks.
    constexpr int kPointLightSlotRetentionFrames = 120;
    for ( auto& it : Engine::GAPI->VobLightMap ) {
        if ( !it.second->LightShadowBuffers ) continue;
        if ( D3D11PointLight* pl = dynamic_cast<D3D11PointLight*>(it.second->LightShadowBuffers.get()) ) {
            const bool visible = it.second->Vob->IsEnabled() && it.second->VisibleInFrame;
            if ( pl->NoteAbsence( visible, kPointLightSlotRetentionFrames ) ) {
                pl->ReleaseShadowMap();
            }
        }
    }

    if ( settings.EnablePointlightShadows <= 0 ) return XR_SUCCESS;

    auto graphicsEngine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
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
            auto& queue = graphicsEngine->FrameShadowUpdateLights;
            if ( std::find( queue.begin(), queue.end(), light ) == queue.end() ) {
                queue.emplace_back( light );
            }
        } else if ( staticOnlyMode ) {
            auto& queue = graphicsEngine->FrameShadowUpdateLights;
            auto queued = std::find( queue.begin(), queue.end(), light );
            if ( queued != queue.end() ) {
                queue.erase( queued );
            }
        }
    };

    // Drops a light out of both update paths - nothing is going to re-render its cube this frame.
    auto dequeueLight = [&]( VobLightInfo* light ) {
        auto it = std::find( graphicsEngine->FrameShadowUpdateLights.begin(), graphicsEngine->FrameShadowUpdateLights.end(), light );
        if ( it != graphicsEngine->FrameShadowUpdateLights.end() ) {
            graphicsEngine->FrameShadowUpdateLights.erase( it );
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
            light->LightShadowBuffers.reset( bpl );
        }

        D3D11PointLight* pl = dynamic_cast<D3D11PointLight*>(light->LightShadowBuffers.get());
        if ( !pl ) continue;

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
            if ( !pl->HasShadowMap( 0 ) || pl->GetShadowMapResolution() != SHADOW_CUBE_SIZE ) {
                legacyAcquires.push_back( { light, pl, d } );
                continue;
            }
            classifyLight( light, pl, d );
        } else if ( pl->HasAnyShadowMap() ) {
            pl->ReleaseShadowMap();
            dequeueLight( light );
        }
    }

    std::sort( legacyAcquires.begin(), legacyAcquires.end(), []( const LegacyAcquire& a, const LegacyAcquire& b ) {
        return a.distSq < b.distSq;
    } );
    for ( auto& c : legacyAcquires ) {
        c.pl->ReleaseShadowMap();
        c.pl->AcquireShadowMap( dsPool, SHADOW_CUBE_SIZE );
        c.light->UpdateShadows = true;
        classifyLight( c.light, c.pl, c.distSq );
    }
    // No fixed pools to report on here; zero Max hides the row entirely.
    auto& info = Engine::GAPI->GetRendererState().RendererInfo;
    info.PointLightSlotsMax = 0;
    info.PointLightStaticSlotsMax = 0;
    info.PointLightSlotsStarved = 0;

    // Render the immediate priority lights - but never more than a handful in one frame.
    //
    // Each rebuild keeps its view-matrix CB bound at VS b3 / GS b2 across every draw of both its passes while
    // those draws keep allocating from the same per-frame ring; once it wraps, earlier lights finish rendering
    // with another light's projection. Overflow drains through the round-robin below instead.
    std::sort( importantUpdates.begin(), importantUpdates.end(), []( const auto& a, const auto& b ) {
        return a.first < b.first;
    } );

    constexpr int maxImportantUpdates = 8;
    int importantDone = 0;
    for ( auto const& [distSq, importantUpdate] : importantUpdates ) {
        if ( importantDone >= maxImportantUpdates ) {
            auto& queue = graphicsEngine->FrameShadowUpdateLights;
            if ( std::find( queue.begin(), queue.end(), importantUpdate ) == queue.end() ) {
                queue.emplace_back( importantUpdate );
            }
            continue;
        }

        static_cast<D3D11PointLight*>(importantUpdate->LightShadowBuffers.get())->RenderCubemap( importantUpdate->UpdateShadows );
        importantUpdate->UpdateShadows = false;
        importantDone++;
    }

    // Process Background Queue (Round-Robin)
    constexpr int kBackgroundUpdateBudget = 2;
    int updateBudget = kBackgroundUpdateBudget;

    while ( !graphicsEngine->FrameShadowUpdateLights.empty() && updateBudget > 0 ) {
        auto light = graphicsEngine->FrameShadowUpdateLights.front();
        graphicsEngine->FrameShadowUpdateLights.pop_front();

        if ( !light ) continue;

        D3D11PointLight* l = static_cast<D3D11PointLight*>( light->LightShadowBuffers.get() );
        if ( !l ) continue;

        if ( staticOnlyMode && l->IsStaticShadowReady() && !l->NeedsUpdate() ) {
            light->UpdateShadows = false;
            continue;
        }
        bool force = light->UpdateShadows;
        light->UpdateShadows = false;

        // FORCE the render! It waited in line for its turn, it must draw.
        l->RenderCubemap( force );
        graphicsEngine->DebugPointlight = l;

        --updateBudget;
    }

    return XR_SUCCESS;
}

void D3D11ShadowMap::ClearAtlasCascade( UINT cascadeIndex, float depth ) {
    if ( !m_shadowAtlas ) return;
    auto dsv = m_shadowAtlas->GetDepthStencilView();
    if ( !dsv ) return;

    auto graphicsEngine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    auto _ = graphicsEngine->RecordGraphicsEvent( GE_NAME( "ClearAtlasCascade" ) );

    // The viewport both scissors the triangle to this cascade's rect and, by collapsing its
    // depth range onto a single value, turns VS_PFX's z=0 output into the clear depth.
    D3D11_VIEWPORT vp = m_shadowAtlas->GetCascadeViewport( cascadeIndex );
    vp.MinDepth = depth;
    vp.MaxDepth = depth;

    D3D11_VIEWPORT oldVP; UINT n = 1;
    m_context->RSGetViewports( &n, &oldVP );
    m_context->RSSetViewports( 1, &vp );
    m_context->OMSetRenderTargets( 0, nullptr, dsv );

    auto& renderState = Engine::GAPI->GetRendererState();
    renderState.BlendState.SetDefault();
    renderState.BlendState.ColorWritesEnabled = false;
    renderState.BlendState.SetDirty();
    renderState.RasterizerState.SetDefault();
    renderState.RasterizerState.CullMode = GothicRasterizerStateInfo::CM_CULL_NONE;
    renderState.RasterizerState.SetDirty();
    renderState.DepthState.SetDefault();
    renderState.DepthState.DepthBufferCompareFunc = GothicDepthBufferStateInfo::CF_COMPARISON_ALWAYS;
    renderState.DepthState.SetDirty();

    graphicsEngine->SetActiveVertexShader( VShaderID::VS_PFX );
    graphicsEngine->GetActiveVS()->Apply();
    D3D11PipelineStateCache::SetPixelShader( m_context.Get(), nullptr );

    graphicsEngine->GetPfxRenderer()->DrawFullScreenQuad();

    m_context->RSSetViewports( 1, &oldVP );
}

XRESULT D3D11ShadowMap::DrawWorldShadow( )
{
    auto graphicsEngine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    auto _ = graphicsEngine->RecordGraphicsEvent( GE_NAME( "DrawWorldShadow" ) );
    ZoneScopedN( "DrawWorldShadow" );

    WaitShadowCullingComplete();

    auto& settings = Engine::GAPI->GetRendererState().RendererSettings;
    
    int numCascades = settings.NumShadowCascades;
    bool isOutdoor = Engine::GAPI->GetLoadedWorldInfo()->BspTree->GetBspTreeMode() == zBSP_MODE_OUTDOOR;

    // Fully enclosed view (portal culling): nothing the sun lights is on screen, so clear every
    // cascade to 0.0 = fully shadowed and cast nothing - the same result the room's own shell would
    // have produced, for three clears. Only the DRAWS are skipped here; the culls were launched back
    // in OnStartWorldRendering, before this frame's portal solve existed, and WaitShadowCullingComplete
    // above already joined them. D3D12 evaluates this early enough to skip the culls too.
    const bool sunFullyOccluded = isOutdoor && Engine::GAPI->AreSunShadowsFullyOccluded();
    if ( sunFullyOccluded ) {
        if ( m_useAtlas && m_shadowAtlas ) {
            if ( auto dsv = m_shadowAtlas->GetDepthStencilView() ) {
                m_context->ClearDepthStencilView( dsv, D3D11_CLEAR_DEPTH, 0.0f, 0 );
            }
        } else {
            for ( int cascadeIdx = 0; cascadeIdx < numCascades; ++cascadeIdx ) {
                if ( auto dsv = GetCascadeDSV( static_cast<UINT>( cascadeIdx ) ) ) {
                    m_context->ClearDepthStencilView( dsv, D3D11_CLEAR_DEPTH, 0.0f, 0 );
                }
            }
        }
        for ( int cascadeIdx = 0; cascadeIdx < numCascades; ++cascadeIdx ) {
            m_RenderQueues[cascadeIdx]->Reset();
        }
        Engine::GAPI->SetCameraReplacementPtr( nullptr );
        return XR_SUCCESS;
    }

    if ( isOutdoor ) {
        // Atlas path: one DSV covers every cascade, so nothing may be cleared wholesale while
        // lazy updates leave some cascades frozen. Only the rects redrawn below get cleared.
        float atlasClearValue = 1.0f;
        if ( m_useAtlas && m_shadowAtlas ) {
            const bool shouldRenderShadows =
                Engine::GAPI->GetSky()->GetAtmoshpereSettings().LightDirection.y > 0 &&
                settings.DrawShadowGeometry &&
                settings.EnableShadows;
            if ( !shouldRenderShadows ) {
                // No caster is drawn at all this frame; the whole atlas becomes one constant.
                atlasClearValue = Engine::GAPI->GetSky()->GetAtmoshpereSettings().LightDirection.y <= 0 ? 0.0f : 1.0f;
                if ( auto dsv = m_shadowAtlas->GetDepthStencilView() ) {
                    m_context->ClearDepthStencilView( dsv, D3D11_CLEAR_DEPTH, atlasClearValue, 0 );
                }
                for ( int cascadeIdx = 0; cascadeIdx < numCascades; ++cascadeIdx ) {
                    m_RenderQueues[cascadeIdx]->Reset();
                }
                Engine::GAPI->SetCameraReplacementPtr( nullptr );
                return XR_SUCCESS;
            }
        }

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

            // Atlas path: clear only this cascade's rect, then render into it.
            if ( m_useAtlas && m_shadowAtlas ) {
                ClearAtlasCascade( static_cast<UINT>(cascadeIdx), atlasClearValue );
                renderParams.ViewportOverride = m_shadowAtlas->GetCascadeViewport( static_cast<UINT>(cascadeIdx) );
                renderParams.UseViewportOverride = true;
                renderParams.SkipClear = true;
            }

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
    if ( Engine::GAPI->GetRainFXWeight() > 0.00001f ) {
        auto graphicsEngine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
        auto _ = graphicsEngine->RecordGraphicsEvent( GE_NAME( "DrawRainShadowmap" ) );
        ZoneScopedN( "DrawRainShadowmap" );

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
    auto& settings = Engine::GAPI->GetRendererState().RendererSettings;

    if ( m_TiledDeferred && settings.EnableTiledLighting ) {
        return m_TiledDeferred->DrawPointlightLights( lights, color, normals, specular, depthCopy );
    }

    return m_LegacyDeferred.DrawPointlightLights( lights, color, normals, specular, depthCopy );
}

XRESULT D3D11ShadowMap::DrawLighting(
    std::vector<VobLightInfo*>& lights,
    RenderToTextureBuffer& color,
    RenderToTextureBuffer& normals,
    RenderToTextureBuffer& specular,
    RenderToTextureBuffer& depthCopy,
    ID3D11ShaderResourceView* aoMaskSRV) {
    auto graphicsEngine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    auto& settings = Engine::GAPI->GetRendererState().RendererSettings;

    graphicsEngine->SetDefaultStates();

    // Draw pointlight shadows
    DrawPointlightShadows(lights);

    if ( settings.EnableShadows ) {
        DrawWorldShadow();
    }

    graphicsEngine->SetDefaultStates();

    DrawRainShadowmap();

    Engine::GAPI->SetFarPlane(static_cast<float>(settings.SectionDrawRadius) * WORLD_SECTION_SIZE );

    DrawPointlightLights(lights, color, normals, specular, depthCopy);

    m_context->OMSetRenderTargets( 1, graphicsEngine->GetHDRBackBuffer().GetRenderTargetView().GetAddressOf(),
        nullptr );

    ID3D11ShaderResourceView* srvs[3] = {
        color.GetShaderResView().Get(),
        normals.GetShaderResView().Get(),
        depthCopy.GetShaderResView().Get(),
    };
    m_context->PSSetShaderResources( 0, 3, srvs );

    srvs[0] = specular.GetShaderResView().Get();
    m_context->PSSetShaderResources( 7, 1, srvs );

    DrawWorldLights( aoMaskSRV );

    m_context->OMSetRenderTargets( 1, graphicsEngine->GetHDRBackBuffer().GetRenderTargetView().GetAddressOf(),
        graphicsEngine->GetDepthBuffer()->GetDepthStencilView().Get() );

    return XR_SUCCESS;
}



/** Renders the shadowmaps for the sun */
void D3D11ShadowMap::RenderShadowmaps( const RenderShadowmapsParams& params ) {

    // We now assume that "target" always is something else than the world shadowmap
    UINT targetSize;
    if ( params.UseViewportOverride ) {
        targetSize = static_cast<UINT>( params.ViewportOverride.Width );
    } else if ( params.Target ) {
        targetSize = params.Target->GetSizeX();
    } else if ( m_useAtlas && m_shadowAtlas ) {
        targetSize = m_shadowAtlas->GetCascade0Size();
    } else {
        targetSize = m_cascadedShadowMap ? m_cascadedShadowMap->GetSize() : 0;
    }

    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> dsvOverwrite = params.DSVOverwrite;
    if ( params.Target && !dsvOverwrite.Get() ) dsvOverwrite = params.Target->GetDepthStencilView().Get();
    const bool isNotWorldShadowMap = params.Target != nullptr;

    // todo: remove this dependency at some point
    auto graphicsEngine = (D3D11GraphicsEngine*)Engine::GraphicsEngine;
    auto _ = graphicsEngine->RecordGraphicsEvent( GE_NAME( "RenderShadowmaps" ) );

    D3D11_VIEWPORT oldVP;
    UINT n = 1;
    m_context->RSGetViewports( &n, &oldVP );

    // Apply new viewport
    if ( params.UseViewportOverride ) {
        m_context->RSSetViewports( 1, &params.ViewportOverride );
    } else {
        D3D11_VIEWPORT vp;
        vp.TopLeftX = 0;
        vp.TopLeftY = 0;
        vp.MinDepth = 0.0f;
        vp.MaxDepth = 1.0f;
        vp.Width = static_cast<float>(targetSize);
        vp.Height = vp.Width;
        m_context->RSSetViewports( 1, &vp );
    }

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
        if ( !params.SkipClear ) {
            m_context->ClearDepthStencilView( dsvOverwrite.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0 );
        }

        // Draw the world mesh without textures        

        XMVECTOR cameraPosition = XMLoadFloat3( &params.CameraPosition );
        int timerLabelIndex = std::clamp(params.CascadeIndex, 0, MAX_CSM_CASCADES-1);

        ZoneScopedN( "Shadows::DrawCascade" );
        graphicsEngine->DrawWorldAroundForWorldShadow( cameraPosition, 2, params );

    } else {
        if ( !params.SkipClear ) {
            if ( Engine::GAPI->GetSky()->GetAtmoshpereSettings().LightDirection.y <= 0 ) {
                m_context->ClearDepthStencilView( dsvOverwrite.Get(), D3D11_CLEAR_DEPTH, 0.0f,
                    0 );  // Always shadow in the night
            } else {
                m_context->ClearDepthStencilView(
                    dsvOverwrite.Get(), D3D11_CLEAR_DEPTH, 1.0f,
                    0 );  // Clear shadowmap when shadows not enabled
            }
        }
    }

    // Restore state
    graphicsEngine->SetRenderingStage( oldStage );
    m_context->RSSetViewports( 1, &oldVP );

    Engine::GAPI->SetFarPlane(
        Engine::GAPI->GetRendererState().RendererSettings.SectionDrawRadius *
        WORLD_SECTION_SIZE );
}

DS_ScreenQuadConstantBuffer D3D11ShadowMap::FillSunCSMConstantBuffer() const {
    auto& settings = Engine::GAPI->GetRendererState().RendererSettings;
    float rain = Engine::GAPI->GetRainFXWeight();

    XMMATRIX viewRaw = Engine::GAPI->GetViewMatrixXM();
    XMMATRIX view = XMMatrixTranspose( viewRaw );

    GSky* sky = Engine::GAPI->GetSky();
    auto& proj = Engine::GAPI->GetProjectionMatrix();

    DS_ScreenQuadConstantBuffer scb = {};
    scb.SQ_ProjParams = float4( 1.0f / proj._11, 1.0f / proj._22, proj._43, proj._33 );
    XMStoreFloat4x4( &scb.SQ_InvView, XMMatrixInverse( nullptr, viewRaw ) );
    XMStoreFloat4x4( &scb.SQ_View, viewRaw );

    static uint32_t frameCounter = 0;
    if ( proj._13 != 0 || proj._23 != 0 ) {
        scb.SQ_FrameIndex = frameCounter++;
    }

    // Camera jitter (TAA/FSR) baked into the projection, in UV space. Subtracted in the
    // shader before depth->world reconstruction so shadows don't crawl/flicker each frame.
    scb.SQ_JitterOffset = float2( proj._13 * 0.5f, -proj._23 * 0.5f );

    XMVECTOR lightDirWorld = XMLoadFloat3( &sky->GetAtmosphereCB().AC_LightPos );
    XMStoreFloat3( &scb.SQ_LightDirectionWS, lightDirWorld );
    XMStoreFloat3( &scb.SQ_LightDirectionVS,
        XMVector3TransformNormal( lightDirWorld, view ) );

    float3 sunColor = settings.SunLightColor;
    float sunStrength = Toolbox::lerp(
        settings.SunLightStrength,
        settings.RainSunLightStrength,
        std::min( 1.0f, rain * 2.0f ) );
    scb.SQ_LightColor = float4( sunColor.x, sunColor.y, sunColor.z, sunStrength );
    scb.SQ_SunSpecularEnabled = ( settings.SpecularHighlightsFlags & GothicRendererSettings::SH_SUN ) ? 1.0f : 0.0f;

    for ( size_t cascadeIdx = 0; cascadeIdx < MAX_CSM_CASCADES; ++cascadeIdx ) {
        XMStoreFloat4x4( &scb.SQ_ShadowViewProj[cascadeIdx],
            XMLoadFloat4x4( &m_CascadeCRs[cascadeIdx].ProjectionReplacement ) *
                XMLoadFloat4x4( &m_CascadeCRs[cascadeIdx].ViewReplacement ) );
    }

    scb.SQ_ShadowmapSize = static_cast<float>( this->GetSizeX() );

    if ( m_useAtlas && m_shadowAtlas ) {
        for ( size_t i = 0; i < MAX_CSM_CASCADES; ++i ) {
            scb.SQ_CascadeAtlasRect[i] = m_shadowAtlas->GetCascadeUVRect( static_cast<UINT>( i ) );
        }
    }

    // Precompute world-space units-per-texel for each cascade so shaders can read
    // SQ_CascadeTexelSize[i] instead of running per-fragment matrix math.
    {
        const float mapSize = static_cast<float>( this->GetSizeX() );
        float* ts = scb.SQ_CascadeTexelSize.toPtr();
        for ( size_t i = 0; i < MAX_CSM_CASCADES; ++i ) {
            const XMFLOAT4X4& m = scb.SQ_ShadowViewProj[i];
            const float sx = sqrtf( m._11 * m._11 + m._21 * m._21 + m._31 * m._31 );
            const float sy = sqrtf( m._12 * m._12 + m._22 * m._22 + m._32 * m._32 );
            const float wx = ( sx > 1e-6f ) ? ( 2.0f / sx ) : 0.0f;
            const float wy = ( sy > 1e-6f ) ? ( 2.0f / sy ) : 0.0f;
            // Cascade resolution in pixels - in atlas mode this is the sub-rect's own size, not
            // the atlas-relative UV scale (which is half of it and doubled the normal offset).
            const float res = m_useAtlas
                ? static_cast<float>( GetCascadePixelSize( static_cast<UINT>( i ) ) )
                : mapSize;
            ts[i] = 0.5f * ( wx + wy ) / std::max( res, 1.0f );
        }
    }

    XMStoreFloat4x4( &scb.SQ_RainViewProj,
        XMLoadFloat4x4( &reinterpret_cast<D3D11GraphicsEngine*>( Engine::GraphicsEngine )->Effects->GetRainShadowmapCameraRepl().ProjectionReplacement ) *
        XMLoadFloat4x4( &reinterpret_cast<D3D11GraphicsEngine*>( Engine::GraphicsEngine )->Effects->GetRainShadowmapCameraRepl().ViewReplacement ) );

    scb.SQ_ShadowStrength = settings.ShadowStrength;
    scb.SQ_ShadowAOStrength = settings.ShadowAOStrength;
    scb.SQ_WorldAOStrength = settings.WorldAOStrength;
    scb.SQ_ShadowSoftness = settings.ShadowSoftness;
    scb.SQ_LightSize = std::clamp( settings.PCSSLightSize, 0.005f, 0.5f );

    if ( auto bspTree = Engine::GAPI->GetLoadedWorldInfo()->BspTree )
        if ( bspTree->GetBspTreeMode() == zBSP_MODE_INDOOR ) {
#if BUILD_GOTHIC_1_08k
            if ( Engine::GAPI->GetLoadedWorldInfo()->WorldName == "ORCTEMPEL" )
                scb.SQ_ShadowStrength = 0.15f;
            else
                scb.SQ_ShadowStrength = 0.3f;
#else
            scb.SQ_ShadowStrength = 0.0f;
#endif
            scb.SQ_WorldAOStrength = 1.0f;
            // ...and no shadow AO on top of it. Both terms multiply the same baked vertLighting into the
            // result, so keeping them both on darkens interiors twice over.
            scb.SQ_ShadowAOStrength = 0.0f;
            scb.SQ_LightColor = float4( 1, 1, 1, DEFAULT_INDOOR_VOB_AMBIENT.x );
        }

    return scb;
}

XRESULT D3D11ShadowMap::DrawWorldLights( ID3D11ShaderResourceView* aoMaskSRV )
{
    auto graphicsEngine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    auto _ = graphicsEngine->RecordGraphicsEvent( GE_NAME( "DrawWorldLights" ) );
    TracyD3D11ZoneCGX( "D3D11ShadowMap::DrawWorldLights");
    auto& settings = Engine::GAPI->GetRendererState().RendererSettings;

    Engine::GAPI->GetRendererState().BlendState.SetAdditiveBlending();
    Engine::GAPI->GetRendererState().BlendState.SetDirty();

    Engine::GAPI->GetRendererState().DepthState.DepthBufferCompareFunc = GothicDepthBufferStateInfo::CF_COMPARISON_ALWAYS;
    Engine::GAPI->GetRendererState().DepthState.SetDirty();

    Engine::GAPI->GetRendererState().RasterizerState.CullMode = GothicRasterizerStateInfo::CM_CULL_NONE;
    Engine::GAPI->GetRendererState().RasterizerState.SetDirty();

    // Modify light when raining
    float rain = Engine::GAPI->GetRainFXWeight();
    float wetness = Engine::GAPI->GetSceneWetness();

    XMMATRIX viewRaw = Engine::GAPI->GetViewMatrixXM();
    XMMATRIX view = XMMatrixTranspose( viewRaw );

    bool isSnow = oCGame::GetGame()
        && oCGame::GetGame()->_zCSession_world
        && oCGame::GetGame()->_zCSession_world->GetSkyControllerOutdoor()
        && oCGame::GetGame()->_zCSession_world->GetSkyControllerOutdoor()->GetWeatherType() == zTWEATHER_SNOW;

    // Switch global light shader when raining
    if ( wetness > 0.0f && !isSnow ) {
        // Same shader, just has a DEFINE set to enable rain-related effects
        graphicsEngine->SetActivePixelShader( PShaderID::PS_DS_AtmosphericScattering_Rain );
    } else {
        graphicsEngine->SetActivePixelShader( PShaderID::PS_DS_AtmosphericScattering );
    }

    graphicsEngine->SetActiveVertexShader( VShaderID::VS_PFX );

    auto psAtmo = graphicsEngine->GetActivePS();
    auto vsPfx = graphicsEngine->GetActiveVS();

    graphicsEngine->SetupVS_ExMeshDrawCall();

    GSky* sky = Engine::GAPI->GetSky();
    psAtmo->UpdateBuffer("Atmosphere", &sky->GetAtmosphereCB(), sizeof(sky->GetAtmosphereCB()));

    auto& proj = Engine::GAPI->GetProjectionMatrix();
    DS_ScreenQuadConstantBuffer scb = {};
    scb.SQ_ProjParams = float4( 1.0f / proj._11, 1.0f / proj._22, proj._43, proj._33 );
    XMStoreFloat4x4( &scb.SQ_InvView, XMMatrixInverse( nullptr, viewRaw ) );
    XMStoreFloat4x4( &scb.SQ_View, viewRaw );

    static uint32_t frameCounter = 0;
    if ( proj._13 != 0 || proj._23 != 0) {
        // only when we have jitter in the frame
        scb.SQ_FrameIndex = frameCounter++;
    }

    // Camera jitter (TAA/FSR) baked into the projection, in UV space. Subtracted in the
    // shader before depth->world reconstruction so shadows don't crawl/flicker each frame.
    scb.SQ_JitterOffset = float2( proj._13 * 0.5f, -proj._23 * 0.5f );

    XMVECTOR lightDirWorld = XMLoadFloat3( &sky->GetAtmosphereCB().AC_LightPos );
    XMStoreFloat3( &scb.SQ_LightDirectionWS, lightDirWorld );
    XMStoreFloat3( &scb.SQ_LightDirectionVS,
        XMVector3TransformNormal( lightDirWorld, view ) );

    float3 sunColor =
        settings.SunLightColor;

    float sunStrength = Toolbox::lerp(
        settings.SunLightStrength,
        settings.RainSunLightStrength,
        std::min( 1.0f, rain * 2.0f ) );// Scale the darkening-factor faster here, so it
    // matches more with the increasing fog-density

    scb.SQ_LightColor = float4( sunColor.x, sunColor.y, sunColor.z, sunStrength );
    scb.SQ_SunSpecularEnabled = ( settings.SpecularHighlightsFlags & GothicRendererSettings::SH_SUN ) ? 1.0f : 0.0f;

    // CSM: Alle Cascade-Matrizen setzen

    for ( size_t cascadeIdx = 0; cascadeIdx < MAX_CSM_CASCADES; ++cascadeIdx ) {
        XMStoreFloat4x4( &scb.SQ_ShadowViewProj[cascadeIdx],
            XMLoadFloat4x4( &m_CascadeCRs[cascadeIdx].ProjectionReplacement ) *
                XMLoadFloat4x4( &m_CascadeCRs[cascadeIdx].ViewReplacement )
        );
    }

    scb.SQ_ShadowmapSize = static_cast<float>( this->GetSizeX() );

    // Atlas path: fill per-cascade UV rects for shader atlas sampling
    if ( m_useAtlas && m_shadowAtlas ) {
        for ( size_t i = 0; i < MAX_CSM_CASCADES; ++i ) {
            scb.SQ_CascadeAtlasRect[i] = m_shadowAtlas->GetCascadeUVRect( static_cast<UINT>( i ) );
        }
    }

    // Precompute world-space units-per-texel for each cascade so shaders can read
    // SQ_CascadeTexelSize[i] instead of running per-fragment matrix math.
    {
        const float mapSize = static_cast<float>( this->GetSizeX() );
        float* ts = scb.SQ_CascadeTexelSize.toPtr();
        for ( size_t i = 0; i < MAX_CSM_CASCADES; ++i ) {
            const XMFLOAT4X4& m = scb.SQ_ShadowViewProj[i];
            const float sx = sqrtf( m._11 * m._11 + m._21 * m._21 + m._31 * m._31 );
            const float sy = sqrtf( m._12 * m._12 + m._22 * m._22 + m._32 * m._32 );
            const float wx = ( sx > 1e-6f ) ? ( 2.0f / sx ) : 0.0f;
            const float wy = ( sy > 1e-6f ) ? ( 2.0f / sy ) : 0.0f;
            // Cascade resolution in pixels - in atlas mode this is the sub-rect's own size, not
            // the atlas-relative UV scale (which is half of it and doubled the normal offset).
            const float res = m_useAtlas
                ? static_cast<float>( GetCascadePixelSize( static_cast<UINT>( i ) ) )
                : mapSize;
            ts[i] = 0.5f * ( wx + wy ) / std::max( res, 1.0f );
        }
    }

    // Get rain matrix
    
    XMStoreFloat4x4( &scb.SQ_RainViewProj,
        XMLoadFloat4x4( &graphicsEngine->Effects->GetRainShadowmapCameraRepl().ProjectionReplacement ) *
        XMLoadFloat4x4( &graphicsEngine->Effects->GetRainShadowmapCameraRepl().ViewReplacement )
    );

    scb.SQ_ShadowStrength = settings.ShadowStrength;
    scb.SQ_ShadowAOStrength = settings.ShadowAOStrength;
    scb.SQ_WorldAOStrength = settings.WorldAOStrength;
    scb.SQ_ShadowSoftness = settings.ShadowSoftness;
    scb.SQ_LightSize = std::clamp( settings.PCSSLightSize, 0.005f, 0.5f );

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

            // Only use world AO - shadow AO would multiply the same baked vertLighting in a second
            // time and over-darken the interior.
            scb.SQ_WorldAOStrength = 1.0f;
            scb.SQ_ShadowAOStrength = 0.0f;
            // Darken the lights
            scb.SQ_LightColor = float4( 1, 1, 1, DEFAULT_INDOOR_VOB_AMBIENT.x );
        }

    psAtmo->UpdateBuffer("DS_ScreenQuadConstantBuffer", &scb, sizeof(scb));

    // CSM: Bind the cascade array to a single slot (Texture2DArray)
    BindToPixelShader( m_context.Get(), TX_ShadowmapArray );

    if ( graphicsEngine->Effects->GetRainShadowmap() )
        graphicsEngine->Effects->GetRainShadowmap()->BindToPixelShader( m_context.Get(), TX_RainShadowmap );

    this->BindSampler( m_context.Get(), 2 );

    m_context->PSSetShaderResources( TX_ReflectionCube, 1, graphicsEngine->ReflectionCube2.GetAddressOf() );

    graphicsEngine->GetDistortionTexture()->BindToPixelShader( TX_Distortion );
    graphicsEngine->GetBlueNoiseTexture()->BindToPixelShader( TX_BlueNoise512 );

    // Screen-space AO mask (applied to indirect light only). White fallback when disabled.
    ID3D11ShaderResourceView* aoSRV = aoMaskSRV ? aoMaskSRV
        : graphicsEngine->GetWhiteTexture()->GetShaderResourceView().Get();
    m_context->PSSetShaderResources( TX_AOMask, 1, &aoSRV );

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
    const RenderToDepthStencilBuffer& targetCube,
    const ComPtr<ID3D11DepthStencilView>& face,
    const ComPtr<ID3D11RenderTargetView>& debugRTV, bool cullFront, bool indoor, bool noNPCs,
    std::list<VobInfo*>* renderedVobs,
    std::list<SkeletalVobInfo*>* renderedMobs,
    std::vector<MeshDrawRange>* worldMeshCache,
    bool clearDepth,
    unsigned int casterMask,
    const std::move_only_function<bool(const zCVob*) const>& ignoreVob ) {

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

    ID3D11DepthStencilView*  activeFace = face.Get();
    bool useLayeredPath = false;
    if ( !activeFace ) {
        if ( Engine::GAPI->GetRendererState().RendererSettings.DebugSettings.FeatureSet.UseLayeredRendering ) {
            useLayeredPath = true;
            activeFace = targetCube.GetDepthStencilView().Get();

            // Set layered shader
            graphicsEngine->SetActiveVertexShader( VShaderID::VS_ExLayered );
        } else if (!graphicsEngine->IsCubeFaceFallbackActive()) {
            // Set cubemap shader
            graphicsEngine->SetActiveGShader( GShaderID::GS_Cubemap );
            graphicsEngine->GetActiveGS()->Apply();
            activeFace = targetCube.GetDepthStencilView().Get();

            graphicsEngine->SetActiveVertexShader( VShaderID::VS_ExCube );
        }
    }
    if (graphicsEngine->IsCubeFaceFallbackActive()) {
        // VS_Ex's extra outputs shift SV_POSITION's register vs. what PS_CubeShadow expects; VS_ExCubeFace matches.
        graphicsEngine->SetActiveVertexShader( VShaderID::VS_ExCubeFace );
    }

    // Set the rendering stage
    D3D11ENGINE_RENDER_STAGE oldStage = graphicsEngine->GetRenderingStage();
    graphicsEngine->SetRenderingStage( DES_SHADOWMAP_CUBE );

    ID3D11ShaderResourceView* nullSrv = nullptr;
    m_context->PSSetShaderResources( 3, 1, &nullSrv );

    const bool oldColorWrites = Engine::GAPI->GetRendererState().BlendState.ColorWritesEnabled;

    if ( !debugRTV.Get() ) {
        m_context->OMSetRenderTargets( 0, nullptr, activeFace );

        // Depth-only now that the caster writes no SV_Depth (it used to need color writes on for that).
        Engine::GAPI->GetRendererState().BlendState.ColorWritesEnabled = false;
        Engine::GAPI->GetRendererState().BlendState.SetDirty();
    } else {
        m_context->OMSetRenderTargets( 1, debugRTV.GetAddressOf(), activeFace );

        Engine::GAPI->GetRendererState().BlendState.ColorWritesEnabled = true;
        Engine::GAPI->GetRendererState().BlendState.SetDirty();
    }

    if ( clearDepth ) {
        m_context->ClearDepthStencilView( activeFace, D3D11_CLEAR_DEPTH, 1.0f, 0 );
    }

    // Draw the world mesh without textures
    if ( useLayeredPath ) {
        graphicsEngine->DrawWorldAround_Layered( position, range, cullFront, indoor, noNPCs, renderedVobs,
            renderedMobs, worldMeshCache, casterMask, ignoreVob );
    } else {
        graphicsEngine->DrawWorldAround( position, range, cullFront, indoor, noNPCs, renderedVobs,
            renderedMobs, worldMeshCache, casterMask, ignoreVob );
    }

    // Restore state
    graphicsEngine->SetRenderingStage( oldStage );
    m_context->RSSetViewports( 1, &oldVP );
    m_context->GSSetShader( nullptr, nullptr, 0 );
    graphicsEngine->SetActiveVertexShader( VShaderID::VS_Ex );

    Engine::GAPI->GetRendererState().BlendState.ColorWritesEnabled = oldColorWrites;
    Engine::GAPI->GetRendererState().BlendState.SetDirty();

    Engine::GAPI->SetFarPlane(
        Engine::GAPI->GetRendererState().RendererSettings.SectionDrawRadius *
        WORLD_SECTION_SIZE );

    graphicsEngine->SetRenderingStage( DES_MAIN );
}
