// Per-frame distance-fog + camera-position constants, shared verbatim across World/Vob/Skeletal/
// Vegetation/Decal.hlsl. Register differs per shader (each root sig has different b-slot occupancy) —
// #define FOGCB_REGISTER before including this file.
#ifndef D3D12_FOGCB_HLSL
#define D3D12_FOGCB_HLSL

#ifndef FOGCB_REGISTER
#define FOGCB_REGISTER b1
#endif

cbuffer FogCB : register(FOGCB_REGISTER) { float3 FogColor; float FogNear; float3 CamPosWS; float FogFar; };

#endif // D3D12_FOGCB_HLSL
