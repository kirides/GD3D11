cbuffer FrameCB    : register(b0) { float4x4 ViewProj; };
cbuffer InstanceCB : register(b1) { float4x4 M_World; float4 ModelColor; float Fatness; float3 _pad; };
cbuffer BonesCB    : register(b2) { float4x4 Bones[96]; };
cbuffer FogCB      : register(b3) { float3 FogColor; float FogNear; float3 CamPosWS; float FogFar; };
cbuffer LightCB    : register(b4) { uint LightCount; uint NumTilesX; uint LimitLightIntensity; uint _lpad; };   // Forward+ tiled: light count + tiles/row

#include "include/ForwardPlusTypes.hlsl"

// Forward+ tiled point lights (root-descriptor SRVs + per-tile grid) — see the world shader for the rationale.
StructuredBuffer<GPULight>  Lights        : register(t1);
StructuredBuffer<LightGrid> LightGridBuf  : register(t2);
StructuredBuffer<uint>      LightIndexBuf : register(t3);

Texture2D    tx  : register(t0);
SamplerState smp : register(s0);

// CSM sun-shadow sampling (P2.9c-4b). Skeletal already uses b3 (fog) + b4 (light count), so the shadow CB
// lands at b5 here (world/VOB use b3); t4/s2 are free. Same select+PCF math as the world/VOB block.
cbuffer ShadowCB : register(b5)
{
    float4x4 CascadeViewProj[NUM_CSM_CASCADES];
    float3   SunDirWS;          float ShadowMapSize;
    float3   SunColor;          float SunIntensity;
    float3   CascadeTexelWorld; float AmbientStrength;
    float    ShadowAOStrength;  float WorldAOStrength;  float2 _shpad;
    // Scene-wetness (rain) tail — see World.hlsl for the layout notes; must stay identical in all three
    // lit shaders and in the CPU-side WetnessCBData.
    float4x4 RainViewProj;
    float    SceneWetness;      float RainFxWeight;     float RainTime;   uint RainShadowIndex;
    uint     DistortionIndex;   float RainShadowMapSize; float2 _wetpad;
    // Screen-space AO reprojection tail — see World.hlsl for the layout notes; must stay identical in all
    // three lit shaders and in the CPU-side AoReprojCBData.
    float4x4 AoPrevViewProj;
    uint     AoPrevDepthIndex;  float AoPrevProjZX;      float AoPrevProjZY;  float AoReprojValid;
    // --- Sky IBL tail, uploaded by UploadSkyIblConstants (kSkyIblCbOffset = 432). The bindless indices of the
    // sky irradiance + prefiltered-specular cubes built by Shaders/D3D12/SkyIbl.hlsl. Both are 0xFFFFFFFF when
    // the IBL is unavailable or switched off, which makes EvaluateSkyIBL fall back to the flat ambient term.
    // Keep in sync across World/Vob/Skeletal/Vegetation.hlsl and the SkyIblCBData struct on the CPU side.
    // NOTE: SkyIblIntensity is the COMPLETE ambient scale for the IBL path (user knob x radiance
    // normalization x an UNHALVED ShadowStrength), premultiplied by UploadSkyIblConstants. The IBL branch
    // must not also apply AmbientStrength — that one still belongs to the flat fallback branch only.
    uint     SkyIrradianceIndex; uint  SkySpecularIndex;  float SkySpecularMips; float SkyIblIntensity;
};
Texture2DArray          ShadowMap : register(t4);
SamplerComparisonState  shadowCmp : register(s2);
// Per-material bindless indices (root consts b6): SM6.6 ResourceDescriptorHeap[...] indices for this material's
// normal + ORM maps. MatNormalIndex == 0xFFFFFFFF -> no normal map (skip perturb); MatOrmIndex is always valid
// (the 1x1 default ORM = AO 1 / rough 0.5 / metal 0 when the material has no _FX map); its top 2 bits pack the
// FxMap's channel layout for SampleOrm() to decode (see PBRLighting.hlsl).
cbuffer MaterialCB : register(b6) { uint MatNormalIndex; uint MatOrmIndex; };
TextureCubeArray        PointShadowCubes : register(t5);   // point-light shadow cubes (P2.10d), R16 linear depth
// Simple-SSAO mask (bindless, set once per frame — see D3D12GraphicsEngine::RenderSSAO/m_ActiveAOMaskSrvSlot).
// b8, not b7: b7 is GhostCB below, read only by the separate GhostSkeletal root sig/PSO (PSGhost), not PSMain.
cbuffer AOCB : register(b8) { uint AoMaskIndex; };
// Point-clamp for the AO mask — see World.hlsl's identical declaration for why Sample (not Load) is required.
SamplerState smpAoClamp : register(s1);
// SampleScreenSpaceAO — see World.hlsl; needs AOCB/smpAoClamp + the ShadowCB reprojection tail declared above.
#include "include/ScreenSpaceAO.hlsl"

