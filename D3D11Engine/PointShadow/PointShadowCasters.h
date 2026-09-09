#pragma once
// The caster machinery every point-shadow technique shares: the cube's view/projection basis, the
// self-exclusion rules, and the two rasterization strategies (one layered draw vs. the NVIDIA per-face
// fallback). Nothing here knows WHERE the depth ends up - that is the technique's business.

#include "../pch.h"
#include "../ConstantBufferStructs.h"
#include <list>
#include <vector>

struct VobLightInfo;
struct VobInfo;
struct SkeletalVobInfo;
struct MeshDrawRange;
struct RenderToDepthStencilBuffer;

namespace PointShadowCasters {

    /** Whether this light's category is allowed VOB/NPC casters at all (RendererSettings.
        PointlightShadowCasterFlags). A light that isn't holds the world mesh alone. */
    bool AllowsDynamicCasters( const VobLightInfo* info );
    bool RestrictsCastersToWorld( const VobLightInfo* info );

    /** Is there a moving caster inside this light's cube at all? The same sphere test the animated pass
        applies, run first so an empty overlay costs neither a clear nor a slot. */
    bool HasAnimatedCastersInRange( const VobLightInfo* info, float shadowRange );

    /** Draws through the whole-array DSV with absolute slice indices; false for a target that owns
        its own cube. */
    bool UsesAbsoluteSliceIndexing( const RenderToDepthStencilBuffer* target );

    /** The 6 face view matrices and the shared projection for one light, bound for the layered VS /
        cubemap GS for as long as it lives. The CB comes from the per-frame ring pool, so a scope may
        never outlive the frame that made it. */
    class CubeRenderScope {
    public:
        CubeRenderScope( VobLightInfo* info, float shadowRange );
        ~CubeRenderScope();
        CubeRenderScope( const CubeRenderScope& ) = delete;
        CubeRenderScope& operator=( const CubeRenderScope& ) = delete;

        const XMFLOAT4X4& View( int face ) const { return m_View[face]; }
        const XMFLOAT4X4& Proj() const { return m_Proj; }
        float ZNear() const { return m_ZNear; }
        float ZFar() const { return m_ZFar; }

        /** Uploads the face matrices for one pass; sliceBase is what the layered VS/GS adds to the
            face index. */
        void BindCubeCB( unsigned int sliceBase ) const;

    private:
        CubemapGSConstantBuffer m_GCB{};
        XMFLOAT4X4 m_View[6];
        XMFLOAT4X4 m_Proj;
        float m_ZNear = 0.0f;
        float m_ZFar = 0.0f;
        bool m_SavedDepthClip = false;
    };

    /** Where one caster pass draws, and what it is allowed to draw. */
    struct CasterPass {
        VobLightInfo* Light = nullptr;
        float Range = 0.0f;
        RenderToDepthStencilBuffer* Target = nullptr;
        unsigned int CasterMask = 0;
        bool ClearDepth = true;
        /** Target is a window into a shared cube array rather than its own cube - the only thing the
            NVIDIA per-face fallback keys on (IPointShadowTechnique::Info().UsesSharedArrayTargets). */
        bool TargetIsSharedArray = false;
        std::list<VobInfo*>* VobCache = nullptr;
        std::list<SkeletalVobInfo*>* MobCache = nullptr;
        std::vector<MeshDrawRange>* WorldMeshCache = nullptr;
    };

    /** Picks the rasterization strategy and applies the light's self-exclusion. The one place either
        decision is made. */
    void Render( const CubeRenderScope& scope, const CasterPass& pass );

    // The three caster sets techniques compose from. Each fills in CasterPass::CasterMask and calls Render.
    void RenderStatic( const CubeRenderScope& scope, CasterPass pass );
    void RenderAnimated( const CubeRenderScope& scope, CasterPass pass );
    /** PLS_FULL: static and animated in one uncached pass. */
    void RenderAll( const CubeRenderScope& scope, CasterPass pass );

} // namespace PointShadowCasters
