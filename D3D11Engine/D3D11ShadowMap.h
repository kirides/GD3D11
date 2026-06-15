#pragma once

#include "pch.h"
#include <wrl/client.h>
#include <memory>
#include <vector>
#include <array>
#include <DirectXMath.h>
#include "RenderToTextureBuffer.h"
#include "D3D11CascadedShadowMapBuffer.h"
#include "D3D11ShadowAtlas.h"
#include "D3D11_Helpers.h"
#include "WorldObjects.h"
#include "D3D11PointLight.h"
#include "Engine.h"
#include "GothicAPI.h"
#include "GSky.h"
#include "Frustum.h"
#include "D3D11RenderQueue.h"
#include "D3D11TiledDeferredShading.h"
#include "D3D11LegacyDeferredShading.h"

struct RenderToDepthStencilBuffer;
struct RenderToTextureBuffer;
struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11SamplerState;

class zCVob;
class zCVobLight;
class GSky;

enum PS_DS_AtmosphericScatteringSlots {
    TX_ShadowmapArray = 3,
    TX_RainShadowmap = 4,
    TX_ReflectionCube = 5,
    TX_Distortion = 6,
    TX_SI_SP = 7,
    TX_BlueNoise512 = 8,
};

const int POINTLIGHT_SHADOWMAP_SIZE = 128;

/** Parameters for rendering shadow maps */
struct RenderShadowmapsParams {
    // Camera position for shadow rendering
    DirectX::XMFLOAT3 CameraPosition = {};
    
    // Optional target depth stencil buffer (nullptr = use world shadowmap)
    RenderToDepthStencilBuffer* Target = nullptr;
    
    // Culling options
    bool CullFront = true;
    bool DontCull = false;
    
    // Optional DSV override
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> DSVOverwrite = nullptr;
    
    // Optional debug RTV for visualization
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> DebugRTV = nullptr;
    
    // Cascade index (-1 = not a cascade render)
    int CascadeIndex = -1;
    
    // Cascade split distances (size = numCascades + 1)
    std::vector<float> CascadeSplits = {};
    
    // Optional array of camera replacements for all cascades
    // Used to build frustums for culling without requiring CameraReplacement to be set externally
    const std::array<CameraReplacement, MAX_CSM_CASCADES>* CascadeCameraReplacements = nullptr;

    // Atlas viewport override for atlas rendering path
    D3D11_VIEWPORT ViewportOverride = {};
    bool UseViewportOverride = false;
    bool SkipClear = false;
};

class D3D11ShadowMap {
public:
    D3D11ShadowMap();
    ~D3D11ShadowMap();

    // Initialize resources. `size` is the initial square shadowmap size.
    // This will create a set of cascades (default cascades count used internally).
    void Init( Microsoft::WRL::ComPtr<ID3D11Device1>& device, Microsoft::WRL::ComPtr<ID3D11DeviceContext1>& context, int size );

    // Resize world shadowmap to a new size
    void Resize( int size );

    RenderToTextureBuffer* GetDummyCubeRT() { return m_dummyCubeRT.get(); }

    // Get the cascaded shadow map (texture array path)
    D3D11CascadedShadowMapBuffer* GetCascadedShadowMap() { return m_cascadedShadowMap.get(); }

    // Get the shadow atlas (atlas path, FL10)
    D3D11ShadowAtlas* GetShadowAtlas() { return m_shadowAtlas.get(); }

    // Whether we're using the atlas path (FL10) vs texture array (FL11+)
    bool IsUsingAtlas() const { return m_useAtlas; }

    // Get DSV for a specific cascade
    ID3D11DepthStencilView* GetCascadeDSV( UINT cascadeIndex ) {
        if ( m_useAtlas && m_shadowAtlas )
            return m_shadowAtlas->GetDepthStencilView(); // Single DSV for entire atlas
        return m_cascadedShadowMap ? m_cascadedShadowMap->GetCascadeDSV( cascadeIndex ) : nullptr;
    }

    // Get cascade 0 pixel size (largest cascade)
    int GetSizeX() const {
        if ( m_useAtlas && m_shadowAtlas ) return m_shadowAtlas->GetCascade0Size();
        if ( m_cascadedShadowMap ) return m_cascadedShadowMap->GetSize();
        return 0;
    }

    // Get per-cascade pixel size (differs in atlas mode)
    UINT GetCascadePixelSize( UINT cascadeIndex ) const {
        if ( m_useAtlas && m_shadowAtlas ) return m_shadowAtlas->GetCascadeSize( cascadeIndex );
        if ( m_cascadedShadowMap ) return m_cascadedShadowMap->GetSize();
        return 0;
    }

    // Bind world shadowmap SRV to a pixel shader slot (binds entire cascade array)
    void BindToPixelShader( ID3D11DeviceContext1* context, UINT slot );

