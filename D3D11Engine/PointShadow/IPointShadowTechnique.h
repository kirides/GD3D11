#pragma once
// One point-light shadowing TECHNIQUE: where a light's depth lives, how it is rendered, and how it is
// sampled. Exactly one is active at a time; D3D11ShadowMap picks it and switches it (see
// D3D11ShadowMap::SelectPointShadowTechnique, which invalidates every existing bake on a change).
//
// Two exist today - LegacyPerLightCube (own cubemap per light, tiled lighting off) and TiledCubeArray (the
// two-tier shared cube arrays, see POINTLIGHT_TWO_TIER_PLAN.md). What varies WITHIN a technique - the
// PLS_* update policy, the NVIDIA per-face rasterization fallback - is not a technique of its own: see
// PointShadowPolicy and PointShadowCasters.

#include <dxgiformat.h>
#include <memory>
#include <vector>
#include <cstdint>

#include "../ShaderIDs.h"

class zCVob;
class D3D11PointLight;
struct VobLightInfo;

enum class EPointShadowTechnique {
    LegacyPerLightCube,
    TiledCubeArray,
};

/** What a technique brings with it besides its resources. Caster-side shader selection is deliberately
    NOT here: PointShadowBatch picks it per pass. */
struct PointShadowTechniqueInfo {
    // The deferred point-light PS pair the legacy lit pass picks between. Unused by a technique whose
    // lighting runs through the tiled/forward+ shaders instead.
    PShaderID   SampleWithShadow = PShaderID::PS_DS_PointLightDynShadow;
    PShaderID   SampleNoShadow = PShaderID::PS_DS_PointLight;

    DXGI_FORMAT DepthFormat = DXGI_FORMAT_R16_TYPELESS;
    DXGI_FORMAT DSVFormat = DXGI_FORMAT_D16_UNORM;
    DXGI_FORMAT SRVFormat = DXGI_FORMAT_R16_UNORM;

    UINT        FacesPerLight = 6;

    /** The render target is a window into a shared array rather than a self-contained cube. Replaces the
        old D3D11PointLight::IsTiledArrayTarget - it is what the NVIDIA per-face fallback keys on. */
    bool        UsesSharedArrayTargets = false;
};

/** The per-light half of a technique: everything about one light that only that technique understands.
    D3D11PointLight owns one of these and nothing else technique-specific. */
class IPointShadowLightState {
public:
    virtual ~IPointShadowLightState() = default;

    virtual EPointShadowTechnique Technique() const = 0;

    /** Any depth this light can currently be sampled from, in any tier. */
    virtual bool HasAnyShadowMap() const = 0;

    /** True when a dropped bake is counted HERE. False for a technique whose slot table is the single
        counter and has already noted every drop this light can see (the tiled one). */
    virtual bool CountsOwnRebakes() const = 0;

    /** Hand back every GPU resource this light holds. Must leave nothing sampleable behind - it is what
        makes a technique switch's invalidation provable rather than a checklist. */
    virtual void ReleaseResources() = 0;

    /** A vob left the world; drop anything baked from it. Called for every light, not just nearby ones. */
    virtual void OnVobRemovedFromWorld( const zCVob* vob ) {}
};

class IPointShadowTechnique {
public:
    virtual ~IPointShadowTechnique() = default;

    virtual EPointShadowTechnique Id() const = 0;
    virtual const char* Name() const = 0;
    virtual const PointShadowTechniqueInfo& Info() const = 0;

    /** Allocate whatever this technique needs up front. Lights are created lazily either way. */
    virtual XRESULT OnActivate() = 0;
    /** Release EVERYTHING, including anything the lights still hold - see IPointShadowLightState. */
    virtual void OnDeactivate() = 0;

    /** This frame's shadow work: which lights render, into what, and how much. */
    virtual XRESULT DrawShadows( std::vector<VobLightInfo*>& lights ) = 0;

    /** Per-light sampling setup for the legacy deferred lit pass. No-op for a technique that binds its
        depth once for the whole frame instead. */
    virtual void BindPerLightSampling( D3D11PointLight& light ) {}

    /** Does this light currently have depth the lit pass can sample? Replaces HasShadowMap(int kind). */
    virtual bool ProvidesShadowFor( const D3D11PointLight& light ) const = 0;

    /** The light's shadow index as the shaders decode it (PointLightSlotSelector::EncodeIndex); 0 means
        unshadowed, which is what a technique without shared arrays always reports. */
    virtual int32_t EncodeShadowIndexFor( const VobLightInfo& light ) const { return 0; }

    /** A light vob is leaving the world - release anything keyed on it. */
    virtual void OnLightVobRemoved( const zCVob* lightVob ) {}

    virtual std::unique_ptr<IPointShadowLightState> CreateLightState( D3D11PointLight& light ) = 0;
};
