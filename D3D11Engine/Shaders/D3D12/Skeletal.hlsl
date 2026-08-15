cbuffer FrameCB    : register(b0) { float4x4 ViewProj; };
// The motion-vector fields are APPENDED, never inserted: PointShadow.hlsl declares a byte-compatible prefix of
// this same CB (it shares the upload) and reading a prefix of a larger constant buffer is legal, whereas
// shifting ModelColor/Fatness would silently corrupt the point-shadow skeletal caster.
cbuffer InstanceCB : register(b1)
{
    float4x4 M_World;
    float4   ModelColor;
    float    Fatness;
    float3   _pad;
    float4x4 M_PrevWorld;      // previous frame's world matrix (SkeletalVobInfo::PrevWorldMatrix)
    uint     PrevBoneOffset;   // index of the FIRST previous-frame bone in Bones[] below; 0 == "no history"
    float3   _pad2;
};
// One palette holding BOTH poses back to back: current at [0, numBones), previous at [PrevBoneOffset, +numBones).
// Packing them into a single allocation is what keeps this change off the root signature AND off the 80-byte
// SkeletalDrawCommand indirect signature — a separate previous-bone CB would have needed a third root CBV in
// every skeletal command. 192 is 2 * NUM_MAX_BONES; only the allocated prefix is ever indexed.
cbuffer BonesCB    : register(b2) { float4x4 Bones[192]; };
cbuffer FogCB      : register(b3) { float3 FogColor; float FogNear; float3 CamPosWS; float FogFar; };
// Forward+ tiled: light count + tiles/row + low-res cube heap slot. ProjA/ProjB/NearZ/FarZ feed
// PBRLighting.hlsl's ComputeZSlice (clustered Forward+, P2.14) — see World.hlsl.
cbuffer LightCB    : register(b4) {
    uint LightCount; uint NumTilesX; uint LimitLightIntensity; uint PointShadowLowIndex; uint PointShadowDynIndex;
    float ProjA; float ProjB; float NearZ; float FarZ;
};

#include "include/ForwardPlusTypes.hlsl"

// Forward+ tiled point lights (root-descriptor SRVs + per-cluster mask) — see the world shader for the rationale.
StructuredBuffer<GPULight>  Lights        : register(t1);
StructuredBuffer<LightGrid> LightGridBuf  : register(t2);

// No t0 diffuse texture: EVERY entry point in this file (lit, depth-prepass, shadow-clip and ghost) fetches its
// diffuse bindlessly from MaterialCB.MatDiffuseIndex, so neither Skeletal.RootSig nor GhostSkeletal.RootSig
// carries an SRV descriptor table — see the note on MaterialCB below.
SamplerState smp : register(s0);

