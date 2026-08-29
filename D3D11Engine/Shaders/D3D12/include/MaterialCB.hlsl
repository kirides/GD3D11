// Per-material bindless indices (root consts): SM6.6 ResourceDescriptorHeap[...] slots for this
// material's normal/ORM/diffuse maps, shared by World/Vob/Skeletal.hlsl. MatNormalIndex == 0xFFFFFFFF
// -> no normal map (skip perturb); MatOrmIndex is always valid (1x1 default = AO 1 / rough 0.5 / metal 0)
// and its top 2 bits pack the FxMap's channel layout for SampleOrm() to decode (see PBRLighting.hlsl).
// MatDiffuseIndex is always valid too (1x1 black fallback while a material's texture isn't cached in).
//
// Register differs per shader — #define MATERIALCB_REGISTER before including this file. World.hlsl also
// appends MatNormalStrength (scales the normal-map perturb: 1.0 for a real normalmap, or the weak
// DEFAULT_NOISE_NORMALMAP_STRENGTH (0.10) when raining and MatNormalIndex is the rain-distortion texture
// standing in for a missing normalmap) — #define MATERIALCB_EXTRA_FIELDS before including for that.
#ifndef D3D12_MATERIALCB_HLSL
#define D3D12_MATERIALCB_HLSL

#ifndef MATERIALCB_REGISTER
#define MATERIALCB_REGISTER b6
#endif
#ifndef MATERIALCB_EXTRA_FIELDS
#define MATERIALCB_EXTRA_FIELDS
#endif

cbuffer MaterialCB : register(MATERIALCB_REGISTER) { uint MatNormalIndex; uint MatOrmIndex; uint MatDiffuseIndex; MATERIALCB_EXTRA_FIELDS };

#endif // D3D12_MATERIALCB_HLSL
