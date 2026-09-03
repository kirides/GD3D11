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

XRESULT D3D11ShadowMap::DrawPointlightShadows( std::vector<VobLightInfo*>& lights ) {
    ZoneScopedN( "DrawPointlightShadows" );

    auto& settings = Engine::GAPI->GetRendererState().RendererSettings;

    // Release resources of lights that have been out of view for a while now - NOT on the very first
    // missing frame. A light that only blinks (zCVobLight::IsEnabled toggling for a flicker effect, or a
    // one-frame frustum/visibility edge case) must keep its slot and baked depth across that blink: a
    // PLS_STATIC_ONLY light's bake is meant to happen once and stick, but if every absent frame evicted it
    // outright, a light that blinks even occasionally could never finish a bake that survives - it gets
    // evicted mid-wait, wins a slot again on the next visible frame, and gets evicted again before that
    // bake completes, so it lights unshadowed (bleeding through walls) forever instead of self-healing.
    // Mirrors D3D12PointShadows' Slot::missingFrames retention in SelectShadowedLights.
    constexpr int kPointLightSlotRetentionFrames = 120;
    for ( auto& it : Engine::GAPI->VobLightMap ) {
        if ( !it.second->LightShadowBuffers ) continue;
        if ( D3D11PointLight* pl = dynamic_cast<D3D11PointLight*>(it.second->LightShadowBuffers.get()) ) {
            const bool visible = it.second->Vob->IsEnabled() && it.second->VisibleInFrame;
            if ( pl->NoteAbsence( visible, kPointLightSlotRetentionFrames ) ) {
                pl->ClearTiledSlot();
                pl->ReleaseShadowMap();
            }
        }
    }

    if (settings.EnablePointlightShadows <= 0) {
        return XR_SUCCESS;
    }
    
    auto graphicsEngine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    auto _ = graphicsEngine->RecordGraphicsEvent( GE_NAME( "DrawPointlightShadows" ) );

    static const XMVECTORF32 xmFltMax = { { { FLT_MAX, FLT_MAX, FLT_MAX, FLT_MAX } } };
    graphicsEngine->SetDefaultStates();

    // ********************************
    // Draw world shadows
    // ********************************
    const XMVECTOR cameraPositionXm = Engine::GAPI->GetCameraPositionXM();
    XMFLOAT3 cameraPosition;
    XMStoreFloat3( &cameraPosition, cameraPositionXm );
    XMVECTOR vPlayerPosition =
        Engine::GAPI->GetPlayerVob() != nullptr
        ? Engine::GAPI->GetPlayerVob()->GetPositionWorldXM()
        : xmFltMax;

    bool partialShadowUpdate = settings.PartialDynamicShadowUpdates;
    const bool staticOnlyMode = settings.EnablePointlightShadows == GothicRendererSettings::PLS_STATIC_ONLY;

    // Draw pointlight shadows
    static std::vector<std::pair<float, VobLightInfo*>> importantUpdates;
    importantUpdates.clear();

    DepthStencilPool* dsPool = graphicsEngine->GetPfxRenderer()->GetDepthStencilPool();

    const bool isTiledShadingEnabled = m_TiledDeferred && settings.EnableTiledLighting;
    const int requiredShadowMapKind = isTiledShadingEnabled ? 1 : 0;

    // Tiled lights share small fixed-size cube-array pools (unlike legacy, where every light gets its own
    // unbounded DepthStencilPool allocation), so requests are ranked closest-to-player and may evict the
    // farthest current occupant under pressure - mirrors D3D12PointShadows' SelectShadowedLights. Two
    // independent pools: WantsStaticOnlySlot() lights route to the low-res MAX_STATIC_SHADOW_CUBEMAPS pool
    // instead of the full-res MAX_SHADOW_CUBEMAPS one.
    struct PendingAcquire { VobLightInfo* light; D3D11PointLight* pl; float distSq; int desiredResolution;
        bool wantsLowTier; bool spatiallyStatic; };
    static std::vector<PendingAcquire> candidates;
    candidates.clear();

    // Lights that asked for a cube this frame and could not be given one in either tier. Surfaced in the
    // ImGui point-light window: a starved static light is range-clamped by the tiled light fill below, which
    // makes it look switched off, and that is otherwise indistinguishable from a bug in slot assignment.
    unsigned int starvedThisFrame = 0;

    static std::array<VobLightInfo*, MAX_SHADOW_CUBEMAPS> slotOwnerLightHigh;
    slotOwnerLightHigh.fill( nullptr );
    static std::array<VobLightInfo*, MAX_STATIC_SHADOW_CUBEMAPS> slotOwnerLightLow;
    slotOwnerLightLow.fill( nullptr );

    // A slot must be held by a light ~1.7x farther than a challenger to be evicted, to avoid thrashing
    // between similarly-distant lights every frame (mirrors D3D12PointShadows' kIncumbentBias).
    constexpr float kPointShadowIncumbentBias = 0.35f;

    auto allocateOrEvictSlot = [&]( float requesterDistSq, auto& slotOwnerArray, uint32_t poolSize, auto&& allocate ) -> int {
        int slot = allocate();
        if ( slot >= 0 ) return slot;

        int worstSlot = -1;
        float worstDistSq = -1.0f;
        for ( uint32_t s = 0; s < poolSize; s++ ) {
            VobLightInfo* owner = slotOwnerArray[s];
            if ( !owner ) continue;
            const float ownerDistSq = XMVectorGetX( XMVector3LengthSq( owner->Vob->GetPositionWorldXM() - vPlayerPosition ) );
            if ( ownerDistSq > worstDistSq ) {
                worstDistSq = ownerDistSq;
                worstSlot = static_cast<int>(s);
            }
        }
        if ( worstSlot < 0 || requesterDistSq * kPointShadowIncumbentBias >= worstDistSq ) {
            return -1;
        }
        VobLightInfo* worstOwner = slotOwnerArray[worstSlot];
        D3D11PointLight* worstPl = dynamic_cast<D3D11PointLight*>(worstOwner->LightShadowBuffers.get());
        if ( !worstPl ) return -1;

        worstPl->ClearTiledSlot();
        slotOwnerArray[worstSlot] = nullptr;
        return allocate();
    };

    auto classifyLight = [&]( VobLightInfo* light, D3D11PointLight* pl, float distSq ) {
        bool needsUpdate = pl->NeedsUpdate();
        bool isInited = pl->IsInited();

        if ( isInited ) {
            if ( needsUpdate || light->UpdateShadows ) {
                importantUpdates.emplace_back( distSq, light );
            }
            else if ( partialShadowUpdate && !staticOnlyMode ) {
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
        }
    };

    for ( auto const& light : lights ) {
        if ( !light->Vob->IsEnabled() || !light->VisibleInFrame ) {
            continue;
        }
        // Create shadowmap in case we should have one but haven't got it yet
        if ( !light->LightShadowBuffers && light->UpdateShadows ) {
            BaseShadowedPointLight* bpl = nullptr;
            // assume this is a dynamic light
            graphicsEngine->CreateShadowedPointLight( &bpl, light, /*dynamic light*/ true );
            light->LightShadowBuffers.reset(bpl);
        }

        if ( D3D11PointLight* pl = dynamic_cast<D3D11PointLight*>(light->LightShadowBuffers.get()) ) {
            pl->NoteStationary();   // feeds IsSpatiallyStatic(), i.e. whether this light may spill (see below)
            bool wantsLowTier = isTiledShadingEnabled && pl->WantsStaticOnlySlot();

            if ( isTiledShadingEnabled ) {
                int ownSlot = pl->GetTiledSlot();
                if ( ownSlot >= 0 ) {
                    if ( pl->IsTiledSlotLowRes() && static_cast<uint32_t>(ownSlot) < MAX_STATIC_SHADOW_CUBEMAPS ) {
                        slotOwnerLightLow[ownSlot] = light;
                    } else if ( !pl->IsTiledSlotLowRes() && static_cast<uint32_t>(ownSlot) < MAX_SHADOW_CUBEMAPS ) {
                        slotOwnerLightHigh[ownSlot] = light;
                    }
                }
            }

            const float d = XMVectorGetX( XMVector3LengthSq( light->Vob->GetPositionWorldXM() - vPlayerPosition ) );
            float range = light->Vob->GetLightRange();
            const float rangeSq = range * range;

            float distVeryCloseSq = (range * 0.8f) * (range * 0.8f);
            // Shadow-candidacy horizon. range*9 alone puts it at ~13 m for a candle (range ~150 units), and a
            // light that falls outside it is shaded UNSHADOWED - which the tiled light fill then range-clamps
            // to 0.35x/0.15x, collapsing the lit patch around the candle so it reads as switched OFF at a
            // distance where it is still plainly visible. The horizon has to be about where the light stops
            // being SEEN, not where its own falloff sphere stops reaching the camera, so it gets an absolute
            // floor as well. Mirrors D3D12PointShadows::SelectShadowedLights' kMinShadowDist.
            constexpr float kMinShadowDist = 3000.0f;   // Gothic world units (~100 = 1 m)
            const float maxShadowDist = std::max( range * 9.0f, kMinShadowDist );
            float distMaxShadowSq = maxShadowDist * maxShadowDist; // Fade out entirely after this

            // Resolution also doubles as the tier-mismatch check below: a light whose tier flipped no longer
            // matches its current GetShadowMapResolution(), so it re-acquires into the right tier automatically.
            int desiredResolution = wantsLowTier ? STATIC_SHADOW_CUBE_SIZE : SHADOW_CUBE_SIZE;
            if ( d < distVeryCloseSq && !staticOnlyMode ) {
                light->UpdateShadows = true;
                // for now, keep all lights/shadows the same size, otherwise they change their "volume"
                // desiredResolution = 256; // High res for close lights
            }

            bool inShadowRange = d < distMaxShadowSq;
            if ( inShadowRange ) {
                if ( !pl->HasShadowMap( requiredShadowMapKind ) || pl->GetShadowMapResolution() != desiredResolution ) {
                    candidates.push_back( { light, pl, d, desiredResolution, wantsLowTier, pl->IsSpatiallyStatic() } );
                    continue;
                }

                classifyLight( light, pl, d );
            } else {
                if ( pl->HasAnyShadowMap() ) {
                    // Past the horizon: stop UPDATING the cube, but in the tiled path do not throw it away.
                    // The depth in a tiled slot is still perfectly good and still sampled, and dropping it
                    // turned "too far to be worth re-rendering" into "unshadowed", i.e. into the range clamp
                    // that makes a light look switched off. Slots are reclaimed under real pressure instead
                    // (allocateOrEvictSlot takes the farthest owner) or by the absence retention above.
                    // The legacy non-tiled path keeps releasing: there every light owns an unbounded
                    // DepthStencilPool allocation, so holding one for a distant light is a real cost.
                    if ( !isTiledShadingEnabled ) {
                        pl->ClearTiledSlot();
                        pl->ReleaseShadowMap();
                    }

                    auto it = std::find( graphicsEngine->FrameShadowUpdateLights.begin(), graphicsEngine->FrameShadowUpdateLights.end(), light );
                    if ( it != graphicsEngine->FrameShadowUpdateLights.end() ) {
                        graphicsEngine->FrameShadowUpdateLights.erase( it );
                    }

                    auto importantIt = std::find_if( importantUpdates.begin(), importantUpdates.end(),
                        [&]( const auto& entry ) { return entry.second == light; } );
                    if ( importantIt != importantUpdates.end() ) {
                        importantUpdates.erase( importantIt );
                    }
                }
            }
        }
    }

    std::sort( candidates.begin(), candidates.end(), []( const PendingAcquire& a, const PendingAcquire& b ) {
        return a.distSq < b.distSq;
    } );

    for ( auto& c : candidates ) {
        D3D11PointLight* pl = c.pl;
        VobLightInfo* light = c.light;

        if ( isTiledShadingEnabled ) {
            // Find the new slot BEFORE giving up the one this light already holds. Releasing first and then
            // failing to allocate left the light with no cube at all - and no cube is what the range clamp
            // turns into a light that visibly switches off. This is also what makes PROMOTION safe: a light
            // that spilled into the low-res tier asks for the full-res one again every frame (its preferred
            // resolution no longer matches its current one, which is what put it in `candidates`), and it
            // only lets go of the cube it has once the better slot is actually in hand.
            bool useLowTier = c.wantsLowTier;
            int slot = -1;
            if ( useLowTier ) {
                slot = allocateOrEvictSlot( c.distSq, slotOwnerLightLow, MAX_STATIC_SHADOW_CUBEMAPS,
                    [&] { return m_TiledDeferred->AllocateStaticSlot(); } );
            } else if ( c.desiredResolution != SHADOW_CUBE_SIZE ) {
                // only one high-tier resolution is ever used; don't put a mismatched size into a tiled slot
                light->UpdateShadows = false;
                continue;
            } else {
                slot = allocateOrEvictSlot( c.distSq, slotOwnerLightHigh, MAX_SHADOW_CUBEMAPS,
                    [&] { return m_TiledDeferred->AllocateSlot(); } );
                // SPILL. The full-res pool is full of lights no farther away than this one, but a light that
                // never moves has a cacheable cube, and a 32^2 cached cube is enormously better than none: it
                // gives up its dynamic overlay (that tier has no overlay array - see GetCurrentShadowMode)
                // and keeps its shadow. Without this a room full of fixed lights left everything past the
                // pool size unshadowed while the static tier sat empty.
                if ( slot < 0 && c.spatiallyStatic ) {
                    if ( pl->GetTiledSlot() >= 0 && pl->IsTiledSlotLowRes() ) {
                        // Already spilled, and still nothing in the full-res pool: keep the cube it has.
                        // Taking a DIFFERENT low-res slot instead would drop the bake and re-render it, and
                        // since this light asks to be promoted again every single frame, that is a re-bake
                        // every frame for as long as the full-res tier stays full.
                        light->UpdateShadows = false;
                        classifyLight( light, pl, c.distSq );
                        continue;
                    }
                    slot = allocateOrEvictSlot( c.distSq, slotOwnerLightLow, MAX_STATIC_SHADOW_CUBEMAPS,
                        [&] { return m_TiledDeferred->AllocateStaticSlot(); } );
                    if ( slot >= 0 ) useLowTier = true;
                }
            }

            if ( slot < 0 ) {
                // Nothing to be had in either tier. Keep whatever this light already owns rather than
                // dropping it - an older cube, even a stale one, beats going unshadowed.
                light->UpdateShadows = false;
                ++starvedThisFrame;
                continue;
            }

            // Read the previous slot only now: allocateOrEvictSlot may have evicted THIS light (it is a
            // candidate for "farthest owner" like any other), in which case it has already been cleared.
            const int prevSlot = pl->GetTiledSlot();
            if ( prevSlot >= 0 ) {
                if ( pl->IsTiledSlotLowRes() ) {
                    if ( static_cast<uint32_t>(prevSlot) < MAX_STATIC_SHADOW_CUBEMAPS ) slotOwnerLightLow[prevSlot] = nullptr;
                } else if ( static_cast<uint32_t>(prevSlot) < MAX_SHADOW_CUBEMAPS ) {
                    slotOwnerLightHigh[prevSlot] = nullptr;
                }
                pl->ClearTiledSlot();
            }
            pl->ReleaseShadowMap();

            if ( useLowTier ) {
                pl->SetTiledSlot( slot, m_TiledDeferred->GetStaticSlotTarget( slot ), m_TiledDeferred.get(), true );
                slotOwnerLightLow[slot] = light;
                pl->SetCurrentResolution( STATIC_SHADOW_CUBE_SIZE );
            } else {
                pl->SetTiledSlot( slot, m_TiledDeferred->GetSlotTarget( slot ), m_TiledDeferred.get(), false );
                slotOwnerLightHigh[slot] = light;
                pl->SetCurrentResolution( SHADOW_CUBE_SIZE );
            }
        } else {
            pl->ClearTiledSlot();
            pl->ReleaseShadowMap();
            pl->AcquireShadowMap( dsPool, c.desiredResolution );
        }

        light->UpdateShadows = true;
        classifyLight( light, pl, c.distSq );
    }

    // Slot occupancy + starvation for the ImGui point-light window (DrawPointLightSlotStat). Occupancy comes
    // from the pools themselves, not from the per-frame owner arrays: those only list slots held by lights
    // VISIBLE this frame, while a slot stays allocated across the absence-retention window.
    {
        auto& info = Engine::GAPI->GetRendererState().RendererInfo;
        if ( isTiledShadingEnabled ) {
            info.PointLightSlotsUsed = m_TiledDeferred->GetUsedSlotCount();
            info.PointLightSlotsMax = MAX_SHADOW_CUBEMAPS;
            info.PointLightStaticSlotsUsed = m_TiledDeferred->GetUsedStaticSlotCount();
            info.PointLightStaticSlotsMax = MAX_STATIC_SHADOW_CUBEMAPS;
            info.PointLightSlotsStarved = starvedThisFrame;
        } else {
            // Legacy per-light cubemaps have no fixed pools to report on; zero Max hides the row entirely.
            info.PointLightSlotsMax = 0;
            info.PointLightStaticSlotsMax = 0;
            info.PointLightSlotsStarved = 0;
        }
    }

    // Render the immediate priority lights - but never more than a handful in one frame.
    //
    // A global shadow-mode toggle dirties EVERY light at once. Each rebuild allocates its cubemap
    // view-matrix CB from the 4 MB per-frame ring and keeps it bound at VS b3 / GS b2 across every draw of
    // both its passes, while those draws keep allocating from that same ring. Once the ring wraps
    // (ConstantBufferPool.cpp:61 resets the offset to 0 and overwrites in place) the earlier lights' view
    // matrices are replaced mid-frame, so their cubes finish rendering with another light's projection -
    // which shows up as point-light shadows randomly cut off along a cube-face edge, on a different set of
    // lights every time. Overflow keeps its UpdateShadows flag and drains through the round-robin below.
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
    // Set a strict, safe limit to prevent FPS drops. 
    // 2 per frame is 120 updates per second at 60fps.
    int maxBackgroundUpdates = 2;
    int updatesDone = 0;

    while ( !graphicsEngine->FrameShadowUpdateLights.empty() && updatesDone < maxBackgroundUpdates ) {
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

        updatesDone++;
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
        // VS_Ex's extra outputs shift SV_POSITION's register vs. what PS_LinDepth expects; VS_ExLinDepth matches.
        graphicsEngine->SetActiveVertexShader( VShaderID::VS_ExLinDepth );
    }

    // Set the rendering stage
    D3D11ENGINE_RENDER_STAGE oldStage = graphicsEngine->GetRenderingStage();
    graphicsEngine->SetRenderingStage( DES_SHADOWMAP_CUBE );

    ID3D11ShaderResourceView* nullSrv = nullptr;
    m_context->PSSetShaderResources( 3, 1, &nullSrv );

    if ( !debugRTV.Get() ) {
        m_context->OMSetRenderTargets( 0, nullptr, activeFace );

        Engine::GAPI->GetRendererState().BlendState.ColorWritesEnabled =
            true;  // Should be false, but needs to be true for SV_Depth to work
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

    Engine::GAPI->SetFarPlane(
        Engine::GAPI->GetRendererState().RendererSettings.SectionDrawRadius *
        WORLD_SECTION_SIZE );

    graphicsEngine->SetRenderingStage( DES_MAIN );
}