// DelightDiffuse, SamplePointShadow, ComputeSunShadow, the Cook-Torrance PBR helpers, PerturbNormal/
// CotangentFrame, ComputeSunLightingPBR and AccumTiledPointLights are shared with World.hlsl/Vob.hlsl.
#include "include/PBRLighting.hlsl"
// Wet-ground / scene-wetness (rain) — see World.hlsl. NPCs/monsters get wet in D3D11 too (its deferred
// wetness pass covers every opaque G-buffer pixel), so the skinned pass applies it as well.
#include "include/Wetness.hlsl"

struct VS_IN
{
    float4 pos[4]         : POSITION;    // 4 per-bone-space positions (half4)
    float3 normal         : NORMAL;
    float3 bindPoseNormal : TEXCOORD0;   // unused (view-space normal is a later step)
    float2 uv             : TEXCOORD1;
    uint4  boneIndices    : BONEIDS;
    float4 weights        : WEIGHTS;
};
struct VS_OUT { float4 clip : SV_POSITION; float2 uv : TEXCOORD0; float4 col : TEXCOORD1; float fogDist : TEXCOORD2; float3 wpos : TEXCOORD3; float3 wnrm : TEXCOORD4; };

VS_OUT VSMain( VS_IN i )
{
    float3 skinnedPos    = float3( 0, 0, 0 );
    float3 skinnedNormal = float3( 0, 0, 0 );
    [unroll]
    for ( int b = 0; b < 4; ++b )
    {
        float4x4 bone = Bones[i.boneIndices[b]];
        float    w    = i.weights[b];
        skinnedPos    += w * mul( float4( i.pos[b].xyz, 1.0 ), bone ).xyz;
        skinnedNormal += w * mul( i.normal, (float3x3)bone );
    }
    float3 worldPos = mul( float4( skinnedPos + Fatness * skinnedNormal, 1.0 ), M_World ).xyz;

    VS_OUT o;
    o.clip = mul( float4( worldPos, 1.0 ), ViewProj );
    o.uv  = i.uv;
    o.col = ModelColor;
    o.wpos = worldPos;
    // skinnedNormal is in model space (bone-rotated); rotate into world by M_World (rigid + ~uniform scale).
    o.wnrm = mul( skinnedNormal, (float3x3)M_World );
    o.fogDist = length( worldPos - CamPosWS );
    return o;
}

