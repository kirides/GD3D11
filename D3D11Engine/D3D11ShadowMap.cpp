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
extern bool FeatureRTArrayIndexFromAnyShader;

const float NUM_FRAME_SHADOW_UPDATES = 2;  // Fraction of lights to update per frame
const int NUM_MIN_FRAME_SHADOW_UPDATES = 4;  // Minimum lights to update per frame
const int MAX_IMPORTANT_LIGHT_UPDATES = 1;

D3D11ShadowMap::D3D11ShadowMap() {}

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
    m_dummyCubeRT = std::make_unique<RenderToTextureBuffer>( m_device.Get(), POINTLIGHT_SHADOWMAP_SIZE, POINTLIGHT_SHADOWMAP_SIZE, DXGI_FORMAT_B8G8R8A8_UNORM, nullptr, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, 1, 6 );

    // Initialize the cascaded shadow map
    m_cascadedShadowMap = std::make_unique<D3D11CascadedShadowMapBuffer>();
    m_cascadedShadowMap->Init( m_device, s, MAX_CSM_CASCADES );

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

// Computes cascade splits using a practical interpolation between uniform and logarithmic splits.
// Returns vector with (numCascades + 1) entries: [nearPlane, split1, split2, ..., farPlane]
std::vector<float> D3D11ShadowMap::ComputeCascadeSplits( float nearPlane, float farPlane, size_t numCascades, float lambda ) {
    if ( numCascades == 0 ) return { nearPlane, farPlane };
    lambda = std::clamp( lambda, 0.0f, 1.0f );
    std::vector<float> splits = {};
    splits.reserve( numCascades + 1 );
    splits.push_back( nearPlane );
    for ( size_t i = 1; i <= numCascades; ++i ) {
        float si = static_cast<float>(i) / static_cast<float>(numCascades);
        float logSplit = nearPlane * std::pow( farPlane / nearPlane, si );
        float uniformSplit = nearPlane + (farPlane - nearPlane) * si;
        float d = lambda * logSplit + (1.0f - lambda) * uniformSplit;
        splits.push_back( d );
    }
    return splits;
}

