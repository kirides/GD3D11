#pragma once
// The two-tier shared cube arrays - see POINTLIGHT_TWO_TIER_PLAN.md. A light holds a slot in the always-on
// STATIC core array, optionally a slot in the scarce DYNAMIC overlay array, and the lit pass mins the two.
// Every decision (who gets a slot, what re-renders this frame) belongs to PointLightSlotSelector, which is
// shared verbatim with the D3D12 backend; this technique is the D3D11 half that owns the targets and draws.

#include "../pch.h"
#include "../PointLightSlotSelector.h"
#include "IPointShadowTechnique.h"
#include <vector>

class D3D11PointLight;
class D3D11TiledDeferredShading;
struct RenderToDepthStencilBuffer;
struct VobLightInfo;

class TiledCubeLightState final : public IPointShadowLightState {
public:
    explicit TiledCubeLightState( D3D11PointLight& light ) : m_Light( light ) {}
    ~TiledCubeLightState() override;

    EPointShadowTechnique Technique() const override { return EPointShadowTechnique::TiledCubeArray; }
    bool HasAnyShadowMap() const override { return m_StaticTarget != nullptr; }
    /** The slot table is the single rebake counter here, and has already noted every drop this light
        can see. */
    bool CountsOwnRebakes() const override { return false; }
    void ReleaseResources() override;

    // Two independent tiers, two independent slot indices. `sel` is the slot table that handed the static
    // slot out; the light reports its finished bakes back to it, which is what lets the backend-neutral
    // world-change invalidation reach a D3D11 light. Never owned.
    void SetStaticSlot( int slot, RenderToDepthStencilBuffer* target, PointLightSlotSelector* sel );
    void ClearStaticSlot();
    void SetDynSlot( int slot, RenderToDepthStencilBuffer* target );
    void ClearDynSlot();
    int GetStaticSlot() const { return m_StaticSlot; }
    int GetDynSlot() const { return m_DynSlot; }
    const PointLightSlotSelector* GetSlotSelector() const { return m_SlotSel; }
    ID3D11Texture2D* GetStaticCubeTexture() const;

    /** The per-frame entry point: the selector has already decided what this light does, so unlike the
        legacy path this asks no questions of its own. */
    void Render( bool renderStatic, bool renderDynamic );

private:
    /** Reports a finished static bake back to the slot table: the cache stamp plus the caster identities
        it covered. */
    void CommitStaticBakeToSlot();

    D3D11PointLight& m_Light;
    // Non-owning: the targets belong to D3D11TiledDeferredShading, the slots to the selector.
    int m_StaticSlot = -1;
    int m_DynSlot = -1;
    RenderToDepthStencilBuffer* m_StaticTarget = nullptr;
    RenderToDepthStencilBuffer* m_DynTarget = nullptr;
    PointLightSlotSelector* m_SlotSel = nullptr;
};

class TiledCubeArrayTechnique final : public IPointShadowTechnique {
public:
    TiledCubeArrayTechnique( D3D11TiledDeferredShading& tiled, PointLightSlotSelector& slots );

    EPointShadowTechnique Id() const override { return EPointShadowTechnique::TiledCubeArray; }
    const char* Name() const override { return "Tiled shared cube arrays"; }
    const PointShadowTechniqueInfo& Info() const override { return m_Info; }

    XRESULT OnActivate() override;
    void OnDeactivate() override;

    XRESULT DrawShadows( std::vector<VobLightInfo*>& lights ) override;

    /** Nothing per-light: both arrays are bound once for the whole frame by the tiled/forward+ lit pass. */
    bool ProvidesShadowFor( const D3D11PointLight& light ) const override;
    int32_t EncodeShadowIndexFor( const VobLightInfo& light ) const override;

    void OnLightVobRemoved( const zCVob* lightVob ) override;

    std::unique_ptr<IPointShadowLightState> CreateLightState( D3D11PointLight& light ) override;

private:
    /** Makes every light's slot agree with the selector. Walks the whole light map, not this frame's
        visible set: a light whose slot was handed to somebody else must let go of its end of it, and it
        can be anywhere at all when that happens. */
    void ReconcileSlots();

    D3D11TiledDeferredShading& m_Tiled;
    PointLightSlotSelector& m_Slots;
    PointShadowTechniqueInfo m_Info{};
    // This frame's dome sweep. Kept across frames so its capacity is reused (32-bit address-space rule).
    std::vector<PointLightSlotSelector::Candidate> m_Candidates;
};