    // Bind the shadowmap sampler to the given slot
    void BindSampler( ID3D11DeviceContext1* context, UINT slot );
    void BindSamplerToCS( ID3D11DeviceContext1* context, UINT slot );

    XRESULT PrepareRender();

    // Compute cascade split distances.
    // Returns a vector of size (numCascades + 1) where:
    //  splits[0] == nearPlane, splits[numCascades] == farPlane
    //  For cascade i: near = splits[i], far = splits[i+1]
    //  lambda in [0,1] interpolates between logarithmic (1.0) and uniform (0.0) splits.
    static std::vector<float> ComputeCascadeSplits( float nearPlane, float farPlane, size_t numCascades, float lambda = 0.95f, float bias = 1.0f );
    XRESULT DrawPointlightShadows(std::vector<VobLightInfo*>& lights);
    XRESULT DrawWorldShadow();
    XRESULT DrawRainShadowmap();
    XRESULT DrawPointlightLights(std::vector<VobLightInfo*>& lights, RenderToTextureBuffer& color, RenderToTextureBuffer& normals, RenderToTextureBuffer
                                 & specular, RenderToTextureBuffer& depthCopy);

    /** Renders the shadowmaps for the sun using parameter struct */
    void RenderShadowmaps( const RenderShadowmapsParams& params );

    XRESULT DrawWorldLights();
    DS_ScreenQuadConstantBuffer FillSunCSMConstantBuffer() const;
    XRESULT DrawLighting(std::vector<VobLightInfo*>& lights, RenderToTextureBuffer& color, RenderToTextureBuffer& normals, RenderToTextureBuffer
                         & specular, RenderToTextureBuffer& depthCopy);

    D3D11TiledDeferredShading* GetTiledDeferred() const { return m_TiledDeferred.get(); }

    void XM_CALLCONV RenderShadowCube( DirectX::FXMVECTOR position,
        float range,
        const RenderToDepthStencilBuffer& targetCube,
        Microsoft::WRL::ComPtr<ID3D11DepthStencilView> face,
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> debugRTV,
        bool cullFront = true,
        bool indoor = false,
        bool noNPCs = false,
        std::list<VobInfo*>* renderedVobs = nullptr, 
        std::list<SkeletalVobInfo*>* renderedMobs = nullptr,
        std::vector<std::pair<MeshKey, MeshInfo*>>* worldMeshCache = nullptr,
        bool clearDepth = true,
        unsigned int casterMask = 0xFFFFFFFFu );

    inline static struct { float lambda; float bias; } lambdaBiasTable[] {
        /* 0 */ { 0, 0 },
        /* 1 */ { 1.0f, 1.0f },
        /* 2 */ { 0.90f, 1.0f }, // 2 cascades really is bare minimum for quality
        /* 3 */ { 0.85f, 1.0f }, // with 3 cascades, we can show higher quality shadows further
        /* 4 */ { 0.80f, 1.0f }, // Players should really want to use 4 cascades for best quality and furthest
    };

    D3D11RenderQueue* GetRenderQueue( int cascadeIndex ) { return m_RenderQueues[cascadeIndex].get(); }

private:
    bool ShouldUseAtlas() const;
    void RecreateShadowSampler();
    void EnsureShadowMapBackend( int size );

    void WaitShadowCullingComplete();

    Microsoft::WRL::ComPtr<ID3D11Device1> m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext1> m_context;

    // CSM using texture array (FL11+)
    std::unique_ptr<D3D11CascadedShadowMapBuffer> m_cascadedShadowMap;

    // CSM using texture atlas (FL10 fallback)
    std::unique_ptr<D3D11ShadowAtlas> m_shadowAtlas;
    bool m_useAtlas = false;

    std::unique_ptr<RenderToTextureBuffer> m_dummyCubeRT;
    std::unique_ptr<D3D11ConstantBuffer> m_PointLightCB;

    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_shadowmapSampler;
    int m_lastNumCascades = 0;
    std::array<CameraReplacement, MAX_CSM_CASCADES> m_CascadeCRs;
    std::array<std::unique_ptr<D3D11RenderQueue>, MAX_CSM_CASCADES> m_RenderQueues;
    std::vector<float> m_CascadeSplits;
    std::array<bool, MAX_CSM_CASCADES> m_ShouldUpdateCascade;
    XMFLOAT3 m_WorldShadowPos;

    std::unique_ptr<D3D11TiledDeferredShading> m_TiledDeferred;
    D3D11LegacyDeferredShading m_LegacyDeferred;

    TracyLockable(std::mutex, m_CullingJobsMutex);
    std::vector<std::future<void>> m_ShadowCullingJobs;
};
