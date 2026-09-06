// Per-frame GPU point light — the ONE definition, compiled both as HLSL (ForwardPlusTypes.hlsl,
// LightCull.hlsl) and as C++ (D3D12EngineCommon.h). Filled by BuildFrameLightBuffer (D3D12Scene.cpp);
// the point-shadow slot selection (D3D12PointShadows::SelectShadowedLights) reads
// Range/PositionWorld/Color.w and writes ShadowCubeIndex/ShadowOrigin/ShadowRange.
//
// float3/float4 need no translation: Types.h already gives the C++ side HLSL-named, layout-compatible
// wrappers around DirectX::XMFLOAT3/XMFLOAT4 (no extra data members, no vtable), so this single struct
// body is valid on both sides verbatim — no cpp<->hlsl type-mapping shim needed (contrast XeGTAO.h,
// which DOES need one because its fields include `uint`).
//
// This USED to be byte-identical to D3D11's 48-byte TiledPointLight; it deliberately is not any more.
// The D3D12 backend needs a light to be able to sample a shadow cube that is NOT centred on itself:
// clustered static lights (the 10-30 "atmospheric" fill lights a Gothic room is lit with) share ONE cube
// rendered from their cluster centroid, so the cube lookup origin/far-plane differ from the light's own.
// D3D11's own TiledPointLight is a separate, unaffected declaration.
#ifndef GD3D12_GPULIGHT_SHARED_H
#define GD3D12_GPULIGHT_SHARED_H

struct GPULight {
    float3 PositionView;    // 0
    float  Range;           // 12  shading falloff radius (range-clamped for unshadowed statics)
    float4 Color;           // 16  (.w = static flag 0/1)
    float3 PositionWorld;   // 32
    int    ShadowCubeIndex; // 44  0 = unshadowed, else the HI-LO slot pair below
    float3 ShadowOrigin;    // 48  cube centre — == PositionWorld unless this light is clustered
    float  ShadowRange;     // 60  cube far-plane basis (far = ShadowRange*2) — == Range unless clustered
};

// ShadowCubeIndex encoding, HI-LO with 0 meaning invalid in each half:
//   LO 16 bits = STATIC (core) cube slot + 1     HI 16 bits = DYNAMIC (overlay) cube slot + 1
// So 0 overall is "unshadowed", and each half is decoded by subtracting 1. Mirrors
// PointLightSlotSelector::EncodeIndex, which both backends encode with.
static const int kShadowSlotShift = 16;
static const int kShadowSlotMask = 0xFFFF;

#endif // GD3D12_GPULIGHT_SHARED_H
