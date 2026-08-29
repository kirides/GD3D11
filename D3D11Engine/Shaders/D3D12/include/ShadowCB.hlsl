// Per-frame CSM sun-shadow + scene-wetness + AO/SSR-reprojection + sky-IBL constants — ONE 512-byte
// buffer shared by World/Vob/Skeletal/Vegetation/Decal.hlsl, and the single source of truth for its
// HLSL layout (used to be five hand-synced copies, one per shader). Register differs per shader (each
// root sig has different b-slot occupancy) — #define SHADOWCB_REGISTER before including this file.
//
// The buffer is written by several disjoint CPU-side passes at fixed byte offsets, not as one struct:
//   [0,   256) D3D12ShadowMap::Prepare       — CSM cascades + sun dir/color/ambient (head, below)
//   [256, 352) UploadWetnessConstants        — WetnessCBData (D3D12GraphicsEngine.h)
//   [352, 432) UploadAoScreenConstants       — AoScreenCBData (kAoReprojCbOffset = 352)
//   [432, ...) UploadSkyIblConstants         — SkyIblCBData   (kSkyIblCbOffset  = 432)
// Vegetation.hlsl applies no wetness/SSR and reads no ShadowMap.hlsl helpers for those fields, but must
// still declare them so its copy of this same 512-byte resource keeps the sky-IBL tail at the right
// offset — three (four, with Decal) disjoint writers into one layout.
#ifndef D3D12_SHADOWCB_HLSL
#define D3D12_SHADOWCB_HLSL

#ifndef SHADOWCB_REGISTER
#define SHADOWCB_REGISTER b3
#endif

cbuffer ShadowCB : register(SHADOWCB_REGISTER)
{
    float4x4 CascadeViewProj[NUM_CSM_CASCADES];
    float3   SunDirWS;          float ShadowMapSize;    // dir TOWARD sun; shadow-map resolution
    float3   SunColor;          float SunIntensity;     // sun color (sRGB) + strength (0 when sun below horizon)
    float3   CascadeTexelWorld; float AmbientStrength;  // world units/texel; SQ_ShadowStrength (ambient/sky term)
    float    ShadowAOStrength;  float WorldAOStrength;   // vertLighting -> AO modulation weights
    // How hard baked vertex light gates the sky-IBL AMBIENT term (PBRLighting.hlsl ComputeSunLightingPBR).
    // 0 = the old unoccluded behaviour, 1 = interiors get no sky ambient at all. See the note there.
    float    SkyOccStrength;    float SunSpecularEnabled;
    // --- Scene wetness (rain) tail, uploaded separately by UploadWetnessConstants after the rain shadow
    // pass has computed this frame's rain camera. RainShadowIndex/DistortionIndex are 0xFFFFFFFF when the
    // rain shadowmap / distortion2.dds isn't available, which disables the effect entirely.
    float4x4 RainViewProj;
    float    SceneWetness;      float RainFxWeight;     float RainTime;   uint RainShadowIndex;
    uint     DistortionIndex;   float RainShadowMapSize; float2 _wetpad;
    // --- Screen-space AO / opaque-SSR-reprojection block, 80 bytes, written by UploadAoScreenConstants
    // (kAoReprojCbOffset). AoInvRes: 1/screen-size, which SampleScreenSpaceAO turns SV_Position into a mask
    // UV with. SsrPrevColorIndex/SsrPrevDepthIndex + SsrPrevViewProj: the previous-frame opaque scene
    // color/depth (D3D12Ssr.cpp's m_SsrPrevColor/m_SsrPrevDepth) and the view-proj to reproject into their
    // UV space — see PBRLighting.hlsl's opaque-SSR march and D3D12_SSR_WET_SURFACES_PLAN.md. Each index's
    // low 24 bits are the bindless SRV slot, top 8 bits the step count for that pass (MaxSteps/RefineSteps);
    // MaxSteps == 0 means SSR is off. This block must stay exactly 80 bytes — the sky-IBL tail below relies
    // on it landing at kSkyIblCbOffset = 432.
    float2   AoInvRes;          uint SsrPrevColorIndex; uint SsrPrevDepthIndex;
    float4x4 SsrPrevViewProj;
    // --- Sky IBL tail, uploaded by UploadSkyIblConstants (kSkyIblCbOffset = 432). The bindless indices of the
    // sky irradiance + prefiltered-specular cubes built by Shaders/D3D12/SkyIbl.hlsl. Both are 0xFFFFFFFF when
    // the IBL is unavailable or switched off, which makes EvaluateSkyIBL fall back to the flat ambient term.
    // NOTE: SkyIblIntensity is the COMPLETE ambient scale for the IBL path (user knob x radiance
    // normalization x an UNHALVED ShadowStrength), premultiplied by UploadSkyIblConstants. The IBL branch
    // must not also apply AmbientStrength — that one still belongs to the flat fallback branch only.
    uint     SkyIrradianceIndex; uint  SkySpecularIndex;  float SkySpecularMips; float SkyIblIntensity;
};

#endif // D3D12_SHADOWCB_HLSL
