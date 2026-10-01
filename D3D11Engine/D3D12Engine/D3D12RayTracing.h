#pragma once
#include "../RHI/Rhi.h"
#include <cstdint>
#include <memory>
#include <vector>
#include <wrl/client.h>
#include <DirectXMath.h>

class D3D12GraphicsEngine;
class zCVob;
struct MeshInfo;

/** D3D12 inline ray tracing (RayQuery): one scene per frame, traced by the water reflections
    (Shaders/D3D12/WaterRT.hlsl) and the sun / point-light shadow mask (Shaders/D3D12/RtShadows.hlsl).

    The TLAS covers the world mesh (one BLAS per world, one geometry per material range of the wrapped world
    buffers), the static VOBs within reach (one cached BLAS per visual over the VOB arena), node attachments (one
    cached BLAS per mesh over the attachment arena) and the main view's NPCs (rebuilt per frame from the posed
    skinning streams). Cached BLASes are compacted once their size is read back and live in suballocated pools,
    so a small prop costs its bytes rather than a 64 KB placement.

    Main thread only. The scene is built at most once per frame by whichever pass asks first. */
class D3D12RayTracing {
public:
    explicit D3D12RayTracing( D3D12GraphicsEngine& engine );
    ~D3D12RayTracing();

    // TLAS instance masks; a point light traces with GPULight::RtShadowMask
    static constexpr uint32_t kMaskWorld = 0x01, kMaskVob = 0x02, kMaskDynamic = 0x04, kMaskCarrier = 0x08;
    static constexpr uint32_t kMaskAll = 0xFF;

    void BeginFrame();
    /** Ticks the idle release on frames that built no scene. */
    void EndFrame();
    /** Builds this frame's TLAS on the first call; later calls return that result. `vobRadius` limits the static
        VOBs, so the first caller must already know every consumer's reach. */
    bool EnsureScene( float vobRadius, bool indoorVobs );
    /** False once the world BLAS could not be built for the loaded world. */
    bool CanTrace() const;

    /** Skeletal vobs carrying a light that excludes its carrier; their instances get kMaskCarrier. */
    std::vector<const zCVob*>& CarrierVobs();

    static float WaterVobRadius( int quality );

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
        bool ScreenSpace;        // reuse the on-screen scene color for hits the camera sees
    };
    /** Traces the water reflections against this frame's scene (EnsureScene first). On true the two slots hold
        Water.hlsl's inputs, already in PIXEL_SHADER_RESOURCE. */
    bool TraceWater( const Inputs& in, UINT& outColorSlot, UINT& outDistanceSlot );

    /** The shadow mask pass; the depth buffer must be readable from compute. */
    struct ShadowInputs {
        UINT DepthSlot;
        DirectX::XMFLOAT4X4 View;
        DirectX::XMFLOAT4X4 Projection;
        DirectX::XMFLOAT3 CameraPosition;
        int SunRays;             // 0 = sun off
        float SunDistance;
        int PointRays;           // 0 = point lights off
        bool SunFiltered, PointFiltered;   // one ray, blurred by the blocker distance
        bool ContactShadows;     // also march the depth buffer toward the lights
        UINT NoiseFrame;
        float NearZ, FarZ;       // cluster Z range, as BindFrameLights
        UINT NumTilesX;
        D3D12_GPU_VIRTUAL_ADDRESS Lights, LightGrid;
    };
    /** Writes the R32G32_UINT shadow mask (RtShadowMask.hlsl) and leaves it in PIXEL_SHADER_RESOURCE. */
    bool TraceShadows( const ShadowInputs& in, UINT& outMaskSlot );

    // Shadow slots per pixel (RtShadowMask.hlsl's kRtMaxPointLights)
    static constexpr uint32_t kMaxPointShadowSlots = 15;
    /** Point-shadow slot use of a recent frame, read back a few frames late. */
    struct ShadowStats {
        uint32_t OverflowPixels = 0;   // pixels whose cluster wanted more slots than there are
        uint32_t MostSlots = 0;        // most slots any pixel wanted
        uint32_t ShadowedPixels = 0;   // pixels with at least one ray-traced light
        bool Valid = false;
    };
    const ShadowStats& LastShadowStats() const;

    void OnLoadWorld();
    /** Any thread; applied at the next scene build or idle tick. */
    void OnMeshInfoDestroyed( const MeshInfo* mesh );

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};
