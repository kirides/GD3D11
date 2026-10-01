#pragma once
#include "../RHI/Rhi.h"
#include <cstdint>
#include <memory>
#include <vector>
#include <wrl/client.h>
#include <DirectXMath.h>

class D3D12GraphicsEngine;
struct MeshInfo;

/** Ray-traced water reflections (D3D12 inline RayQuery, Shaders/D3D12/WaterRT.hlsl).

    Every frame with water on screen it builds a TLAS over: the world mesh (one BLAS, built once per world, one
    geometry per material range of the wrapped world buffers), the static VOBs within reach (one cached BLAS per
    visual over the VOB arena), node attachments (one cached BLAS per mesh over the attachment arena) and the
    main view's NPCs (rebuilt per frame from the posed skinning streams). Cached BLASes are compacted once their
    size is read back and live in suballocated pools, so a small prop costs its bytes rather than a 64 KB
    placement. The trace writes a premultiplied color + distance pair the water pixel shader reads in place of
    its screen-space march.

    Main thread only; Trace runs inside DrawWaterSurfaces after the water depth prepass. */
class D3D12RtReflections {
public:
    explicit D3D12RtReflections( D3D12GraphicsEngine& engine );
    ~D3D12RtReflections();

    /** What DrawWaterSurfaces hands over; slots index the shader-visible heap. */
    struct Inputs {
        UINT SurfaceDepthSlot;   // depth after the water prepass
        UINT SceneDepthSlot;     // depth before it
        UINT SceneColorSlot;     // opaque scene color before water (linear)
        UINT DistortionSlot;
        DirectX::XMFLOAT4X4 View;
        DirectX::XMFLOAT4X4 Projection;
        DirectX::XMFLOAT3 CameraPosition;
        float Time;
        int Quality;             // GothicRendererSettings::E_WaterRayTracing, not OFF
    };
    /** Builds the frame's acceleration structures and traces. On true the two slots hold Water.hlsl's inputs,
        already in PIXEL_SHADER_RESOURCE. */
    bool Trace( const Inputs& in, UINT& outColorSlot, UINT& outDistanceSlot );

    /** Called on frames that do not trace; drops every cache after a while of not being used. */
    void Idle();

    void OnLoadWorld();
    /** Any thread; applied at the next Trace or Idle. */
    void OnMeshInfoDestroyed( const MeshInfo* mesh );

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};
