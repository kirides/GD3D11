#pragma once
#include "BaseShadowedPointLight.h"
#include "WorldConverter.h"
#include <thread>
#include <condition_variable>
#include <atomic>
#include "TexturePool.h"
#include "ThreadPool.h"

class D3D11PointLight;
class D3D11TiledDeferredShading;

struct VobLightInfo;
struct RenderToDepthStencilBuffer;
struct RenderToTextureBuffer;
struct VobInfo;
struct SkeletalVobInfo;
class D3D11ConstantBuffer;
class D3D11PointLight : public BaseShadowedPointLight {
public:
    D3D11PointLight( VobLightInfo* info, bool dynamicLight = false );
    ~D3D11PointLight() override;

    /** Initializes the resources of this light */
    void InitResources();

    /** Draws the surrounding scene into the cubemap */
    void RenderCubemap( bool forceUpdate, D3D11ConstantBuffer* ViewMatricesCB );

    /** Binds the shadowmap to the pixelshader */
    void OnRenderLight();

    /** Returns if this light is inited already */
    bool IsInited();

    /** Returns if this light needs an update */
    bool NeedsUpdate();

    /** Returns true if the light could need an update, but it's not very important */
    bool WantsUpdate();

    /** Returns true if this is the first time that light is being rendered */
    bool NotYetDrawn();

    /** Called when a vob got removed from the world */
    void OnVobRemovedFromWorld( BaseVobInfo* vob ) override;

    bool HasAnyShadowMap() const {
        return HasShadowMap(0) || HasShadowMap(1) || m_StaticDepthCubemap != nullptr;
    }

    bool IsStaticShadowReady() const {
        return m_StaticShadowReady;
    }

    bool HasShadowMap(int shadowMapKind ) const { 
        if ( shadowMapKind == 0 ) return m_DepthCubemap != nullptr;
        return m_TiledDepthTarget != nullptr;
    }
    int GetShadowMapResolution() const { return m_CurrentResolution; }
    ID3D11Texture2D* GetShadowCubeTexture() const { return m_DepthCubemap ? m_DepthCubemap->GetTexture().Get() : nullptr; }

    void AcquireShadowMap( DepthStencilPool* pool, int resolution );
    void ReleaseShadowMap();

    // Tiled deferred slot management (renders directly into shared TextureCubeArray)
    void SetTiledSlot( int slot, RenderToDepthStencilBuffer* target, D3D11TiledDeferredShading* owner );
    void ClearTiledSlot();
    int GetTiledSlot() const { return m_TiledSlotIndex; }
    void SetCurrentResolution( int r ) { m_CurrentResolution = r; }

protected:
    int GetCurrentShadowMode() const;
    void HandleShadowModeChange( int shadowMode );
    RenderToDepthStencilBuffer* GetActiveShadowTarget() const;
    void AcquireStaticAsideShadowMap( DepthStencilPool* pool, int resolution );
    void ReleaseStaticAsideShadowMap();
    void CopyStaticAsideToActiveTarget() const;
    void RenderStaticShadowPass( RenderToDepthStencilBuffer& target, bool clearDepth );
    void RenderAnimatedShadowPass( RenderToDepthStencilBuffer& target, bool clearDepth );

    bool IsReady();
    void Invalidate();
    void StartReInit();

    /** Renders the scene with the given view-proj-matrices */
    void RenderCubemapFace( const XMFLOAT4X4& view, const XMFLOAT4X4& proj, UINT faceIdx );

    /** Renders all cubemap faces at once, using the geometry shader */
    void RenderFullCubemap();

    std::list<VobInfo*> VobCache;
    std::list<SkeletalVobInfo*> SkeletalVobCache;
    std::vector<std::pair<MeshKey, MeshInfo*>> WorldMeshCache;
    bool WorldCacheInvalid;

    VobLightInfo* LightInfo;
    DepthStencilHandle m_DepthCubemap;
    DepthStencilHandle m_StaticDepthCubemap;
    int m_CurrentResolution = 0; // Track current LOD size
    XMFLOAT4X4 CubeMapViewMatrices[6];
    XMFLOAT3 LastUpdatePosition;
    DWORD LastUpdateColor;
    bool DynamicLight;
    std::atomic<bool> InitDone;
    bool DrawnOnce;
    bool m_StaticShadowReady = false;
    int m_LastShadowMode = -1;

    // Tiled deferred slot (non-owning, owned by D3D11TiledDeferredShading)
    int m_TiledSlotIndex = -1;
    RenderToDepthStencilBuffer* m_TiledDepthTarget = nullptr;
    D3D11TiledDeferredShading* m_TiledOwner = nullptr;
    TaskHandle<void> m_PendingInit;
};
