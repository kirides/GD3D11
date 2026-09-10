#pragma once
// Every point-light caster pass of a frame, drawn together. Queueing a pass collects its casters into storage all passes
// share; Flush then draws caster type by caster type, each caster into every light (and face) that sees it, so shaders
// bind once per type and only the cube CB, depth target and viewport change between draws.

#include "../pch.h"
#include <functional>

class zCTexture;

namespace PointShadowCasters {
    class CubeRenderScope;
    struct CasterPass;
}

/** One mesh draw of a caster, resolved once per point-light pass. */
struct CasterMeshDraw {
    ID3D11Buffer* VertexBuffer = nullptr;
    ID3D11Buffer* IndexBuffer = nullptr;   // null: non-indexed
    UINT Count = 0;
    UINT Offset = 0;
    zCTexture* AlphaTexture = nullptr;     // null: opaque, drawn without a pixel shader
};

namespace PointShadowBatch {

    /** Drops everything queued. The queued draws point at meshes a later pass may release, so they never outlive
        one DrawPointlightShadows. */
    void Begin();

    /** Collects the pass's casters now; nothing draws until Flush. */
    void Queue( const PointShadowCasters::CubeRenderScope& scope, const PointShadowCasters::CasterPass& pass );

    /** Runs after every phase 0 draw and before the phase 1 ones, e.g. to composite a static aside cube. */
    void AtPhaseBoundary( std::move_only_function<void()> action );

    /** Draws everything queued, phase 0 first, then resets. */
    void Flush();

} // namespace PointShadowBatch
