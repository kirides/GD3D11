#pragma once
#include "BaseShadowedPointLight.h"
#include "WorldConverter.h"
#include <thread>
#include <condition_variable>
#include <atomic>
#include "TexturePool.h"
#include "PointLightSlotSelector.h"
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

    /** True once the target this light holds physically contains THIS light's depth - not the same question
        as whether the bake is up to date (IsStaticShadowReady), which a world change drops. Only a slot
        changing hands clears it. Gates advertising the cube at all: an unrendered slot holds the previous
        occupant's depth and shades the light black rather than merely unshadowed. */
    bool HasOwnDepthInSlot() const { return m_SlotHasOwnDepth; }

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

    /** Which tier this light would PREFER, independent of the one it sits in. Reads the preferred mode, not
        GetCurrentShadowMode(): a spilled light reports STATIC_ONLY, which would make the spill permanent. */
    bool WantsStaticOnlySlot() const {
        return GetPreferredShadowMode() == GothicRendererSettings::PLS_STATIC_ONLY;
    }

    /** Fold this frame's position into the "has not moved recently" tracker; called once per frame per
        visible light. Gothic's IsStatic() bit does not answer this: a colour-animated brazier reads as
        non-static there while never being repositioned. */
    void NoteStationary();

    /** True when this light's category is not opted into VOB/NPC casters (PointlightShadowCasterFlags), so
        its cube holds the world mesh alone and the full-res tier would buy it nothing. */
    bool RestrictsCastersToWorld() const;

    /** True once this light has held still long enough for its cube to be worth caching - the condition for
        spilling it into the low-res tier, which bakes once and never runs the dynamic overlay. */
    bool IsSpatiallyStatic() const;

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

    /** In the low-res tier while preferring the full-res one. Only used by the debug window. */
    bool IsSpilled() const { return m_TiledSlotLowRes && !WantsStaticOnlySlot(); }

    // Debug-visualization accessors (see ImGuiShim::RenderPointLightShadowDebugWindow).
    VobLightInfo* GetLightInfo() const { return LightInfo; }
    ID3D11Texture2D* GetTiledShadowCubeTexture() const { return m_TiledDepthTarget ? m_TiledDepthTarget->GetTexture().Get() : nullptr; }
    int GetTiledFaceBaseSlice() const { return m_TiledSlotIndex >= 0 ? m_TiledSlotIndex * 6 : -1; }
    float GetDebugZNear() const { return m_DebugLastZNear; }
    float GetDebugZFar() const { return m_DebugLastZFar; }

    // Tiled deferred slot management (renders directly into a shared TextureCubeArray). `lowRes` selects
    // which of the two independent slot pools this came from - see WantsStaticOnlySlot(). `sel` is the slot
    // table that handed the slot out; the light reports its finished bakes back to it, which is what lets the
    // backend-neutral world-change invalidation reach a D3D11 light. Never owned.
    void SetTiledSlot( int slot, RenderToDepthStencilBuffer* target, D3D11TiledDeferredShading* owner, bool lowRes,
        PointLightSlotSelector* sel );
    void ClearTiledSlot();
    /** Hands the slot this light is leaving OVER instead of dropping it: it still holds this light's depth
        and stays what the lit pass samples until the new slot is rendered. Without it a tier change shaded
        the light unshadowed until the new bake landed, which the range clamp shows as it easing off. */
    void SetSlotFallback( int slot, bool lowRes ) { m_FallbackSlotIndex = slot; m_FallbackSlotLowRes = lowRes; }
    int GetTiledSlot() const { return m_TiledSlotIndex; }
    bool IsTiledSlotLowRes() const { return m_TiledSlotLowRes; }
    /** Which slot the lit pass samples: this light's own once it holds its depth, else the one being handed
        over from. -1 when neither, i.e. genuinely unshadowed. */
    int GetSampleSlot() const { return m_SlotHasOwnDepth ? m_TiledSlotIndex : m_FallbackSlotIndex; }
    bool IsSampleSlotLowRes() const { return m_SlotHasOwnDepth ? m_TiledSlotLowRes : m_FallbackSlotLowRes; }
    bool HasSampleableDepth() const { return GetSampleSlot() >= 0; }
    /** This light's slot in the selector's single global index space (low-res slots sit above the full-res
        pool), or -1. D3D11's two tiers keep separate local indices because they are separate arrays. */
    int GetGlobalTiledSlot() const;
    void SetCurrentResolution( int r ) { m_CurrentResolution = r; }

protected:
    /** The mode this light's content is rendered and sampled with: GetPreferredShadowMode(), except that a
        low-res tier slot forces STATIC_ONLY - that array has no dynamic-overlay twin. */
    int GetCurrentShadowMode() const;
    /** The mode the light would run at on its own merits, independent of the tier it occupies. */
    int GetPreferredShadowMode() const;
    void HandleShadowModeChange( int shadowMode );
    RenderToDepthStencilBuffer* GetActiveShadowTarget() const;
    void AcquireStaticAsideShadowMap( DepthStencilPool* pool, int resolution );
    void ReleaseStaticAsideShadowMap();
    void CopyStaticAsideToActiveTarget() const;

    /** Single funnel for "this light's baked static shadow is no longer valid". Counts the drop only when
        there actually was a bake to lose, and only on the legacy path - see the note in the definition. */
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
    // See HasOwnDepthInSlot(). Set when a render completes, cleared when the target changes hands.
    bool m_SlotHasOwnDepth = false;
    /** Set once the animated pass has rendered into this light's slot of the dynamic-overlay array; cleared
        whenever the slot changes hands or the light's caches are invalidated, so we never advertise an
        overlay that still holds another light's (or a stale) depth. */
    bool m_HasDynamicOverlay = false;
    int m_LastShadowMode = -1;
    float m_DebugLastZNear = 0.0f;
    float m_DebugLastZFar = 0.0f;
    // Consecutive frames this light has been absent (disabled/out of VisibleInFrame) - see NoteAbsence().
    int m_MissingFrames = 0;

    // "Has not moved recently" tracking - see NoteStationary()/IsSpatiallyStatic().
    XMFLOAT3 m_StationaryPos = {};
    int m_StationaryFrames = 0;

    /** Reports a finished static bake back to the slot table: the cache stamp plus the caster identities it
        covered. No-op on the legacy per-light-cubemap path, which owns no slot. */
    void CommitStaticBakeToSlot();

    // Tiled deferred slot (non-owning, owned by D3D11TiledDeferredShading)
    int m_TiledSlotIndex = -1;
    // The shared slot table this slot came from - see SetTiledSlot. Non-owning; null on the legacy path.
    PointLightSlotSelector* m_SlotSel = nullptr;
    bool m_TiledSlotLowRes = false; // which of the two independent slot pools m_TiledSlotIndex indexes into
    // The slot a tier switch is handing over FROM - see SetSlotFallback. Stays sampleable until
    // m_TiledSlotIndex has been rendered; the selector holds the matching half.
    int m_FallbackSlotIndex = -1;
    bool m_FallbackSlotLowRes = false;
    RenderToDepthStencilBuffer* m_TiledDepthTarget = nullptr;
    D3D11TiledDeferredShading* m_TiledOwner = nullptr;
};
