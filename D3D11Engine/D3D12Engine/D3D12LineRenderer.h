#pragma once
#include "../BaseLineRenderer.h"
#include <vector>

/** D3D12 debug line renderer — the port of D3D11LineRenderer.

    Same two-list model as D3D11: AddLine() caches world-space lines (transformed by the camera's
    reversed-Z ViewProj, depth-tested against the finished scene) and AddLineScreenSpace() caches
    pre-transformed xyzrhw lines (2D overlay, no depth). Both lists are drained once per frame by
    D3D12GraphicsEngine::DrawLines — see there for the ring upload + PSO selection — and cleared.

    Unlike D3D11 (which reallocates a growing dynamic vertex buffer on demand) the vertices go into the
    engine's per-frame, persistently-mapped line ring: no per-frame allocation, drop-and-log on overflow. */
class D3D12LineRenderer : public BaseLineRenderer {
public:
    /** Adds a world-space line to the list. */
    XRESULT AddLine( const LineVertex& v1, const LineVertex& v2 ) override;
    /** Adds a pre-transformed (xyzrhw) screen-space line to the list. */
    XRESULT AddLineScreenSpace( const LineVertex& v1, const LineVertex& v2 ) override;

    /** Draws + clears the cached lines. */
    XRESULT Flush() override;
    XRESULT FlushScreenSpace() override;

    /** Clears the line caches without drawing. */
    XRESULT ClearCache() override;

private:
    // Hard cap per list, in VERTICES (two per line). Nothing drains these lists unless a frame actually
    // renders the world, so an unbounded push (a stuck editor overlay, a world that never renders) would
    // otherwise grow forever. 1M vertices = 32 MB of cache — far past anything the debug overlays emit.
    static constexpr size_t kMaxCachedVertices = 1u << 20;

    std::vector<LineVertex> LineCache;
    std::vector<LineVertex> ScreenSpaceLineCache;
    bool CacheOverflowLogged = false;
};
