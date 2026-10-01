cbuffer FrameCB    : register(b0) { float4x4 ViewProj; };
// Instance record + bone palettes (t3) and the per-draw SkelInstanceRow (b10) — see include/SkeletalInstance.hlsl.
#include "include/SkeletalInstance.hlsl"
#define FOGCB_REGISTER b3
#include "include/FogCB.hlsl"
#undef FOGCB_REGISTER
#include "include/ForwardPlusTypes.hlsl"
// Skeletal already uses b3 (fog), so its LightCB lands at b4 (world/VOB use b2).
#define LIGHTCB_REGISTER b4
#include "include/LightCB.hlsl"
#undef LIGHTCB_REGISTER

// Forward+ tiled point lights (root-descriptor SRVs + per-cluster mask) — see the world shader for the rationale.
StructuredBuffer<GPULight>  Lights        : register(t1);
StructuredBuffer<LightGrid> LightGridBuf  : register(t2);

// No t0 diffuse texture: EVERY entry point in this file (lit, depth-prepass, shadow-clip and ghost) fetches its
// diffuse bindlessly from MaterialCB.MatDiffuseIndex, so neither Skeletal.RootSig nor GhostSkeletal.RootSig
// carries an SRV descriptor table — see the note on MaterialCB below.
SamplerState smp : register(s0);

// CSM sun-shadow sampling (P2.9c-4b). Skeletal already uses b3 (fog) + b4 (light count), so the shadow CB
// lands at b5 here (world/VOB use b3); t4/s2 are free. Same select+PCF math as the world/VOB block.
#define SHADOWCB_REGISTER b5
#include "include/ShadowCB.hlsl"
#undef SHADOWCB_REGISTER
Texture2DArray          ShadowMap : register(t4);
SamplerComparisonState  shadowCmp : register(s2);
// Per-material bindless indices (root consts b6): SM6.6 ResourceDescriptorHeap[...] indices for this material's
// normal + ORM + DIFFUSE maps. MatNormalIndex == 0xFFFFFFFF -> no normal map (skip perturb); MatOrmIndex is
// always valid (the 1x1 default ORM = AO 1 / rough 0.5 / metal 0 when the material has no _FX map); its top 2
// bits pack the FxMap's channel layout for SampleOrm() to decode (see PBRLighting.hlsl). MatDiffuseIndex is
// always valid too (the 1x1 black texture when the material's texture isn't cached in yet), and replaces what
// used to be a per-material descriptor-table bind — same layout the world/VOB ExecuteIndirect commands push.
#include "include/MaterialCB.hlsl"
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
#define AOCB_REGISTER b8
#include "include/AOCB.hlsl"
#undef AOCB_REGISTER
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

// motion vectors + octahedral normals; b9 because b5 is the shadow CB on this root signature.
#define MOTIONCB_REGISTER b9
#include "include/MotionVectors.hlsl"

// Every pass but the ghost one draws vertices SkinVertices.hlsl already posed in world space: stream 0 holds what a
// depth/shadow pass needs, stream 1 the normal and the previous-frame position.
struct VS_POSED_DEPTH_IN
{
    float3 pos : POSITION;
    float2 uv  : TEXCOORD0;
};
struct VS_POSED_IN
{
    float3 pos     : POSITION;
    float2 uv      : TEXCOORD0;
    float2 nrm     : NORMAL;      // octahedral, R16G16_SNORM
    float3 prevPos : TEXCOORD1;
};
struct VS_OUT { float4 clip : SV_POSITION; float2 uv : TEXCOORD0; float4 col : TEXCOORD1; float fogDist : TEXCOORD2; float3 wpos : TEXCOORD3; float3 wnrm : TEXCOORD4; };

VS_OUT VSMain( VS_POSED_IN i )
{
    VS_OUT o;
    o.clip = mul( float4( i.pos, 1.0 ), ViewProj );
    o.uv  = i.uv;
    o.col = LoadSkelInstance().ModelColor;   // per-vob ground light + focus sentinel
    o.wpos = i.pos;
    o.wnrm = DecodeOctNormalMV( i.nrm );
    o.fogDist = distance( i.pos, CamPosWS );
    return o;
}

