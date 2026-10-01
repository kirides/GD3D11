// Forward+ tiled/clustered per-frame params: light count + tiles/row + the overlay point-shadow cube heap
// slot + the cluster Z-slice basis (ProjA/ProjB/NearZ/FarZ feed PBRLighting.hlsl's ComputeZSlice, and
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
    uint LightCount; uint NumTilesX; uint LimitLightIntensity; uint PointShadowDynIndex;
    uint RtShadowMaskIndex;   // bindless SRV of the ray-traced shadow mask + 1; 0 = this pass doesn't read it
    float ProjA; float ProjB; float NearZ; float FarZ;
};

#endif // D3D12_LIGHTCB_HLSL
