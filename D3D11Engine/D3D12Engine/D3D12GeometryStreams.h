#pragma once
#include "../RHI/Rhi.h"
#include <d3d12.h>
#include <cstdint>

class D3D12GraphicsEngine;

/** Bindless heap indices for whole-buffer RAW SRVs of every vertex/index resource, so vertex shaders can pull
    vertices (VERTEX_PULLING_PLAN.md). One SRV block per frame slot, rewritten every frame by Update(). */
class D3D12GeometryStreams {
public:
    enum class Stream : UINT {
        WorldVB, WorldIB, VobVB, VobIB, AttachVB, AttachIB, SkelVB, SkelIB, SkinnedPosUv, SkinnedNrmPrev,
        Count
    };

    explicit D3D12GeometryStreams( D3D12GraphicsEngine& engine );

    /** Main thread, open frame, after the frame's last arena flush/growth and before any pass reads Slot(). */
    void Update();
    /** Heap index of a stream for the current frame slot; UINT_MAX before Update() or if allocation failed. */
    UINT Slot( Stream s ) const;

private:
    D3D12GraphicsEngine& E;
    UINT m_Base[3] = {};   // slot + 1 per frame slot, 0 = not allocated
    bool m_Failed = false;
};
