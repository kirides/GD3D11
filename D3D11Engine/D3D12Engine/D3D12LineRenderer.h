#pragma once
#include "../BaseLineRenderer.h"

/** D3D12 debug line renderer — STUB (Phase 1 first-light).
    Satisfies BaseGraphicsEngine::GetLineRenderer (a pure virtual) so callers such as the editor
    and debug overlays get a valid, non-null object instead of crashing. Lines are cached-and-
    dropped for now; real rendering (dynamic VB from the upload ring + one PSO — plan §5
    D3D12LineRenderer) lands once the D3D12 draw path exists. */
class D3D12LineRenderer : public BaseLineRenderer {
public:
    XRESULT AddLine( const LineVertex& /*v1*/, const LineVertex& /*v2*/ ) override { return XR_SUCCESS; }
    XRESULT AddLineScreenSpace( const LineVertex& /*v1*/, const LineVertex& /*v2*/ ) override { return XR_SUCCESS; }

    XRESULT Flush() override { return XR_SUCCESS; }
    XRESULT FlushScreenSpace() override { return XR_SUCCESS; }

    XRESULT ClearCache() override { return XR_SUCCESS; }
};