float4 PSMain( VS_OUT i ) : SV_TARGET
{
    float4 t = SampleSkelDiffuse( i.uv );
    clip( t.a - 0.5 );
    float3 N = normalize( i.wnrm );
    float3 geomN = N;
    if ( MatNormalIndex != 0xffffffff )
    {
        Texture2D nrmTex = ResourceDescriptorHeap[MatNormalIndex];
        N = PerturbNormal( N, i.wpos, nrmTex, i.uv, smp );
    }
    float3 orm = SampleOrm( MatOrmIndex, i.uv );   // AO/Roughness/Metallic, decoded per the material's FxMap layout
    float3 albedo = SrgbToLinear( t.rgb );
    albedo = DelightDiffuse( albedo );
    float vertLighting = i.col.g;               // ModelColor green (white=1 for NPCs → no baked AO reduction)
    uint2 rtMask = LoadRtShadowMask( i.clip.xy );
    float shadow = ComputeSunShadow( rtMask, i.wpos, geomN, vertLighting );
    // Scene wetness (rain) — see World.hlsl's PSMain for why this runs after the cascade lookup.
    float3 V = normalize( CamPosWS - i.wpos );
    WetSurface wet = ApplySceneWetness( i.wpos, geomN, N, albedo, orm.g );
    float ssao = SampleScreenSpaceAO( i.clip.xy );
    float3 rgb = ComputeSunLightingPBR( i.wpos, N, albedo, vertLighting, shadow, orm.g, orm.b, orm.r, ssao, WetBaseSpecularScale( wet ) );
    rgb *= mad(wet.wetness, 0.8 - 1.0, 1.0);
    rgb = ApplyWetCoat( rgb, wet, i.wpos, shadow, vertLighting, orm.r, ssao );
    rgb += AccumTiledPointLights( i.clip.xyz, i.wpos, N, albedo, orm.g, orm.b, wet, rtMask );   // dynamic point lights on top (PBR)
    // Opaque-surface SSR (temporal, D3D12 only) — see World.hlsl's PSMain for the full explanation. The
    // weight MUST be PBR_FresnelSchlick, not an ad hoc curve (see EvaluateOpaqueSSR's header comment).
    {
        float ssrConfidence;
        float3 ssrColor = EvaluateOpaqueSSR( i.wpos, N, V, orm.g, ssrConfidence );
        float3 ssrF0 = lerp( float3( 0.04, 0.04, 0.04 ), albedo, orm.b );
        float3 ssrFresnel = PBR_FresnelSchlick( saturate( dot( N, V ) ), ssrF0 );
        rgb += ssrColor * ssrConfidence * ssrFresnel;
    }
    rgb *= 1.0f + step( 1.5f, i.col.a );   // focus highlight
    float f = saturate( ( i.fogDist - FogNear ) / max( 1.0, FogFar - FogNear ) );
    return float4( lerp( rgb, SrgbToLinear( FogColor ), f ), 1.0 );
}

// --- Depth-prepass variant (P2.9b-4b: adds skinned NPC/monster meshes to the Forward+ opaque depth prepass) ---
// Also the CSM caster VS. Reads b0 (+ b6's diffuse index in the PS), NOT fog/light CBs — so it needs no
// BindFrameLights (no light-loop hang).
struct VS_DEPTH_OUT { float4 clip : SV_POSITION; float2 uv : TEXCOORD0; };
VS_DEPTH_OUT VSDepth( VS_POSED_DEPTH_IN i )
{
    VS_DEPTH_OUT o;
    o.clip = mul( float4( i.pos, 1.0 ), ViewProj );
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
// Ghosts never pass through PrepareFrameSkeletals, so they are not compute-skinned: VSGhost skins the raw arena
// vertex itself, the same math as SkinVertices.hlsl — unlit diffuse
// sample, alpha multiplied by a per-vob fade factor, no alpha-clip (a fading ghost should smoothly disappear,
// not pop). Mirrors D3D11's PS_TransparencySkel / the non-skeletal PSGhost in Preview.hlsl — including its
// sRGB linearize, which both ghost shaders need and neither originally had (see the note in PSGhost).
#define GHOSTCB_REGISTER b7
#include "include/GhostCB.hlsl"
#undef GHOSTCB_REGISTER

struct VS_SKIN_IN
{
    float4 pos[4]         : POSITION;    // 4 per-bone-space positions (half4)
    float3 normal         : NORMAL;
    float3 bindPoseNormal : TEXCOORD0;   // unused
    float2 uv             : TEXCOORD1;
    uint4  boneIndices    : BONEIDS;
    float4 weights        : WEIGHTS;
};
VS_DEPTH_OUT VSGhost( VS_SKIN_IN i )
{
    const SkeletalInstance inst = LoadSkelInstance();
    float3 skinnedPos, skinnedNormal;
    SkinVertex( i.pos, i.normal, i.boneIndices, i.weights, inst.BoneRow, skinnedPos, skinnedNormal );
    VS_DEPTH_OUT o;
    o.clip = mul( float4( SkelToWorld( inst.World, skinnedPos, skinnedNormal, inst.Fatness ), 1.0 ), ViewProj );
    o.uv = i.uv;
    return o;
}

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
// while a swung arm crosses half the screen. SkinVertices.hlsl therefore poses each vertex twice, through the
// current and the PREVIOUS pose and world matrix (D3D11's ApplySkinningCurrent / ApplySkinningPrevious).

struct VS_GBUF_OUT
{
    float4 clip     : SV_POSITION;
    float2 uv       : TEXCOORD0;
    float3 wnrm     : TEXCOORD1;
    float4 currClip : TEXCOORD2;
    float4 prevClip : TEXCOORD3;
};

VS_GBUF_OUT VSDepthGBuf( VS_POSED_IN i )
{
    VS_GBUF_OUT o;
    o.clip     = mul( float4( i.pos, 1.0 ), ViewProj );
    o.uv       = i.uv;
    o.wnrm     = DecodeOctNormalMV( i.nrm );
    o.currClip = mul( float4( i.pos, 1.0 ), UnjitteredViewProj );
    o.prevClip = mul( float4( i.prevPos, 1.0 ), PrevViewProj );
    return o;
}

GBUF_OUT PSDepthClipGBuf( VS_GBUF_OUT i )
{
    clip( SampleSkelDiffuse( i.uv ).a - 0.5 );   // identical cutout to PSDepthClip
    return MakeGBufOut( i.currClip, i.prevClip, i.wnrm );
}