float4 PSMain( VS_OUT i ) : SV_TARGET
{
    float4 t = tx.Sample( smp, i.uv );
    clip( t.a - 0.5 );
    float3 N = normalize( i.wnrm );
    if ( MatNormalIndex != 0xffffffff )
    {
        Texture2D nrmTex = ResourceDescriptorHeap[MatNormalIndex];
        N = PerturbNormal( N, i.wpos, nrmTex, i.uv, smp );
    }
    float3 orm = SampleOrm( MatOrmIndex, i.uv );   // AO/Roughness/Metallic, decoded per the material's FxMap layout
    float3 albedo = SrgbToLinear( t.rgb );
    albedo = DelightDiffuse( albedo );
    float vertLighting = i.col.g;               // ModelColor green (white=1 for NPCs → no baked AO reduction)
    float shadow = ComputeSunShadow( i.wpos, N );
    // Scene wetness (rain) — see World.hlsl's PSMain for why this runs after the cascade lookup.
    float3 V = normalize( CamPosWS - i.wpos );
    float wetSheen;
    float wetness = ApplySceneWetness( i.wpos, V, N, albedo, orm.g, wetSheen );
    float ssao = SampleScreenSpaceAO( i.wpos );
    float3 rgb = ComputeSunLightingPBR( i.wpos, N, albedo, vertLighting, shadow, orm.g, orm.b, orm.r, ssao );
    rgb *= lerp( 1.0, 0.8, wetness );
    rgb += AccumTiledPointLights( i.clip.xy, i.wpos, N, albedo, orm.g, orm.b );   // dynamic point lights on top (PBR)
    rgb += wetSheen * ( 1.0 + shadow ) * SrgbToLinear( SunColor ) * SunIntensity;
    float f = saturate( ( i.fogDist - FogNear ) / max( 1.0, FogFar - FogNear ) );
    return float4( lerp( rgb, SrgbToLinear( FogColor ), f ), 1.0 );
}

// --- Depth-prepass variant (P2.9b-4b: adds skinned NPC/monster meshes to the Forward+ opaque depth prepass) ---
// Same matrix-palette skinning as VSMain (so the depth matches the color pass bit-for-bit) but outputs only
// clip + uv; reads b0/b1/b2 + t0/s0, NOT fog/light CBs — so it needs no BindFrameLights (no light-loop hang).
struct VS_DEPTH_OUT { float4 clip : SV_POSITION; float2 uv : TEXCOORD0; };
VS_DEPTH_OUT VSDepth( VS_IN i )
{
    float3 skinnedPos    = float3( 0, 0, 0 );
    float3 skinnedNormal = float3( 0, 0, 0 );
    [unroll]
    for ( int b = 0; b < 4; ++b )
    {
        float4x4 bone = Bones[i.boneIndices[b]];
        float    w    = i.weights[b];
        skinnedPos    += w * mul( float4( i.pos[b].xyz, 1.0 ), bone ).xyz;
        skinnedNormal += w * mul( i.normal, (float3x3)bone );
    }
    float3 worldPos = mul( float4( skinnedPos + Fatness * skinnedNormal, 1.0 ), M_World ).xyz;
    VS_DEPTH_OUT o;
    o.clip = mul( float4( worldPos, 1.0 ), ViewProj );
    o.uv = i.uv;
    return o;
}
float4 PSDepthClip( VS_DEPTH_OUT i ) : SV_TARGET
{
    float4 t = tx.Sample( smp, i.uv );
    clip( t.a - 0.5 );          // same cutout as PSMain so alpha edges don't lay down depth
    return float4( 0, 0, 0, 1 );   // discarded: the PSO's color write mask is 0 (depth-only pass)
}
// Shadow caster (P2.9c-2): void PS so the depth-only shadow PSO binds NO render target without a validation
// warning; only alpha-clips the cutout so alpha edges don't cast solid shadows.
void PSShadowClip( VS_DEPTH_OUT i )
{
    clip( tx.Sample( smp, i.uv ).a - 0.5 );
}

// Ghost/transparency skeletal VOBs (D3D12PipelineState::CreateGhostSkeletal): invisible-potion/fade NPCs.
// Reuses VSDepth's matrix-palette skinning (identical pose to the color/prepass/shadow draws) — unlit diffuse
// sample, alpha multiplied by a per-vob fade factor, no alpha-clip (a fading ghost should smoothly disappear,
// not pop). Mirrors D3D11's PS_TransparencySkel / the non-skeletal PSGhost in Preview.hlsl (same conventions:
// no SRGB linearize, raw t.rgb — kept consistent with the already-shipped non-skeletal ghost path).
cbuffer GhostCB : register(b7) { float GhostAlpha; float3 _GhostPad; }

float4 PSGhost( VS_DEPTH_OUT i ) : SV_TARGET
{
    float4 t = tx.Sample( smp, i.uv );
    return float4( t.rgb, t.a * GhostAlpha );
}
