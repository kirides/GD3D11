#pragma once
// Tiled lighting off: every light owns an unbounded DepthStencilPool cubemap of its own, gated by distance
// and drained through a small per-frame budget. No shared arrays, so no slot table and no HI-LO shadow
// index - the lit pass binds one light's cube at a time (D3D11LegacyDeferredShading).

#include "../pch.h"
#include "../TexturePool.h"
#include "IPointShadowTechnique.h"
#include <list>
#include <vector>

class D3D11PointLight;
class DepthStencilPool;
struct VobLightInfo;

namespace PointShadowCasters { class CubeRenderScope; }

class LegacyCubeLightState final : public IPointShadowLightState {
public:
    LegacyCubeLightState( D3D11PointLight& light, const PointShadowTechniqueInfo& info )
        : m_Light( light ), m_Info( info ) {}
    ~LegacyCubeLightState() override;

    EPointShadowTechnique Technique() const override { return EPointShadowTechnique::LegacyPerLightCube; }
    bool HasAnyShadowMap() const override { return m_DepthCubemap != nullptr || m_StaticDepthCubemap != nullptr; }
    /** The legacy path has no slot table to count for it. */
    bool CountsOwnRebakes() const override { return true; }
    void ReleaseResources() override;

    void AcquireShadowMap( DepthStencilPool* pool, int resolution );
    bool HasCube() const { return m_DepthCubemap != nullptr; }
    ID3D11Texture2D* GetCubeTexture() const;

    /** Binds this light's cubemap to the pixelshader. */
    void BindForSampling();

    /** Counts consecutive absent frames and returns true once the streak exceeds retentionFrames, so a
        light that merely blinks keeps its cubemap and can finish a bake that sticks. The tiled path
        retains on the selector's dome instead, which cannot see visibility at all. */
    bool NoteAbsence( bool visibleThisFrame, int retentionFrames ) {
        if ( visibleThisFrame ) {
            m_MissingFrames = 0;
            return false;
        }
        return ++m_MissingFrames > retentionFrames;
    }
    int GetMissingFrames() const { return m_MissingFrames; }

    /** Draws the surrounding scene into this light's own cubemap. */
    void Render( bool forceUpdate );

private:
    /** All six faces at once, compositing the cached static depth out of the aside cube and drawing the
        movers on top. */
    void RenderFullCubemap( const PointShadowCasters::CubeRenderScope& scope );
    void AcquireStaticAsideShadowMap( DepthStencilPool* pool, int resolution );
    void ReleaseStaticAsideShadowMap();
    void CopyStaticAsideToCube() const;

    D3D11PointLight& m_Light;
    const PointShadowTechniqueInfo& m_Info;
    DepthStencilHandle m_DepthCubemap;
    /** Holds the cached static bake while the active cube carries static+movers composited. */
    DepthStencilHandle m_StaticDepthCubemap;
    // Consecutive frames this light has been absent (disabled/out of VisibleInFrame) - see NoteAbsence().
    int m_MissingFrames = 0;
};

class LegacyCubeTechnique final : public IPointShadowTechnique {
public:
    EPointShadowTechnique Id() const override { return EPointShadowTechnique::LegacyPerLightCube; }
    const char* Name() const override { return "Legacy per-light cube"; }
    const PointShadowTechniqueInfo& Info() const override { return m_Info; }

    XRESULT OnActivate() override { return XR_SUCCESS; }
    void OnDeactivate() override { m_UpdateQueue.clear(); }

    XRESULT DrawShadows( std::vector<VobLightInfo*>& lights ) override;

    void BindPerLightSampling( D3D11PointLight& light ) override;
    bool ProvidesShadowFor( const D3D11PointLight& light ) const override;

    void OnLightVobRemoved( const zCVob* lightVob ) override;

    std::unique_ptr<IPointShadowLightState> CreateLightState( D3D11PointLight& light ) override;

private:
    PointShadowTechniqueInfo m_Info{};   // self-contained cubes: no shared-array fallback, defaults are right
    /** The round-robin background queue: lights whose re-render did not fit this frame's budget. */
    std::list<VobLightInfo*> m_UpdateQueue;
};