// CSM sun-shadow sampling (P2.9c-4b). Skeletal already uses b3 (fog) + b4 (light count), so the shadow CB
// lands at b5 here (world/VOB use b3); t4/s2 are free. Same select+PCF math as the world/VOB block.
cbuffer ShadowCB : register(b5)
{
    float4x4 CascadeViewProj[NUM_CSM_CASCADES];
    float3   SunDirWS;          float ShadowMapSize;
    float3   SunColor;          float SunIntensity;
    float3   CascadeTexelWorld; float AmbientStrength;
    float    ShadowAOStrength;  float WorldAOStrength;   // vertLighting -> AO modulation weights
    // How hard baked vertex light gates the sky-IBL AMBIENT term (PBRLighting.hlsl ComputeSunLightingPBR).
    // 0 = the old unoccluded behaviour, 1 = interiors get no sky ambient at all. See the note there.
    float    SkyOccStrength;    float SunSpecularEnabled;
    // Scene-wetness (rain) tail — see World.hlsl for the layout notes; must stay identical in all three
    // lit shaders and in the CPU-side WetnessCBData.
    float4x4 RainViewProj;
    float    SceneWetness;      float RainFxWeight;     float RainTime;   uint RainShadowIndex;
    uint     DistortionIndex;   float RainShadowMapSize; float2 _wetpad;
    // --- Screen-space AO block, 80 bytes, written by UploadAoScreenConstants (kAoReprojCbOffset). Only the
    // first float2 is live: 1/screen-size, which SampleScreenSpaceAO turns SV_Position into a mask UV with.
    // The other 72 bytes are the hole left by the AO REPROJECTION constants (previous-frame view-proj + depth
    // index) from back when the mask was built off a previous-frame depth SNAPSHOT; RenderSSAO now runs off
    // THIS frame's depth prepass and nothing reprojects. The hole stays so the sky-IBL tail below keeps its
    // byte offset (kSkyIblCbOffset = 432). Keep in sync across World/Vob/Skeletal/Vegetation/Decal.hlsl.
    float2   AoInvRes;          float2 _aopad0;
    float4   _aoReserved[4];
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
// normal + ORM + DIFFUSE maps. MatNormalIndex == 0xFFFFFFFF -> no normal map (skip perturb); MatOrmIndex is
// always valid (the 1x1 default ORM = AO 1 / rough 0.5 / metal 0 when the material has no _FX map); its top 2
// bits pack the FxMap's channel layout for SampleOrm() to decode (see PBRLighting.hlsl). MatDiffuseIndex is
// always valid too (the 1x1 black texture when the material's texture isn't cached in yet), and replaces what
// used to be a per-material descriptor-table bind — same layout the world/VOB ExecuteIndirect commands push.
cbuffer MaterialCB : register(b6) { uint MatNormalIndex; uint MatOrmIndex; uint MatDiffuseIndex; };
// The diffuse for every non-ghost entry point. One helper so the color/prepass/shadow-clip variants can never
// drift apart on which slot or sampler they read.
float4 SampleSkelDiffuse( float2 uv )
{
    Texture2D difTex = ResourceDescriptorHeap[MatDiffuseIndex];
    return difTex.Sample( smp, uv );
}
TextureCubeArray        PointShadowCubes : register(t5);   // point-light shadow cubes (P2.10d), R16 linear depth
// Simple-SSAO mask (bindless, set once per frame — see D3D12GraphicsEngine::RenderSSAO/m_ActiveAOMaskSrvSlot).
// b8, not b7: b7 is GhostCB below, read only by the separate GhostSkeletal root sig/PSO (PSGhost), not PSMain.
cbuffer AOCB : register(b8) { uint AoMaskIndex; };
// Point-clamp for the AO mask — see World.hlsl's identical declaration for why Sample (not Load) is required.
SamplerState smpAoClamp : register(s1);
// SampleScreenSpaceAO — see World.hlsl; needs AOCB/smpAoClamp declared above.
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
    float4 t = SampleSkelDiffuse( i.uv );
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
    float shadow = ComputeSunShadow( i.wpos, N, vertLighting );
    // Scene wetness (rain) — see World.hlsl's PSMain for why this runs after the cascade lookup.
    float3 V = normalize( CamPosWS - i.wpos );
    float wetSheen;
    float wetness = ApplySceneWetness( i.wpos, V, N, albedo, orm.g, wetSheen );
    float ssao = SampleScreenSpaceAO( i.clip.xy );
    float3 rgb = ComputeSunLightingPBR( i.wpos, N, albedo, vertLighting, shadow, orm.g, orm.b, orm.r, ssao );
    rgb *= lerp( 1.0, 0.8, wetness );
    rgb += AccumTiledPointLights( i.clip.xyz, i.wpos, N, albedo, orm.g, orm.b );   // dynamic point lights on top (PBR)
    rgb += wetSheen * ( 1.0 + shadow ) * SrgbToLinear( SunColor ) * SunIntensity;
    float f = saturate( ( i.fogDist - FogNear ) / max( 1.0, FogFar - FogNear ) );
    return float4( lerp( rgb, SrgbToLinear( FogColor ), f ), 1.0 );
}

// --- Depth-prepass variant (P2.9b-4b: adds skinned NPC/monster meshes to the Forward+ opaque depth prepass) ---
// Same matrix-palette skinning as VSMain (so the depth matches the color pass bit-for-bit) but outputs only
// clip + uv; reads b0/b1/b2 (+ b6's diffuse index in the PS), NOT fog/light CBs — so it needs no
// BindFrameLights (no light-loop hang).
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
    float4 t = SampleSkelDiffuse( i.uv );
    clip( t.a - 0.5 );          // same cutout as PSMain so alpha edges don't lay down depth
    return float4( 0, 0, 0, 1 );   // discarded: the PSO's color write mask is 0 (depth-only pass)
}
// Shadow caster (P2.9c-2): void PS so the depth-only shadow PSO binds NO render target without a validation
// warning; only alpha-clips the cutout so alpha edges don't cast solid shadows.
void PSShadowClip( VS_DEPTH_OUT i )
{
    clip( SampleSkelDiffuse( i.uv ).a - 0.5 );
}

