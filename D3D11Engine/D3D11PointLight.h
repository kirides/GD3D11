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
class D3D11PointLight : public BaseShadowedPointLight {
public:
    D3D11PointLight( VobLightInfo* info, bool dynamicLight = false );
    ~D3D11PointLight() override;

    /** Initializes the resources of this light */
    void InitResources();

    /** Draws the surrounding scene into the cubemap */
    void RenderCubemap( bool forceUpdate );

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

    /** Drops the cached bake so the next RenderCubemap re-renders from scratch, even for an
        already-ready PLS_STATIC_ONLY light (forceUpdate alone doesn't cover that case). */
    void ForceRebake() { Invalidate(); }

    bool HasAnyShadowMap() const {
        return HasShadowMap(0) || HasShadowMap(1) || m_StaticDepthCubemap != nullptr;
    }

    bool IsStaticShadowReady() const {
        return m_StaticShadowReady;
    }

    bool IsShadowReady() const override {
        return m_StaticShadowReady;
    }

    /** True when this light's tiled slot in the dynamic-overlay cube array holds a valid, already-rendered set
        of moving casters. Drives SHADOW_CUBE_HAS_DYNAMIC, i.e. whether the shader takes the second sample.
        Only PLS_UPDATE_DYNAMIC ever refreshes that overlay, so the mode is re-checked here rather than trusted
        to have been cleared: any other mode would otherwise keep sampling whatever the last update left in the
        slot, freezing that NPC into the shadow. */
    bool HasDynamicShadowOverlay() const {
        return m_HasDynamicOverlay
            && GetCurrentShadowMode() == GothicRendererSettings::PLS_UPDATE_DYNAMIC;
    }

    bool HasShadowMap(int shadowMapKind ) const {
        if ( shadowMapKind == 0 ) return m_DepthCubemap != nullptr;
        return m_TiledDepthTarget != nullptr;
    }
    int GetShadowMapResolution() const { return m_CurrentResolution; }
    ID3D11Texture2D* GetShadowCubeTexture() const { return m_DepthCubemap ? m_DepthCubemap->GetTexture().Get() : nullptr; }

    /** Whether this light belongs in the low-res static-only tiled tier rather than the full-res one. */
    bool WantsStaticOnlySlot() const {
        return GetCurrentShadowMode() == GothicRendererSettings::PLS_STATIC_ONLY;
    }

    void AcquireShadowMap( DepthStencilPool* pool, int resolution );
    void ReleaseShadowMap();

    /** Called once per frame from DrawPointlightShadows for every light that currently owns shadow
        resources. Tracks consecutive absent frames (light disabled, or dropped out of VisibleInFrame) and
        returns true only once that streak exceeds retentionFrames - the caller should release the slot/
        shadow map then, not on the very first absent frame. A light that merely blinks (zCVobLight
        IsEnabled toggling for a flicker effect, or a one-frame frustum/visibility edge case) never crosses
        the threshold and keeps its slot and baked depth. Without this, a PLS_STATIC_ONLY light that blinks
        even occasionally can never finish a bake that STICKS: it gets evicted the instant it goes briefly
        absent, so by the time it wins a slot again the next blink has already wiped the previous progress -
        it lights unshadowed (bleeding through walls) forever. Mirrors D3D12PointShadows' Slot::missingFrames
        retention in SelectShadowedLights. */
    bool NoteAbsence( bool visibleThisFrame, int retentionFrames ) {
        if ( visibleThisFrame ) {
            m_MissingFrames = 0;
            return false;
        }
        return ++m_MissingFrames > retentionFrames;
    }
    int GetMissingFrames() const { return m_MissingFrames; }

    // Debug-visualization accessors (see ImGuiShim::RenderPointLightShadowDebugWindow).
    VobLightInfo* GetLightInfo() const { return LightInfo; }
    ID3D11Texture2D* GetTiledShadowCubeTexture() const { return m_TiledDepthTarget ? m_TiledDepthTarget->GetTexture().Get() : nullptr; }
    int GetTiledFaceBaseSlice() const { return m_TiledSlotIndex >= 0 ? m_TiledSlotIndex * 6 : -1; }
    float GetDebugZNear() const { return m_DebugLastZNear; }
    float GetDebugZFar() const { return m_DebugLastZFar; }

