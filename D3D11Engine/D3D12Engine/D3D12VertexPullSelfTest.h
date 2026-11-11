#pragma once
#include "../RHI/Rhi.h"
#include <d3d12.h>
#include <wrl/client.h>

class D3D12GraphicsEngine;

/** One-shot startup check that SV_VertexID/SV_InstanceID exclude the base vertex/start instance on this backend
    (VERTEX_PULLING_PLAN.md section 4). Run() records two tiny draws; the result is read back a few frames later. */
class D3D12VertexPullSelfTest {
public:
    explicit D3D12VertexPullSelfTest( D3D12GraphicsEngine& engine ) : E( engine ) {}

    /** Main thread, open frame, outside any render pass. Restores the viewport/scissor/RT cache it touches. */
    void Run();

private:
    void Poll();

    D3D12GraphicsEngine& E;
    Microsoft::WRL::ComPtr<Rhi::Resource> m_Readback;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT m_Footprint = {};
    enum class State { Idle, Pending, Done } m_State = State::Idle;
    UINT m_FramesWaited = 0;
};