// Ghost/transparency skeletal VOBs (D3D12PipelineState::CreateGhostSkeletal): invisible-potion/fade NPCs.
// Reuses VSDepth's matrix-palette skinning (identical pose to the color/prepass/shadow draws) — unlit diffuse
// sample, alpha multiplied by a per-vob fade factor, no alpha-clip (a fading ghost should smoothly disappear,
// not pop). Mirrors D3D11's PS_TransparencySkel / the non-skeletal PSGhost in Preview.hlsl — including its
// sRGB linearize, which both ghost shaders need and neither originally had (see the note in PSGhost).
cbuffer GhostCB : register(b7) { float GhostAlpha; float3 _GhostPad; }

float4 PSGhost( VS_DEPTH_OUT i ) : SV_TARGET
{
    float4 t = SampleSkelDiffuse( i.uv );
    // Linearize — m_SceneColor is a LINEAR HDR target on D3D12 (SrgbToLinear comes from PBRLighting.hlsl,
    // included above). D3D11's PS_TransparencySkel returns the raw texel because its HDR buffer is
    // gamma-space; porting that verbatim is what made ghosts read far too bright.
    return float4( SrgbToLinear( t.rgb ), t.a * GhostAlpha );
}

// --- G-buffer prepass variant: depth + motion vectors + normals (see include/MotionVectors.hlsl) ---
// A separate entry point from VSDepth above, because that blob also builds the CSM cascade skeletal caster PSO,
// which binds no render targets and never binds b9.
//
// Skeletals are the one geometry class whose motion is genuinely per-vertex: an NPC's world matrix barely moves
// while a swung arm crosses half the screen. So the previous position is re-skinned through the PREVIOUS pose
// (Bones[PrevBoneOffset + idx]) and pushed through the previous world matrix — the same two-skinning structure
// D3D11's VS_ExSkeletal.hlsl uses (ApplySkinningCurrent / ApplySkinningPrevious with BT_CURR / BT_PREV).
#define MOTIONCB_REGISTER b9
#include "include/MotionVectors.hlsl"

struct VS_GBUF_OUT
{
    float4 clip     : SV_POSITION;
    float2 uv       : TEXCOORD0;
    float3 wnrm     : TEXCOORD1;
    float4 currClip : TEXCOORD2;
    float4 prevClip : TEXCOORD3;
};

VS_GBUF_OUT VSDepthGBuf( VS_IN i )
{
    float3 skinnedPos    = float3( 0, 0, 0 );
    float3 skinnedNormal = float3( 0, 0, 0 );
    float3 prevPos       = float3( 0, 0, 0 );
    float3 prevNormal    = float3( 0, 0, 0 );
    [unroll]
    for ( int b = 0; b < 4; ++b )
    {
        uint  idx = i.boneIndices[b];
        float w   = i.weights[b];
        float4x4 bone     = Bones[idx];
        float4x4 prevBone = Bones[PrevBoneOffset + idx];
        skinnedPos    += w * mul( float4( i.pos[b].xyz, 1.0 ), bone ).xyz;
        skinnedNormal += w * mul( i.normal, (float3x3)bone );
        prevPos       += w * mul( float4( i.pos[b].xyz, 1.0 ), prevBone ).xyz;
        prevNormal    += w * mul( i.normal, (float3x3)prevBone );
    }
    // Fatness is applied to both poses (it inflates along the normal and is itself animated for some monsters),
    // mirroring D3D11's ApplySkinningPrevious feeding PI_ModelFatness * prevNormal into M_PrevWorld.
    float3 worldPos     = mul( float4( skinnedPos + Fatness * skinnedNormal, 1.0 ), M_World ).xyz;
    float3 prevWorldPos = mul( float4( prevPos + Fatness * prevNormal, 1.0 ), M_PrevWorld ).xyz;

    VS_GBUF_OUT o;
    o.clip     = mul( float4( worldPos, 1.0 ), ViewProj );
    o.uv       = i.uv;
    o.wnrm     = mul( skinnedNormal, (float3x3)M_World );
    o.currClip = mul( float4( worldPos, 1.0 ), UnjitteredViewProj );
    o.prevClip = mul( float4( prevWorldPos, 1.0 ), PrevViewProj );
    return o;
}

GBUF_OUT PSDepthClipGBuf( VS_GBUF_OUT i )
{
    clip( SampleSkelDiffuse( i.uv ).a - 0.5 );   // identical cutout to PSDepthClip
    return MakeGBufOut( i.currClip, i.prevClip, i.wnrm );
}
