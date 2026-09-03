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
//   * clustered static lights (the 10-30 "atmospheric" fill lights a Gothic room is lit with) share ONE
//     cube rendered from their cluster centroid, so the cube lookup origin/far-plane differ from the
//     light's own;
//   * the shadow tier bit selects which cube array the slot lives in (see kShadowTierLow below).
// D3D11's own TiledPointLight is a separate, unaffected declaration.
#ifndef GD3D12_GPULIGHT_SHARED_H
#define GD3D12_GPULIGHT_SHARED_H

struct GPULight {
    float3 PositionView;    // 0
    float  Range;           // 12  shading falloff radius (range-clamped for unshadowed statics)
    float4 Color;           // 16  (.w = static flag 0/1)
    float3 PositionWorld;   // 32
    int    ShadowCubeIndex; // 44  -1 = no shadow, else slot | tier bit (see kShadowTierLow)
    float3 ShadowOrigin;    // 48  cube centre — == PositionWorld unless this light is clustered
    float  ShadowRange;     // 60  cube far-plane basis (far = ShadowRange*2) — == Range unless clustered
};

// High bit of ShadowCubeIndex selecting the LOW-RESOLUTION static cube array (D3D12PointShadows::
// kStaticCubeSize) over the full-res dynamic one. Bit 30, so the value stays a positive int and the
// existing "ShadowCubeIndex >= 0 means shadowed" test in every shader keeps working untouched; the slot
// itself is the low 30 bits.
static const int kShadowTierLow = 0x40000000;
// Bit 29: this light's slot also has a valid DYNAMIC (skeletal overlay) cube, so the lit pass samples
// the dynamic array as well and mins the two results. Full-res slots only — the low tier has no dynamic
// twin. Absent = pure static shadow, and no second sample is taken.
static const int kShadowHasDynamic = 0x20000000;
static const int kShadowSlotMask = 0x1FFFFFFF;
// LOW-TIER slot layout inside those bits. The static tier is bigger than one Texture2DArray can hold
// (341 cubes), so it is split into pages — separate arrays whose SRVs sit CONTIGUOUSLY in the bindless heap.
// Bits 0..9 are the slot within its page, bits 10..12 the page: the shader reads
// ResourceDescriptorHeap[PointShadowLowIndex + page]. Full-res slots keep using the whole kShadowSlotMask.
static const int kShadowLowSlotMask = 0x3FF;
static const int kShadowLowPageShift = 10;
static const int kShadowLowPageMask = 0x7;

#endif // GD3D12_GPULIGHT_SHARED_H