XRESULT D3D11ShadowMap::DrawLighting( std::vector<VobLightInfo*>& lights ) {
    auto graphicsEngine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    auto _ = graphicsEngine->RecordGraphicsEvent( L"DrawLighting" );

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

    bool partialShadowUpdate = Engine::GAPI->GetRendererState().RendererSettings.PartialDynamicShadowUpdates;

    // Draw pointlight shadows
    if ( Engine::GAPI->GetRendererState().RendererSettings.EnablePointlightShadows > 0 ) {
        std::list<VobLightInfo*> importantUpdates;
        auto _ = graphicsEngine->RecordGraphicsEvent( L"Pointlight Shadows" );

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
    }

    // ********************************
    // Cascade Shadow Map Rendering (Simple Sequential Version)
    // ********************************

    zCCamera* camera = zCCamera::GetCamera();
    if ( !camera ) return XR_SUCCESS;

    const float nearPlane = std::max( 1.0f, camera->GetNearPlane() );
    const float baseFarPlane = camera->GetFarPlane();

    // WorldShadowRangeScale als Multiplikator für die Schattenreichweite
    const float shadowRangeScale = Engine::GAPI->GetRendererState().RendererSettings.WorldShadowRangeScale;
    const float farPlane = baseFarPlane * std::max( 0.1f, shadowRangeScale );
    int numCascades = Engine::GAPI->GetRendererState().RendererSettings.NumShadowCascades;
    if ( numCascades > MAX_CSM_CASCADES || numCascades < 1 ) {
        numCascades = std::clamp( numCascades, 1, MAX_CSM_CASCADES );
        Engine::GAPI->GetRendererState().RendererSettings.NumShadowCascades = numCascades;
    }

    // Compute cascade splits
    auto splits = ComputeCascadeSplits( nearPlane, farPlane, numCascades, 0.98f );
    splits[numCascades] = baseFarPlane; // Let the last cascade reach the full far plane

    // Get light direction
    XMVECTOR dir = XMLoadFloat3( Engine::GAPI->GetSky()->GetAtmosphereCB().AC_LightPos.toXMFLOAT3() );

    if ( Engine::GAPI->GetRendererState().RendererSettings.SmoothShadowCameraUpdate ) {
        XMVECTOR scale = XMVectorReplicate( Engine::GAPI->GetRendererState().RendererSettings.SmoothShadowFrequency );
        dir = XMVectorDivide( _mm_cvtepi32_ps( _mm_cvtps_epi32( XMVectorMultiply( dir, scale ) ) ), scale );
    }

    static XMVECTOR oldP = XMVectorZero();
    XMVECTOR WorldShadowCP;
    // Update position
    // Try to update only if the camera went 200 units away from the last position
    // This prevents "shaking" when the player is strafing or moving just a tiny bit
    float len;
    XMStoreFloat( &len, XMVector3Length( oldP - cameraPositionXm ) );
    if ( (len < 64 &&
        // And is on even space
        (cameraPosition.x - static_cast<int>( cameraPosition.x )) < 0.1f &&
        // but don't let it go too far
        (cameraPosition.y - static_cast<int>(cameraPosition.y)) < 0.1f) || len < 200.0f ) {
        WorldShadowCP = oldP;
    } else {
        oldP = cameraPositionXm;
        WorldShadowCP = oldP;
    }

    // Indoor check
    static zTBspMode lastBspMode = zBSP_MODE_OUTDOOR;

    // Array für alle Cascade-Matrizen
    std::array<CameraReplacement, MAX_CSM_CASCADES> cascadeCRs = {};

    bool isOutdoor = Engine::GAPI->GetLoadedWorldInfo()->BspTree->GetBspTreeMode() == zBSP_MODE_OUTDOOR;

    const FXMVECTOR p = WorldShadowCP + dir * 10000.0f;
    const FXMVECTOR lookAt = WorldShadowCP;
    static const XMVECTORF32 c_XM_Up = { { { 0, 1, 0, 0 } } };

    if ( !isOutdoor ) {
        if ( Engine::GAPI->GetRendererState().RendererSettings.EnableShadows && lastBspMode == zBSP_MODE_OUTDOOR ) {
            // Clear all cascade DSVs
            for ( size_t cascadeIdx = 0; cascadeIdx < MAX_CSM_CASCADES; ++cascadeIdx ) {
                if ( auto dsv = GetCascadeDSV( static_cast<UINT>( cascadeIdx ) ) ) {
                    m_context->ClearDepthStencilView( dsv, D3D11_CLEAR_DEPTH, 0.0f, 0 );
                }
            }
            lastBspMode = zBSP_MODE_INDOOR;
        }

        // Setze Default für Indoor
        for ( size_t i = 0; i < numCascades; ++i ) {
            XMStoreFloat4x4( &cascadeCRs[i].ViewReplacement, XMMatrixTranspose( XMMatrixLookAtLH( p, lookAt, c_XM_Up ) ) );
            XMStoreFloat4x4( &cascadeCRs[i].ProjectionReplacement, XMMatrixTranspose( XMMatrixOrthographicLH(
                farPlane, farPlane, 1.0f, 20000.f ) ) );
            XMStoreFloat3( &cascadeCRs[i].PositionReplacement, p );
            XMStoreFloat3( &cascadeCRs[i].LookAtReplacement, lookAt );
        }
    } else {
        lastBspMode = zBSP_MODE_OUTDOOR;

        // *** RENDER EACH CASCADE mit korrekter Matrix ***
        for ( size_t cascadeIdx = 0; cascadeIdx < numCascades; ++cascadeIdx ) {
            // Cascade-spezifische Größe basierend auf Split-Verhältnis
            float splitRatio = splits[cascadeIdx + 1] / splits[numCascades];
            float cascadeSize = farPlane * std::sqrt( splitRatio );
            cascadeSize = std::max( cascadeSize, 500.0f );

            // Berechne View-Matrix für diese Cascade
            XMMATRIX lightView = XMMatrixLookAtLH( p, lookAt, c_XM_Up );
            
            // *** TEXEL SNAPPING ***
           // Berechne die Größe eines Texels in World-Space
            float texelSize = cascadeSize / static_cast<float>(GetSizeX());
            
            // Transformiere die Shadow-Kamera-Position in Light-Space
            XMVECTOR lightSpaceOrigin = XMVector3Transform( WorldShadowCP, lightView );
            XMFLOAT3 lightSpaceOriginF;
            XMStoreFloat3( &lightSpaceOriginF, lightSpaceOrigin );
            
            // Snappe auf Texel-Grenzen
            lightSpaceOriginF.x = std::floor( lightSpaceOriginF.x / texelSize ) * texelSize;
            lightSpaceOriginF.y = std::floor( lightSpaceOriginF.y / texelSize ) * texelSize;
            
            // Berechne den Offset und wende ihn auf die View-Matrix an
            XMVECTOR snappedOrigin = XMLoadFloat3( &lightSpaceOriginF );
            XMVECTOR originalOrigin = XMVector3Transform( WorldShadowCP, lightView );
            XMVECTOR snapOffset = snappedOrigin - originalOrigin;
            
            // Erstelle Offset-Matrix
            XMFLOAT3 snapOffsetF;
            XMStoreFloat3( &snapOffsetF, snapOffset );
            XMMATRIX offsetMatrix = XMMatrixTranslation( snapOffsetF.x, snapOffsetF.y, 0.0f );
            
            // Kombiniere View mit Offset
            XMMATRIX snappedLightView = XMMatrixMultiply( lightView, offsetMatrix );

            const XMMATRIX crViewRepl = XMMatrixTranspose( snappedLightView );
            const XMMATRIX crProjRepl = XMMatrixTranspose( XMMatrixOrthographicLH(
                cascadeSize, cascadeSize, 1.0f, 20000.f ) );

            XMStoreFloat4x4( &cascadeCRs[cascadeIdx].ViewReplacement, crViewRepl );
            XMStoreFloat4x4( &cascadeCRs[cascadeIdx].ProjectionReplacement, crProjRepl );
            XMStoreFloat3( &cascadeCRs[cascadeIdx].PositionReplacement, p );
            XMStoreFloat3( &cascadeCRs[cascadeIdx].LookAtReplacement, lookAt );

            // Render diese Cascade using the new CascadedShadowMap
            Engine::GAPI->SetCameraReplacementPtr( &cascadeCRs[cascadeIdx] );

            // Build render params
            RenderShadowmapsParams renderParams = {};
            XMStoreFloat3( &renderParams.CameraPosition, WorldShadowCP );
            renderParams.Target = nullptr;
            renderParams.CullFront = true;
            renderParams.DontCull = false;
            renderParams.DSVOverwrite = GetCascadeDSV( static_cast<UINT>(cascadeIdx) );
            renderParams.DebugRTV = nullptr;
            renderParams.CascadeIndex = static_cast<int>(cascadeIdx);
            renderParams.CascadeSplits = splits;
            renderParams.CascadeCameraReplacements = &cascadeCRs;

            RenderShadowmaps( renderParams );

            Engine::GAPI->SetCameraReplacementPtr( nullptr );
        }
    }

    graphicsEngine->SetDefaultStates();

    // Restore gothics camera
    Engine::GAPI->SetCameraReplacementPtr( nullptr );

    // Draw rainmap, if raining
    if ( Engine::GAPI->GetSceneWetness() > 0.00001f ) {
        graphicsEngine->Effects->DrawRainShadowmap();
    }

    XMMATRIX view = Engine::GAPI->GetViewMatrixXM();
    Engine::GAPI->SetViewTransformXM( view );

    // ********************************
    // Draw direct lighting
    // ********************************
    graphicsEngine->SetActiveVertexShader( "VS_ExPointLight" );
    graphicsEngine->SetActivePixelShader( "PS_DS_PointLight" );

    auto psPointLight = graphicsEngine->GetShaderManager().GetPShader( "PS_DS_PointLight" );
    auto psPointLightDynShadow = graphicsEngine->GetShaderManager().GetPShader( "PS_DS_PointLightDynShadow" );

    Engine::GAPI->SetFarPlane(
        Engine::GAPI->GetRendererState().RendererSettings.SectionDrawRadius *
        WORLD_SECTION_SIZE );

    Engine::GAPI->GetRendererState().BlendState.SetAdditiveBlending();
    if ( Engine::GAPI->GetRendererState().RendererSettings.LimitLightIntesity ) {
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

    view = XMMatrixTranspose( view );

    DS_PointLightConstantBuffer plcb = {};

    XMStoreFloat4x4( &plcb.PL_InvProj, XMMatrixInverse( nullptr, XMLoadFloat4x4( &Engine::GAPI->GetProjectionMatrix() ) ) );
    XMStoreFloat4x4( &plcb.PL_InvView, XMMatrixInverse( nullptr, XMLoadFloat4x4( &Engine::GAPI->GetRendererState().TransformState.TransformView ) ) );

    auto resolution = graphicsEngine->GetResolution();
    plcb.PL_ViewportSize = float2( static_cast<float>(resolution.x), static_cast<float>(resolution.y) );

    graphicsEngine->GetGBuffer0().BindToPixelShader( m_context.Get(), 0 );
    graphicsEngine->GetGBuffer1().BindToPixelShader( m_context.Get(), 1 );
    graphicsEngine->GetGBuffer2().BindToPixelShader( m_context.Get(), 7 );
    graphicsEngine->GetDepthBufferCopy()->BindToPixelShader( m_context.Get(), 2 );

    // Draw all lights
    for ( auto const& light : lights ) {
        zCVobLight* vob = light->Vob;

        // Reset state from CollectVisibleVobs
        light->VisibleInRenderPass = false;

        if ( !vob->IsEnabled() ) continue;

        // Set right shader
        if ( Engine::GAPI->GetRendererState().RendererSettings.EnablePointlightShadows > 0 ) {
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
            Engine::GAPI->GetRendererState().RendererSettings.VisualFXDrawRadius ) {
            // float fadeStart =
            // Engine::GAPI->GetRendererState().RendererSettings.VisualFXDrawRadius -
            // plcb.PL_Range;
            float fadeEnd =
                Engine::GAPI->GetRendererState().RendererSettings.VisualFXDrawRadius;

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

        if ( Engine::GAPI->GetRendererState().RendererSettings.EnablePointlightShadows > 0 ) {
            // Bind shadowmap, if possible
            if ( light->LightShadowBuffers )
                static_cast<D3D11PointLight*>(light->LightShadowBuffers)->OnRenderLight();
        }

        // Draw the mesh
        graphicsEngine->InverseUnitSphereMesh->DrawMesh();

        Engine::GAPI->GetRendererState().RendererInfo.FrameDrawnLights++;
    }

    Engine::GAPI->GetRendererState().BlendState.BlendOp = GothicBlendStateInfo::BO_BLEND_OP_ADD;
    Engine::GAPI->GetRendererState().BlendState.SetDirty();

    Engine::GAPI->GetRendererState().DepthState.DepthBufferCompareFunc = GothicDepthBufferStateInfo::CF_COMPARISON_ALWAYS;
    Engine::GAPI->GetRendererState().DepthState.SetDirty();

    Engine::GAPI->GetRendererState().RasterizerState.CullMode = GothicRasterizerStateInfo::CM_CULL_NONE;
    Engine::GAPI->GetRendererState().RasterizerState.SetDirty();

    // Modify light when raining
    float rain = Engine::GAPI->GetRainFXWeight();
    float wetness = Engine::GAPI->GetSceneWetness();

    bool isSnow = oCGame::GetGame()
        && oCGame::GetGame()->_zCSession_world
        && oCGame::GetGame()->_zCSession_world->GetSkyControllerOutdoor()
        && oCGame::GetGame()->_zCSession_world->GetSkyControllerOutdoor()->GetWeatherType() == zTWEATHER_SNOW;

    // Switch global light shader when raining
    if ( wetness > 0.0f && !isSnow) {
        graphicsEngine->SetActivePixelShader( "PS_DS_AtmosphericScattering_Rain" );
    } else {
        graphicsEngine->SetActivePixelShader( "PS_DS_AtmosphericScattering" );
    }

    graphicsEngine->SetActiveVertexShader( "VS_PFX" );

    graphicsEngine->SetupVS_ExMeshDrawCall();

    GSky* sky = Engine::GAPI->GetSky();
    graphicsEngine->GetActivePS()->GetConstantBuffer()[1]->UpdateBuffer( &sky->GetAtmosphereCB() );
    graphicsEngine->GetActivePS()->GetConstantBuffer()[1]->BindToPixelShader( 1 );

    DS_ScreenQuadConstantBuffer scb = {};
    scb.SQ_InvProj = plcb.PL_InvProj;
    scb.SQ_InvView = plcb.PL_InvView;
    scb.SQ_View = Engine::GAPI->GetRendererState().TransformState.TransformView;

    XMStoreFloat3( scb.SQ_LightDirectionVS.toXMFLOAT3(),
        XMVector3TransformNormal( XMLoadFloat3( sky->GetAtmosphereCB().AC_LightPos.toXMFLOAT3() ), view ) );

    float3 sunColor =
        Engine::GAPI->GetRendererState().RendererSettings.SunLightColor;

    float sunStrength = Toolbox::lerp(
        Engine::GAPI->GetRendererState().RendererSettings.SunLightStrength,
        Engine::GAPI->GetRendererState().RendererSettings.RainSunLightStrength,
        std::min( 1.0f, rain * 2.0f ) );// Scale the darkening-factor faster here, so it
    // matches more with the increasing fog-density

    scb.SQ_LightColor = float4( sunColor.x, sunColor.y, sunColor.z, sunStrength );

    // CSM: Alle Cascade-Matrizen setzen

    for ( size_t cascadeIdx = 0; cascadeIdx < MAX_CSM_CASCADES; ++cascadeIdx ) {
        scb.SQ_ShadowView[cascadeIdx] = cascadeCRs[cascadeIdx].ViewReplacement;
        scb.SQ_ShadowProj[cascadeIdx] = cascadeCRs[cascadeIdx].ProjectionReplacement;
    }

    scb.SQ_ShadowmapSize = static_cast<float>( this->GetSizeX() );

    // Get rain matrix
    scb.SQ_RainView = graphicsEngine->Effects->GetRainShadowmapCameraRepl().ViewReplacement;
    scb.SQ_RainProj = graphicsEngine->Effects->GetRainShadowmapCameraRepl().ProjectionReplacement;

    scb.SQ_ShadowStrength = Engine::GAPI->GetRendererState().RendererSettings.ShadowStrength;
    scb.SQ_ShadowAOStrength = Engine::GAPI->GetRendererState().RendererSettings.ShadowAOStrength;
    scb.SQ_WorldAOStrength = Engine::GAPI->GetRendererState().RendererSettings.WorldAOStrength;
    scb.SQ_ShadowSoftness = Engine::GAPI->GetRendererState().RendererSettings.ShadowSoftness;

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
    vscb.PFXVS_InvProj = scb.SQ_InvProj;
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
    m_context->PSSetShaderResources( 3, ARRAYSIZE( nullSrv ), nullSrv );

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
        const auto oldRadius = Engine::GAPI->GetRendererState().RendererSettings.OutdoorSmallVobDrawRadius;
        const auto oldVobRadius = Engine::GAPI->GetRendererState().RendererSettings.OutdoorVobDrawRadius;

        // Build frustum from the light's view/projection matrices (cascade shadow map perspective)
        // The view/projection matrices are already set via CameraReplacement before this call
        // We use the light-space matrices to build the frustum for proper CSM culling
        Frustum f = {};

        // Calculate expansion values based on cascade size
        // Shadow casters behind the camera (in light direction) need to be included
        // Expand more for larger cascades to catch distant shadow casters
        float expandBack = 0.0f;
        float expandSides = 0.0f;
        if ( Engine::GAPI->GetRendererState().RendererSettings.IsShadowFrustumCullingEnabled() ) {
            const GothicRendererSettings::E_ShadowFrustumCulling cullingMode = Engine::GAPI->GetRendererState().RendererSettings.ShadowFrustumCullingMode;

            // Get the cascade's view and projection matrices
            // If CascadeCameraReplacements is provided, use it directly; otherwise fall back to current camera replacement
            XMMATRIX lightView, lightProj;

            if ( params.CascadeCameraReplacements
                && params.CascadeIndex >= 0
                && params.CascadeIndex < MAX_CSM_CASCADES
                && params.CascadeSplits.size() > 1 ) {
                // Use the provided cascade camera replacement (already transposed for GPU, so transpose back)

                const auto cullingCascadeIndex = cullingMode == GothicRendererSettings::E_ShadowFrustumCulling::SHD_FRUSTUM_CULLING_CONSERVATIVE 
                    ? params.CascadeSplits.size() > 2
                        ? params.CascadeSplits.size() - 2 // second last cascade if we have more than 1
                        : params.CascadeIndex // not more than 2 ? then use current as thats the last
                    : params.CascadeIndex; // Use current cascade for aggressive mode
                const CameraReplacement& cr = params.CascadeCameraReplacements->at( cullingCascadeIndex );
                lightView = XMMatrixTranspose( XMLoadFloat4x4( &cr.ViewReplacement ) );
                lightProj = XMMatrixTranspose( XMLoadFloat4x4( &cr.ProjectionReplacement ) );
            } else {
                // Fall back to current camera replacement (already transposed, so transpose back)
                lightView = XMMatrixTranspose( Engine::GAPI->GetViewMatrixXM() );
                lightProj = XMMatrixTranspose( XMLoadFloat4x4( &Engine::GAPI->GetProjectionMatrix() ) );
            }

            if ( params.CascadeIndex >= 0 && params.CascadeIndex < static_cast<int>( params.CascadeSplits.size() ) - 1 ) {
                const float cascadeSize = params.CascadeSplits[params.CascadeIndex + 1];

                // When SHD_FRUSTUM_CULLING_AGGRESSIVE is used, the lightView/proj matrices are from the current cascade
                // else when CONSERVATIVE, they are from the (second-)last cascade

                const int inverseCount = (params.CascadeSplits.size() - params.CascadeIndex);
                expandBack = cascadeSize * (0.5f * inverseCount);
                expandSides = cascadeSize * (0.1f * inverseCount);
            }
            f.BuildOrthographic( lightView, lightProj, expandBack, expandSides );
        }

        XMVECTOR cameraPosition = XMLoadFloat3( &params.CameraPosition );
        graphicsEngine->DrawWorldAroundForWorldShadow( cameraPosition, 2, params.CullFront, params.DontCull, f );

        Engine::GAPI->GetRendererState().RendererSettings.OutdoorSmallVobDrawRadius = oldRadius;
        Engine::GAPI->GetRendererState().RendererSettings.OutdoorVobDrawRadius = oldVobRadius;
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
        if ( FeatureRTArrayIndexFromAnyShader ) {
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
