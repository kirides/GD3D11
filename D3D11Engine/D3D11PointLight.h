#pragma once
#include "BaseShadowedPointLight.h"
#include "WorldConverter.h"
#include <thread>
#include <condition_variable>
#include <atomic>
#include <memory>
#include "TexturePool.h"
#include "PointShadow/IPointShadowTechnique.h"
#include "ThreadPool.h"

class D3D11PointLight;
class LegacyCubeLightState;
class TiledCubeLightState;
class PointLightSlotSelector;

struct VobLightInfo;
struct RenderToDepthStencilBuffer;
struct RenderToTextureBuffer;
struct VobInfo;
struct SkeletalVobInfo;

/** One shadow-casting point light, as everything OUTSIDE the shadow code sees it. Holds only what is true
    of a point light whatever technique is drawing it: its vob, its caster caches, its cube range and its
    bake bookkeeping. Where the depth lives and how it is rendered belongs to the active
    IPointShadowTechnique and to the IPointShadowLightState it hands out - see PointShadow/. */
class D3D11PointLight : public BaseShadowedPointLight {
public:
    D3D11PointLight( VobLightInfo* info, bool dynamicLight = false );
    ~D3D11PointLight() override;

    /** Initializes the resources of this light */
    void InitResources();

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

    bool IsShadowReady() const override { return m_StaticShadowReady; }
    bool IsStaticShadowReady() const { return m_StaticShadowReady; }

    /** Drops the cached bake so the next render re-renders from scratch, even for an already-ready
        PLS_STATIC_ONLY light (forceUpdate alone doesn't cover that case). */
    void ForceRebake() { Invalidate(); }

    VobLightInfo* GetLightInfo() const { return LightInfo; }
    bool IsDynamicLight() const { return DynamicLight; }

    /** The cube's far-plane basis, NOT the light's live range: DoAnimation re-animates that every frame.
        Quantized and grow-only by PointLightSlotSelector::BuildCandidates on the tiled path. */
    void SetShadowRange( float range ) { if ( range > 0.0f ) m_ShadowRange = range; }
    float GetShadowRange() const;

    int GetShadowMapResolution() const { return m_CurrentResolution; }
    void SetCurrentResolution( int r ) { m_CurrentResolution = r; }

    /** True when this light's category is not opted into VOB/NPC casters (PointlightShadowCasterFlags), so
        its cube holds the world mesh alone and it never receives an overlay. */
    bool RestrictsCastersToWorld() const;

    /** Is there a moving caster inside this light's cube at all? */
    bool HasAnimatedCastersInRange() const;

    // ---- Technique state --------------------------------------------------------------------------------
    /** Attaches (or re-attaches) this light to the active technique. Cheap and idempotent; re-attaching
        after a technique switch is what guarantees no state of the old technique survives. */
    void EnsureState();
    IPointShadowLightState* State() const { return m_State.get(); }
    void DropState();
    /** The state, or null when a different technique is active. */
    LegacyCubeLightState* AsLegacy() const;
    TiledCubeLightState* AsTiled() const;

    /** Any depth this light can currently be sampled from, in any tier of the active technique. */
    bool HasAnyShadowMap() const;

    // ---- Bake bookkeeping (technique-neutral) ------------------------------------------------------------
    bool IsReady() const;
    void MarkStaticBakeReady() { m_StaticShadowReady = true; }
    void MarkNotDrawn() { DrawnOnce = false; }

    /** Single funnel for "this light's baked static shadow is no longer valid". Counted only when the
        active technique's state says the count is this light's to make - the tiled slot table is its own
        counter and has already noted every drop the light can see. */
    void DropStaticBake( EPointLightRebakeCause cause );
    void Invalidate();

    /** Latches a global PLS_* mode transition: drops the cached bake so the new mode renders from scratch.
        Returns the mode that is now latched. */
    int HandleShadowModeChange( int shadowMode );
    int GetLastShadowMode() const { return m_LastShadowMode; }

    /** Stamps a finished render: position, colour and DrawnOnce. Pairs with the caster scope, which
        restores the graphics state it changed. */
    void NoteRendered( const XMFLOAT3& vobPos );
    bool HasMoved() const;
    const XMFLOAT3& GetLastUpdatePosition() const { return LastUpdatePosition; }
    /** Position changed - the cached caster lists describe the old place. */
    void ClearCasterCaches();

    void NoteDebugCubePlanes( float zNear, float zFar ) { m_DebugLastZNear = zNear; m_DebugLastZFar = zFar; }

    // The caster lists this light's last static bake covered. Shared by every technique.
    std::list<VobInfo*> VobCache;
    std::list<SkeletalVobInfo*> SkeletalVobCache;
    std::vector<MeshDrawRange> WorldMeshCache;

    // ---- Debug-visualization accessors (see ImGuiShim::RenderPointLightShadowDebugWindow) ----------------
    // Thin forwarders into the active technique's state; the "not applicable" answer is -1 / null.
    float GetDebugZNear() const { return m_DebugLastZNear; }
    float GetDebugZFar() const { return m_DebugLastZFar; }
    /** Why this light last dropped its own bake, or PLR_NUM_CAUSES if it never has. The slot table keeps
        its own answer (PointLightSlotSelector::StaticSlot::lastCause) for the causes it decides. */
    EPointLightRebakeCause GetLastRebakeCause() const { return m_LastRebakeCause; }
    ID3D11Texture2D* GetShadowCubeTexture() const;
    ID3D11Texture2D* GetTiledShadowCubeTexture() const;
    int GetTiledFaceBaseSlice() const;
    int GetStaticSlot() const;
    int GetDynSlot() const;
    int GetMissingFrames() const;
    const PointLightSlotSelector* GetSlotSelector() const;

protected:
    void StartReInit();

    VobLightInfo* LightInfo;
    std::unique_ptr<IPointShadowLightState> m_State;

    int m_CurrentResolution = 0; // Track current LOD size
    XMFLOAT3 LastUpdatePosition;
    DWORD LastUpdateColor;
    bool DynamicLight;
    std::atomic<bool> InitDone;
    bool DrawnOnce;
    bool m_StaticShadowReady = false;
    int m_LastShadowMode = -1;
    float m_DebugLastZNear = 0.0f;
    float m_DebugLastZFar = 0.0f;

    // Animation-free cube far-plane basis - see SetShadowRange().
    float m_ShadowRange = 0.0f;

    // Debug only, for the ImGui point-light overlay: why this light last dropped its own bake. The slot
    // table records its own causes separately (PointLightSlotSelector::Slot::lastCause).
    EPointLightRebakeCause m_LastRebakeCause = PLR_NUM_CAUSES;
};
