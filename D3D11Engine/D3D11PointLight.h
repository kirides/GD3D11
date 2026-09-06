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

    bool HasAnyShadowMap() const {
        return HasShadowMap(0) || HasShadowMap(1) || m_StaticDepthCubemap != nullptr;
    }

    bool IsStaticShadowReady() const {
        return m_StaticShadowReady;
    }

    /** Drops the cached bake so the next render re-renders from scratch, even for an already-ready
        PLS_STATIC_ONLY light (forceUpdate alone doesn't cover that case). */
    void ForceRebake() { Invalidate(); }

    bool IsShadowReady() const override {
        return m_StaticShadowReady;
    }

    /** 0 = the legacy per-light cubemap (tiled lighting off), 1 = a slot in the shared static cube array. */
    bool HasShadowMap( int shadowMapKind ) const {
        if ( shadowMapKind == 0 ) return m_DepthCubemap != nullptr;
        return m_StaticTarget != nullptr;
    }
    int GetShadowMapResolution() const { return m_CurrentResolution; }
    ID3D11Texture2D* GetShadowCubeTexture() const { return m_DepthCubemap ? m_DepthCubemap->GetTexture().Get() : nullptr; }

    /** True when this light's category is not opted into VOB/NPC casters (PointlightShadowCasterFlags), so
        its cube holds the world mesh alone and it never receives an overlay. */
    bool RestrictsCastersToWorld() const;

    /** The cube's far-plane basis, NOT the light's live range: DoAnimation re-animates that every frame.
        Quantized and grow-only by PointLightSlotSelector::BuildCandidates, which is the only writer. */
    void SetShadowRange( float range ) { if ( range > 0.0f ) m_ShadowRange = range; }
    float GetShadowRange() const;

    void AcquireShadowMap( DepthStencilPool* pool, int resolution );
    void ReleaseShadowMap();

    /** LEGACY path only. Counts consecutive absent frames and returns true once the streak exceeds
        retentionFrames, so a light that merely blinks keeps its cubemap and can finish a bake that sticks.
        The tiled path retains on the selector's dome instead, which cannot see visibility at all. */
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
    ID3D11Texture2D* GetTiledShadowCubeTexture() const { return m_StaticTarget ? m_StaticTarget->GetTexture().Get() : nullptr; }
    int GetTiledFaceBaseSlice() const { return m_StaticSlot >= 0 ? m_StaticSlot * 6 : -1; }
    float GetDebugZNear() const { return m_DebugLastZNear; }
    float GetDebugZFar() const { return m_DebugLastZFar; }
    /** Why this light last dropped its own bake, or PLR_NUM_CAUSES if it never has. The slot table keeps
        its own answer (PointLightSlotSelector::StaticSlot::lastCause) for the causes it decides. */
    EPointLightRebakeCause GetLastRebakeCause() const { return m_LastRebakeCause; }
    /** The slot table this light's cubes live in, or null on the legacy path. Debug only. */
    const PointLightSlotSelector* GetSlotSelector() const { return m_SlotSel; }

    // ---- Tiled slot management -----------------------------------------------------------------------------
    // Two independent tiers, two independent slot indices - see POINTLIGHT_TWO_TIER_PLAN.md. `sel` is the slot
    // table that handed the static slot out; the light reports its finished bakes back to it, which is what
    // lets the backend-neutral world-change invalidation reach a D3D11 light. Never owned.
    void SetStaticSlot( int slot, RenderToDepthStencilBuffer* target, PointLightSlotSelector* sel );
    void ClearStaticSlot();
    void SetDynSlot( int slot, RenderToDepthStencilBuffer* target );
    void ClearDynSlot();
    int GetStaticSlot() const { return m_StaticSlot; }
    int GetDynSlot() const { return m_DynSlot; }
    void SetCurrentResolution( int r ) { m_CurrentResolution = r; }

    /** The tiled path's per-frame entry point: the selector has already decided what this light does, so
        unlike RenderCubemap (the legacy path) this asks no questions of its own. */
    void RenderTiledShadow( bool renderStatic, bool renderDynamic );

    /** Is there a moving caster inside this light's cube at all? The same sphere test the animated pass
        applies, run first so an empty overlay costs neither a clear nor a slot. */
    bool HasAnimatedCastersInRange() const;

protected:
    /** The global point-light shadow mode this light renders and samples with. A static light drops to
        PLS_STATIC_ONLY when its category is not opted into VOB/NPC casters - it has nothing to overlay. */
    int GetCurrentShadowMode() const;
    void HandleShadowModeChange( int shadowMode );
    RenderToDepthStencilBuffer* GetActiveShadowTarget() const;
    void AcquireStaticAsideShadowMap( DepthStencilPool* pool, int resolution );
    void ReleaseStaticAsideShadowMap();
    void CopyStaticAsideToActiveTarget() const;

    /** Single funnel for "this light's baked static shadow is no longer valid". Counts the drop only when
        there actually was a bake to lose, and only on the legacy path - see the note in the definition. */
    void DropStaticBake();
    /** Drops the bake AND attributes it: these are the light's own reasons, which the slot table cannot
        see (it still reads the slot's depth as valid), so unlike the overload above they always count. */
    void DropStaticBake( EPointLightRebakeCause cause );
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

    /** LEGACY path: all six faces into the per-light cubemap, compositing the cached static depth out of the
        aside cube and drawing the movers on top. */
    void RenderFullCubemap();

    /** Cube face matrices + projection for this light's origin and cube range, bound for the layered VS /
        cubemap GS; End restores what Begin changed and stamps the render. Shared by both paths. */
    void BeginCubeRender( const XMFLOAT3& vobPos );
    void EndCubeRender( const XMFLOAT3& vobPos );

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
    int m_LastShadowMode = -1;
    bool m_SavedDepthClip = false;   // BeginCubeRender saves it, EndCubeRender puts it back
    float m_DebugLastZNear = 0.0f;
    float m_DebugLastZFar = 0.0f;
    // Consecutive frames this light has been absent (disabled/out of VisibleInFrame) - see NoteAbsence().
    int m_MissingFrames = 0;

    // Animation-free cube far-plane basis - see SetShadowRange().
    float m_ShadowRange = 0.0f;

    // Debug only, for the ImGui point-light overlay: why this light last dropped its own bake. The slot
    // table records its own causes separately (PointLightSlotSelector::Slot::lastCause).
    EPointLightRebakeCause m_LastRebakeCause = PLR_NUM_CAUSES;

    /** Reports a finished static bake back to the slot table: the cache stamp plus the caster identities it
        covered. No-op on the legacy per-light-cubemap path, which owns no slot. */
    void CommitStaticBakeToSlot();

    // Tiled slots (non-owning; the targets are owned by D3D11TiledDeferredShading, the slots by the selector)
    int m_StaticSlot = -1;
    int m_DynSlot = -1;
    RenderToDepthStencilBuffer* m_StaticTarget = nullptr;
    RenderToDepthStencilBuffer* m_DynTarget = nullptr;
    // The shared slot table the static slot came from - see SetStaticSlot. Non-owning; null on the legacy path.
    PointLightSlotSelector* m_SlotSel = nullptr;
};
