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
    // Atlas path uses CLAMP to prevent seam bleeding at cascade boundaries.
    // Texture array path uses WRAP for compatibility with previous behavior.
    auto addressMode = m_useAtlas ? D3D11_TEXTURE_ADDRESS_CLAMP : D3D11_TEXTURE_ADDRESS_WRAP;
    samplerDesc.AddressU = addressMode;
    samplerDesc.AddressV = addressMode;
    samplerDesc.AddressW = addressMode;
    samplerDesc.MipLODBias = 0;
    samplerDesc.MaxAnisotropy = 1;
    samplerDesc.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
    samplerDesc.MinLOD = -FLT_MAX;
    samplerDesc.MaxLOD = FLT_MAX;

    m_shadowmapSampler.Reset();
    HRESULT hr;
    LE( m_device->CreateSamplerState( &samplerDesc, m_shadowmapSampler.GetAddressOf() ) );
    SetDebugName( m_shadowmapSampler.Get(), "ShadowmapSamplerState" );
}

void D3D11ShadowMap::EnsureShadowMapBackend( int size ) {
    if ( !m_device ) return;
     
    const auto& settings = Engine::GAPI->GetRendererState().RendererSettings;
    const UINT atlasNumCascades = static_cast<UINT>( std::clamp<int>( settings.NumShadowCascades, 1, std::min(4, MAX_CSM_CASCADES) ) );

    bool desiredUseAtlas = ShouldUseAtlas();
    int clampedSize = std::min<int>( std::max<int>( size, 512 ), (FeatureLevel10Compatibility ? 8192 : 16384) );

    if ( desiredUseAtlas != m_useAtlas ) {
        // Switch backend at runtime.
        m_useAtlas = desiredUseAtlas;

        if ( m_useAtlas ) {
            m_cascadedShadowMap.reset();
            m_shadowAtlas = std::make_unique<D3D11ShadowAtlas>();
            const int maxAtlasCascade0Size = (atlasNumCascades <= 1) ? clampedSize : (clampedSize / 2);
            int atlasCascade0Size = std::min<int>( clampedSize, maxAtlasCascade0Size );
            m_shadowAtlas->Init( m_device, atlasCascade0Size, atlasNumCascades );
        } else {
            m_shadowAtlas.reset();
            m_cascadedShadowMap = std::make_unique<D3D11CascadedShadowMapBuffer>();
            m_cascadedShadowMap->Init( m_device, clampedSize, MAX_CSM_CASCADES );
        }

        // Sampler addressing depends on atlas/array path.
        RecreateShadowSampler();

        // SHADOW_ATLAS is a compile-time shader macro; reload relevant shaders when mode flips.
        auto* graphicsEngine = reinterpret_cast<D3D11GraphicsEngine*>( Engine::GraphicsEngine );
        if ( graphicsEngine ) {
            graphicsEngine->ReloadShaders( ShaderCategory::LightsAndShadows );
        }
    }

    // Ensure resources exist even if no mode switch occurred.
    if ( m_useAtlas && !m_shadowAtlas ) {
        m_shadowAtlas = std::make_unique<D3D11ShadowAtlas>();
        const int maxAtlasCascade0Size = (atlasNumCascades <= 1) ? clampedSize : (clampedSize / 4);
        int atlasCascade0Size = std::min<int>( clampedSize, maxAtlasCascade0Size );
        m_shadowAtlas->Init( m_device, atlasCascade0Size, atlasNumCascades );
    } else if ( m_useAtlas && m_shadowAtlas ) {
        const int maxAtlasCascade0Size = (atlasNumCascades <= 1) ? clampedSize : (clampedSize / 4);
        int atlasCascade0Size = std::min<int>( clampedSize, maxAtlasCascade0Size );
        m_shadowAtlas->Resize( atlasCascade0Size, atlasNumCascades );
    } else if ( !m_useAtlas && !m_cascadedShadowMap ) {
        m_cascadedShadowMap = std::make_unique<D3D11CascadedShadowMapBuffer>();
        m_cascadedShadowMap->Init( m_device, clampedSize, MAX_CSM_CASCADES );
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
    HRESULT hr;
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

    D3D11GraphicsEngineBase* engine = reinterpret_cast<D3D11GraphicsEngineBase*>( Engine::GraphicsEngine );

    // Create constantbuffer for the view-matrices
    D3D11ConstantBuffer* cb = nullptr;
    LE(engine->CreateConstantBuffer( &cb, nullptr, sizeof( CubemapGSConstantBuffer ) ));
    m_PointLightCB.reset( cb );

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
    const UINT atlasNumCascades = static_cast<UINT>( std::clamp<int>( settings.NumShadowCascades, 1, std::min( 4, MAX_CSM_CASCADES ) ) );

    EnsureShadowMapBackend( s );

    if ( m_useAtlas ) {
        // Atlas path: with one cascade, use full hardware limit; otherwise reserve width for atlas packing.
        const int maxAtlasCascade0Size = (atlasNumCascades <= 1) ? maxSize : (maxSize / 4);
        int atlasCascade0Size = std::min<int>( s, maxAtlasCascade0Size );
        if ( m_shadowAtlas ) {
            m_shadowAtlas->Resize( atlasCascade0Size, atlasNumCascades );
        }
    } else {
        // Texture array path
        if ( m_cascadedShadowMap ) {
            m_cascadedShadowMap->Resize( s );
        }
    }

    m_lastNumCascades = static_cast<int>( atlasNumCascades );
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
        const int maxSize = FeatureLevel10Compatibility ? 8192 : 16384;
        const int desiredSize = std::min<int>( std::max<int>( settings.ShadowMapSize, 512 ), maxSize );
        const int desiredCascades = std::clamp( settings.NumShadowCascades, 1, MAX_CSM_CASCADES );

        if ( GetSizeX() != desiredSize
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
        bool lazyCascadeUpdate = m_useAtlas // atlas breaks when last cascade is not rendered, as we clear the atlas for the next pass.
            ? false 
            : settings.DebugSettings.ShadowCascades.LazyCascadeUpdate;

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
            if ( lazyCascadeUpdate ) {
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
    }

    if ( settings.ThreadedShadowCulling ) {
        std::lock_guard<LockableBase( std::mutex )> lock( m_CullingJobsMutex );
        m_ShadowCullingJobs.clear();

        for ( size_t i = 0; i < static_cast<size_t>(numCascades); i++ ) {
            m_RenderQueues[i]->Reset();
            if ( !m_ShouldUpdateCascade[i] ) {
                continue; // Skip culling for this cascade if we're not updating it this frame
            }

            m_ShadowCullingJobs.push_back( Engine::WorkerThreadPool->enqueue( []( const CancellationToken& token, D3D11ShadowMap* _this, size_t idx ) {
                if ( token.isCancelled() ) {
                    return;
                }
                ZoneScoped;
                ZoneNameF( "Shadow Cascade %zu", idx );

                RndCullContext ctx;
                ctx.queue = _this->m_RenderQueues[idx].get();
                ctx.frustum = _this->m_CascadeCRs[idx].frustum;
                ctx.cameraPosition = _this->m_WorldShadowPos;
                ctx.stage = RenderStage::STAGE_DRAW_SHADOWS;
                ctx.drawDistances.OutdoorVobs = 20000;
                ctx.drawDistances.OutdoorVobsSmall = 20000;
                ctx.drawDistances.IndoorVobs = 20000;
                ctx.drawDistances.VisualFX = 0.0f;
                ctx.drawDistancesSq.OutdoorVobs = ctx.drawDistances.OutdoorVobs * ctx.drawDistances.OutdoorVobs;
                ctx.drawDistancesSq.OutdoorVobsSmall = ctx.drawDistances.OutdoorVobsSmall * ctx.drawDistances.OutdoorVobsSmall;
                ctx.drawDistancesSq.IndoorVobs = ctx.drawDistances.IndoorVobs * ctx.drawDistances.IndoorVobs;
                ctx.drawDistancesSq.VisualFX = 0.0f;

                const auto& rs = Engine::GAPI->GetRendererState().RendererSettings;
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

        ctx.queue = &q;
        ctx.frustum = frustum;
        ctx.cameraPosition = m_WorldShadowPos;
        ctx.stage = RenderStage::STAGE_DRAW_SHADOWS;
        ctx.drawDistances.OutdoorVobs = 20000;
        ctx.drawDistances.OutdoorVobsSmall = 20000;
        ctx.drawDistances.IndoorVobs = 20000;
        ctx.drawDistances.VisualFX = 0.0f;
        ctx.drawDistancesSq.OutdoorVobs = ctx.drawDistances.OutdoorVobs * ctx.drawDistances.OutdoorVobs;
        ctx.drawDistancesSq.OutdoorVobsSmall = ctx.drawDistances.OutdoorVobsSmall * ctx.drawDistances.OutdoorVobsSmall;
        ctx.drawDistancesSq.IndoorVobs = ctx.drawDistances.IndoorVobs * ctx.drawDistances.IndoorVobs;
        ctx.drawDistancesSq.VisualFX = 0.0f;

        const auto& rs = Engine::GAPI->GetRendererState().RendererSettings;
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

    // Release any resources of not visible lights
    for ( auto& it : Engine::GAPI->VobLightMap ) {
        if ( it.second->LightShadowBuffers
            && (!it.second->Vob->IsEnabled() || !it.second->VisibleInFrame) ) {
            if ( D3D11PointLight* pl = dynamic_cast<D3D11PointLight*>(it.second->LightShadowBuffers.get()) ) {
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
    FXMVECTOR vPlayerPosition =
        Engine::GAPI->GetPlayerVob() != nullptr
        ? Engine::GAPI->GetPlayerVob()->GetPositionWorldXM()
        : xmFltMax;

    bool partialShadowUpdate = settings.PartialDynamicShadowUpdates;
    const bool staticOnlyMode = settings.EnablePointlightShadows == GothicRendererSettings::PLS_STATIC_ONLY;

    // Draw pointlight shadows
    std::list<VobLightInfo*> importantUpdates;

    DepthStencilPool* dsPool = graphicsEngine->GetPfxRenderer()->GetDepthStencilPool();
    
    const bool isTiledShadingEnabled = m_TiledDeferred && settings.EnableTiledLighting;
    const int requiredShadowMapKind = isTiledShadingEnabled ? 1 : 0;
    for ( auto const& light : lights ) {
        if ( !light->Vob->IsEnabled() || !light->VisibleInFrame ) {
            continue;
        }
        // Create shadowmap in case we should have one but haven't got it yet
        if ( !light->LightShadowBuffers && light->UpdateShadows ) {
            BaseShadowedPointLight* bpl;
            // assume this is a dynamic light
            graphicsEngine->CreateShadowedPointLight( &bpl, light, /*dynamic light*/ true );
            light->LightShadowBuffers.reset(bpl);
        }

        if ( D3D11PointLight* pl = dynamic_cast<D3D11PointLight*>(light->LightShadowBuffers.get()) ) {
            const float d = XMVectorGetX( XMVector3LengthSq( light->Vob->GetPositionWorldXM() - vPlayerPosition ) );
            float range = light->Vob->GetLightRange();
            const float rangeSq = range * range;

            float distVeryCloseSq = (range * 0.8f) * (range * 0.8f);
            float distMaxShadowSq = (range * 9.0f) * (range * 9.0f); // Fade out entirely after this

            // pick shadow resolution based on distance.
            int desiredResolution = SHADOW_CUBE_SIZE; // Fallback / far distance
            if ( d < distVeryCloseSq && !staticOnlyMode ) {
                light->UpdateShadows = true;
                // for now, keep all lights/shadows the same size, otherwise they change their "volume"
                // desiredResolution = 256; // High res for close lights
            }

            bool inShadowRange = d < distMaxShadowSq;
            if ( inShadowRange ) {
                // Acquire memory if it doesn't have it (or resolution changed)
                if ( !pl->HasShadowMap( requiredShadowMapKind ) || pl->GetShadowMapResolution() != desiredResolution ) {
                    pl->ClearTiledSlot();
                    pl->ReleaseShadowMap();

                    // Try tiled slot for small (64×64) lights when tiled lighting is active
                    if ( isTiledShadingEnabled ) {
                        if ( desiredResolution != SHADOW_CUBE_SIZE ) {
                            light->UpdateShadows = false;
                            continue; // should never happen, as we currently only use one resolution, but just in case, don't try to put bigger shadowmaps into tiled slots
                        }
                        int slot = m_TiledDeferred->AllocateSlot();
                        if ( slot >= 0 ) {
                            pl->SetTiledSlot( slot, m_TiledDeferred->GetSlotTarget( slot ), m_TiledDeferred.get() );
                            pl->SetCurrentResolution( desiredResolution );
                        } else {
                            light->UpdateShadows = false;
                            continue; // failed to allocate tiled slot, skip shadow rendering for this light this frame
                        }
                    } else {
                        pl->AcquireShadowMap( dsPool, desiredResolution );
                    }

                    light->UpdateShadows = true; // Force an immediate render this frame
                }

                bool needsUpdate = pl->NeedsUpdate();
                bool isInited = pl->IsInited();

                // Sort into Important vs Background Queue
                if ( isInited ) {
                    // Immediate Priority: Light moved, was just created, or explicit flag set
                    if ( needsUpdate || light->UpdateShadows ) {
                        importantUpdates.emplace_back( light );
                    }
                    // Background Priority: Add to round-robin queue if not already there
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
            } else {
                // Out of range: Return VRAM to the pool!
                if ( pl->HasAnyShadowMap() ) {
                    pl->ClearTiledSlot();
                    pl->ReleaseShadowMap();

                    // Erase from the update queue if it happens to be pending
                    auto it = std::find( graphicsEngine->FrameShadowUpdateLights.begin(), graphicsEngine->FrameShadowUpdateLights.end(), light );
                    if ( it != graphicsEngine->FrameShadowUpdateLights.end() ) {
                        graphicsEngine->FrameShadowUpdateLights.erase( it );
                    }

                    auto importantIt = std::find( importantUpdates.begin(), importantUpdates.end(), light );
                    if ( importantIt != importantUpdates.end() ) {
                        importantUpdates.erase( importantIt );
                    }
                }
            }
        }
    }

    // Render the immediate priority lights
    for ( auto const& importantUpdate : importantUpdates ) {
        static_cast<D3D11PointLight*>(importantUpdate->LightShadowBuffers.get())->RenderCubemap( true, m_PointLightCB.get() );
        importantUpdate->UpdateShadows = false;
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

        light->UpdateShadows = false;

        // FORCE the render! It waited in line for its turn, it must draw.
        l->RenderCubemap( true, m_PointLightCB.get() );
        graphicsEngine->DebugPointlight = l;

        updatesDone++;
    }

    return XR_SUCCESS;
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

    if ( isOutdoor ) {
        // For atlas path: clear entire atlas once before rendering all cascades
        if ( m_useAtlas && m_shadowAtlas ) {
            auto dsv = m_shadowAtlas->GetDepthStencilView();
            if ( dsv ) {
                // Determine clear value based on sun/shadow state
                bool shouldRenderShadows =
                    Engine::GAPI->GetSky()->GetAtmoshpereSettings().LightDirection.y > 0 &&
                    settings.DrawShadowGeometry &&
                    settings.EnableShadows;
                float clearValue = shouldRenderShadows ? 1.0f :
                    (Engine::GAPI->GetSky()->GetAtmoshpereSettings().LightDirection.y <= 0 ? 0.0f : 1.0f);
                m_context->ClearDepthStencilView( dsv, D3D11_CLEAR_DEPTH, clearValue, 0 );
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

            // Atlas path: provide per-cascade viewport and skip per-cascade clear
            if ( m_useAtlas && m_shadowAtlas ) {
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
    RenderToTextureBuffer& depthCopy) {
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

    DrawWorldLights();

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

    XMStoreFloat3( scb.SQ_LightDirectionVS.toXMFLOAT3(),
        XMVector3TransformNormal( XMLoadFloat3( sky->GetAtmosphereCB().AC_LightPos.toXMFLOAT3() ), view ) );

    float3 sunColor = settings.SunLightColor;
    float sunStrength = Toolbox::lerp(
        settings.SunLightStrength,
        settings.RainSunLightStrength,
        std::min( 1.0f, rain * 2.0f ) );
    scb.SQ_LightColor = float4( sunColor.x, sunColor.y, sunColor.z, sunStrength );

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
            scb.SQ_LightColor = float4( 1, 1, 1, DEFAULT_INDOOR_VOB_AMBIENT.x );
        }

    return scb;
}

XRESULT D3D11ShadowMap::DrawWorldLights()
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
    psAtmo->GetBuffer("Atmosphere").Update(&sky->GetAtmosphereCB()).Bind();

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

    // Atlas path: fill per-cascade UV rects for shader atlas sampling
    if ( m_useAtlas && m_shadowAtlas ) {
        for ( size_t i = 0; i < MAX_CSM_CASCADES; ++i ) {
            scb.SQ_CascadeAtlasRect[i] = m_shadowAtlas->GetCascadeUVRect( static_cast<UINT>( i ) );
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

            // Only use world AO
            scb.SQ_WorldAOStrength = 1.0f;
            // Darken the lights
            scb.SQ_LightColor = float4( 1, 1, 1, DEFAULT_INDOOR_VOB_AMBIENT.x );
        }

    psAtmo->GetBuffer( "DS_ScreenQuadConstantBuffer" ).Update( &scb ).Bind();

    // CSM: Bind the cascade array to a single slot (Texture2DArray)
    BindToPixelShader( m_context.Get(), TX_ShadowmapArray );

    if ( graphicsEngine->Effects->GetRainShadowmap() )
        graphicsEngine->Effects->GetRainShadowmap()->BindToPixelShader( m_context.Get(), TX_RainShadowmap );

    this->BindSampler( m_context.Get(), 2 );

    m_context->PSSetShaderResources( TX_ReflectionCube, 1, graphicsEngine->ReflectionCube2.GetAddressOf() );

    graphicsEngine->GetDistortionTexture()->BindToPixelShader( TX_Distortion );
    graphicsEngine->GetBlueNoiseTexture()->BindToPixelShader( TX_BlueNoise512 );

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
    std::vector<std::pair<MeshKey, MeshInfo*>>* worldMeshCache,
    bool clearDepth,
    unsigned int casterMask ) {

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
            graphicsEngine->SetActiveVertexShader( VShaderID::VS_ExLayered );
        } else {
            // Set cubemap shader
            graphicsEngine->SetActiveGShader( GShaderID::GS_Cubemap );
            graphicsEngine->GetActiveGS().get()->Apply();
            face = targetCube.GetDepthStencilView().Get();

            graphicsEngine->SetActiveVertexShader( VShaderID::VS_ExCube );
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

    if ( clearDepth ) {
        m_context->ClearDepthStencilView( face.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0 );
    }

    // Draw the world mesh without textures
    if ( useLayeredPath ) {
        graphicsEngine->DrawWorldAround_Layered( position, range, cullFront, indoor, noNPCs, renderedVobs,
            renderedMobs, worldMeshCache, casterMask );
    } else {
        graphicsEngine->DrawWorldAround( position, range, cullFront, indoor, noNPCs, renderedVobs,
            renderedMobs, worldMeshCache, casterMask );
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