    // Tiled deferred slot management (renders directly into a shared TextureCubeArray). `lowRes` selects
    // which of the two independent slot pools this came from - see WantsStaticOnlySlot().
    void SetTiledSlot( int slot, RenderToDepthStencilBuffer* target, D3D11TiledDeferredShading* owner, bool lowRes );
    void ClearTiledSlot();
    int GetTiledSlot() const { return m_TiledSlotIndex; }
    bool IsTiledSlotLowRes() const { return m_TiledSlotLowRes; }
    void SetCurrentResolution( int r ) { m_CurrentResolution = r; }

protected:
    int GetCurrentShadowMode() const;
    void HandleShadowModeChange( int shadowMode );
    RenderToDepthStencilBuffer* GetActiveShadowTarget() const;
    void AcquireStaticAsideShadowMap( DepthStencilPool* pool, int resolution );
    void ReleaseStaticAsideShadowMap();
    void CopyStaticAsideToActiveTarget() const;

    /** Single funnel for "this light's baked static shadow is no longer valid". Counts the drop into
        RendererInfo.PointLightStaticInvalidations, but only when there actually WAS a bake to lose - so
        PLS_FULL, which never latches one, doesn't drown the stat in one event per light per frame. */
    void DropStaticBake();
    void RenderStaticShadowPass( RenderToDepthStencilBuffer& target, bool clearDepth );
    void RenderAnimatedShadowPass( RenderToDepthStencilBuffer& target, bool clearDepth );

    /** True if target is a window into a shared tiled shadow cube array rather than its own
        self-contained cube - see RequiresNvidiaTiledShadowFaceFallback. */
    bool IsTiledArrayTarget( const RenderToDepthStencilBuffer& target ) const;

    /** NVIDIA driver bug workaround for IsTiledArrayTarget() targets: renders each of the 6 faces through
        its own single-slice DSV instead of one layered draw routed by SV_RenderTargetArrayIndex - see
        RequiresNvidiaTiledShadowFaceFallback. */
    void RenderShadowCubeFacePasses(
        RenderToDepthStencilBuffer& target, bool clearDepth, unsigned int casterMask,
        std::list<VobInfo*>* renderedVobs, std::list<SkeletalVobInfo*>* renderedMobs,
        std::vector<MeshDrawRange>* worldMeshCache,
        const std::move_only_function<bool(const zCVob*) const>* ignoreVob );

    bool IsReady();
    void Invalidate();
    void StartReInit();

    /** Renders all cubemap faces at once, using the geometry shader */
    void RenderFullCubemap();

    std::list<VobInfo*> VobCache;
    std::list<SkeletalVobInfo*> SkeletalVobCache;
    std::vector<MeshDrawRange> WorldMeshCache;

    VobLightInfo* LightInfo;
    DepthStencilHandle m_DepthCubemap;
    DepthStencilHandle m_StaticDepthCubemap;
    int m_CurrentResolution = 0; // Track current LOD size
    XMFLOAT4X4 CubeMapViewMatrices[6];
    // Set alongside CubeMapViewMatrices in RenderCubemap() - RenderShadowCubeFacePasses' per-face
    // CameraReplacement needs the same projection every face shares.
    XMFLOAT4X4 CubeMapProjMatrix;
    XMFLOAT3 LastUpdatePosition;
    DWORD LastUpdateColor;
    bool DynamicLight;
    std::atomic<bool> InitDone;
    bool DrawnOnce;
    bool m_StaticShadowReady = false;
    /** Set once the animated pass has rendered into this light's slot of the dynamic-overlay array; cleared
        whenever the slot changes hands or the light's caches are invalidated, so we never advertise an
        overlay that still holds another light's (or a stale) depth. */
    bool m_HasDynamicOverlay = false;
    int m_LastShadowMode = -1;
    float m_DebugLastZNear = 0.0f;
    float m_DebugLastZFar = 0.0f;
    // Consecutive frames this light has been absent (disabled/out of VisibleInFrame) - see NoteAbsence().
    int m_MissingFrames = 0;

    // Tiled deferred slot (non-owning, owned by D3D11TiledDeferredShading)
    int m_TiledSlotIndex = -1;
    bool m_TiledSlotLowRes = false; // which of the two independent slot pools m_TiledSlotIndex indexes into
    RenderToDepthStencilBuffer* m_TiledDepthTarget = nullptr;
    D3D11TiledDeferredShading* m_TiledOwner = nullptr;
};
