// Forward+ tiled/clustered per-frame params: light count + tiles/row + low-res/dynamic point-shadow cube heap
// slots + the cluster Z-slice basis (ProjA/ProjB/NearZ/FarZ feed PBRLighting.hlsl's ComputeZSlice, and
// LightCull.hlsl's own CullCB carries the same NearZ/FarZ for the culling pass). Shared by World/Vob/
// Skeletal/Vegetation/Decal.hlsl; NOT by LightCull.hlsl (see the note in ForwardPlusTypes.hlsl for why).
// Register differs per shader (each root sig has different b-slot occupancy) — #define LIGHTCB_REGISTER
// before including this file.
#ifndef D3D12_LIGHTCB_HLSL
#define D3D12_LIGHTCB_HLSL

#ifndef LIGHTCB_REGISTER
#define LIGHTCB_REGISTER b2
#endif

cbuffer LightCB : register(LIGHTCB_REGISTER) {
    uint LightCount; uint NumTilesX; uint LimitLightIntensity; uint PointShadowLowIndex; uint PointShadowDynIndex;
    float ProjA; float ProjB; float NearZ; float FarZ;
};

#endif // D3D12_LIGHTCB_HLSL
