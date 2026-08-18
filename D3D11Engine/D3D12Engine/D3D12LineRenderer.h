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
    /** Draws + clears the cached lines. */
    XRESULT Flush() override;
    XRESULT FlushScreenSpace() override;
};
