cbuffer WorldCB : register(b0) { float4x4 ViewProj; };   // default column-major packing (see world shader)
cbuffer FogCB   : register(b1) { float3 FogColor; float FogNear; float3 CamPosWS; float FogFar; };
cbuffer LightCB : register(b2) { uint LightCount; uint NumTilesX; uint LimitLightIntensity; uint PointShadowLowIndex; uint PointShadowDynIndex; };
// Wind sway for instanced VOBs (flags/foliage). Mirrors D3D11's WindParams cbuffer (VS_ExInstancedObj.hlsl);
// minHeight/maxHeight are the flat (non-WIND_META_SRV) per-visual bounding-box fallback, refreshed per visual
// by DrawVobsInstanced before each visual's draw calls (see WindMinHeight/WindMaxHeight below).
cbuffer WindCB : register(b4)
{
    float3 WindDir;        float WindGlobalTime;
    float  WindMinHeight;  float WindMaxHeight;   float2 _windPad0;
    float3 WindPlayerPos;  float _windPad1;
};

#include "include/ForwardPlusTypes.hlsl"
#include "include/Wind.hlsl"

// Forward+ tiled point lights (root-descriptor SRVs + per-tile grid)
StructuredBuffer<GPULight>  Lights        : register(t1);
StructuredBuffer<LightGrid> LightGridBuf  : register(t2);
StructuredBuffer<uint>      LightIndexBuf : register(t3);

Texture2D    tx  : register(t0);
SamplerState smp : register(s0);

cbuffer ShadowCB : register(b3)
{
    float4x4 CascadeViewProj[NUM_CSM_CASCADES];
    float3   SunDirWS;          float ShadowMapSize;
    float3   SunColor;          float SunIntensity;
    float3   CascadeTexelWorld; float AmbientStrength;
    float    ShadowAOStrength;  float WorldAOStrength;   // vertLighting -> AO modulation weights
    // How hard baked vertex light gates the sky-IBL AMBIENT term (PBRLighting.hlsl ComputeSunLightingPBR).
    // 0 = the old unoccluded behaviour, 1 = interiors get no sky ambient at all. See the note there.
    float    SkyOccStrength;    float _shpad;
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
// MatDiffuseIndex (3rd field) is read only by the *Bindless PS variants below — the ExecuteIndirect VOB path
// (P2.12) sets the diffuse SRV heap slot as a root constant instead of a per-draw t0 descriptor table, so the
// whole instanced-VOB pass (color/depth/shadow) submits as one ExecuteIndirect. The classic t0-table PS
// (PSMain/PSDepthClip/PSShadowClip, used by node attachments) simply never reads the 3rd field.
cbuffer MaterialCB : register(b6) { uint MatNormalIndex; uint MatOrmIndex; uint MatDiffuseIndex; };
TextureCubeArray        PointShadowCubes : register(t5);
// Simple-SSAO mask (bindless, set once per frame — see D3D12GraphicsEngine::RenderSSAO/m_ActiveAOMaskSrvSlot).
cbuffer AOCB : register(b7) { uint AoMaskIndex; };
// Point-clamp for the AO mask — see World.hlsl's identical declaration for why Sample (not Load) is required.
SamplerState smpAoClamp : register(s1);
// SampleScreenSpaceAO — see World.hlsl; needs AOCB/smpAoClamp + the ShadowCB reprojection tail declared above.
#include "include/ScreenSpaceAO.hlsl"

// DelightDiffuse, SamplePointShadow, ComputeSunShadow, the Cook-Torrance PBR helpers, PerturbNormal/
// CotangentFrame, ComputeSunLightingPBR and AccumTiledPointLights are shared with World.hlsl/Skeletal.hlsl.
#include "include/PBRLighting.hlsl"
// Wet-ground / scene-wetness (rain) — see World.hlsl.
#include "include/Wetness.hlsl"

// Shared by VSMain and VSDepth so the depth prepass writes EXACTLY the bit-for-bit same swayed position as the
// color pass — any divergence here would make the reversed-Z GREATER_EQUAL depth test discard swaying geometry
// the color pass draws in front of where the (unswayed) prepass depth said it should be.
float3 ApplyVobWind( float3 pos, float2 iwind, float4x4 iworld )
{
    float3 localPos = pos;

    // "Hero moves the bushes": push away from the player, height-masked + distance-falloff.
    if ( iwind.y > 0 )
        localPos += ApplyHeroInfluence( WindPlayerPos, localPos, WindMinHeight, WindMaxHeight, iworld );

    // Tree/flag/leaf sway.
    if ( iwind.x > 0 )
    {
        float heightRange = max( WindMaxHeight - WindMinHeight, 0.001 );
        float heightNorm  = saturate( ( pos.y - WindMinHeight ) / heightRange );
        localPos += ApplyTreeWind( pos, normalize( WindDir ), heightNorm, WindGlobalTime, iworld, WindMaxHeight, iwind.x );
    }

    return localPos;
}

struct VS_IN
{
    float3   pos      : POSITION;
    float3   nrm      : NORMAL;                  // ExVertexStruct object-space float3 normal (@12)
    float2   uv       : TEXCOORD0;
    float4x4 iworld   : INSTANCE_WORLD_MATRIX;
    float4   icolor   : INSTANCE_COLOR;
    // VobInstanceInfo::{windStrenth, canBeAffectedByPlayer} (@132/@136) — 0 for non-wind-flagged vobs, so the
    // branches below are no-ops for ordinary instances (matches D3D11's per-instance InstanceWind.x/y > 0 gate).
    float2   iwind    : INSTANCE_WINDFLUENCE;
};
struct VS_OUT { float4 clip : SV_POSITION; float2 uv : TEXCOORD0; float4 col : TEXCOORD1; float fogDist : TEXCOORD2; float3 wpos : TEXCOORD3; float3 wnrm : TEXCOORD4; };

VS_OUT VSMain( VS_IN i )
{
    VS_OUT o;
    float3 localPos = ApplyVobWind( i.pos, i.iwind, i.iworld );

    float3 worldPos = mul( float4( localPos, 1.0 ), i.iworld ).xyz;
    o.clip = mul( float4( worldPos, 1.0 ), ViewProj );
    o.uv  = i.uv;
    o.col = i.icolor;
    o.wpos = worldPos;
    o.wnrm = mul( i.nrm, (float3x3)i.iworld );
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
    float3 orm = SampleOrm( MatOrmIndex, i.uv );
    float3 albedo = SrgbToLinear( t.rgb );
    albedo = DelightDiffuse( albedo );
    float vertLighting = i.col.g;
    float shadow = ComputeSunShadow( i.wpos, N, vertLighting );
    // Scene wetness (rain) — see World.hlsl's PSMain for why this runs after the cascade lookup.
    float3 V = normalize( CamPosWS - i.wpos );
    float wetSheen;
    float wetness = ApplySceneWetness( i.wpos, V, N, albedo, orm.g, wetSheen );
    float ssao = SampleScreenSpaceAO( i.wpos );
    float3 rgb = ComputeSunLightingPBR( i.wpos, N, albedo, vertLighting, shadow, orm.g, orm.b, orm.r, ssao );
    rgb *= lerp( 1.0, 0.8, wetness );
    rgb += AccumTiledPointLights( i.clip.xy, i.wpos, N, albedo, orm.g, orm.b );
    rgb += wetSheen * ( 1.0 + shadow ) * SrgbToLinear( SunColor ) * SunIntensity;
    float f = saturate( ( i.fogDist - FogNear ) / max( 1.0, FogFar - FogNear ) );
    return float4( lerp( rgb, SrgbToLinear( FogColor ), f ), 1.0 );
}

// Same per-instance wind fields as VS_IN (see ApplyVobWind) — the depth prepass must sway identically to VSMain,
// or the color pass's swayed fragments fail the GREATER_EQUAL depth test against an unswayed prepass depth.
struct VS_DEPTH_IN  { float3 pos : POSITION; float2 uv : TEXCOORD0; float4x4 iworld : INSTANCE_WORLD_MATRIX; float2 iwind : INSTANCE_WINDFLUENCE; };
struct VS_DEPTH_OUT { float4 clip : SV_POSITION; float2 uv : TEXCOORD0; };
VS_DEPTH_OUT VSDepth( VS_DEPTH_IN i )
{
    VS_DEPTH_OUT o;
    float3 localPos = ApplyVobWind( i.pos, i.iwind, i.iworld );
    float3 worldPos = mul( float4( localPos, 1.0 ), i.iworld ).xyz;
    o.clip = mul( float4( worldPos, 1.0 ), ViewProj );
    o.uv = i.uv;
    return o;
}
float4 PSDepthClip( VS_DEPTH_OUT i ) : SV_TARGET
{
    float4 t = tx.Sample( smp, i.uv );
    clip( t.a - 0.5 );
    return float4( 0, 0, 0, 1 );
}
void PSShadowClip( VS_DEPTH_OUT i )
{
    clip( tx.Sample( smp, i.uv ).a - 0.5 );
}

// --- Node-attachment variant (weapons/heads/lamps, and morph meshes attached to a bone node) ---
// Node attachments never sway in the wind, so they reuse VS_IN/VS_DEPTH_IN's INSTANCE_WINDFLUENCE slot to instead
// carry {Fatness, Scaling} — mirrors D3D11's VS_ExNode.hlsl (VS_ExConstantBuffer_PerInstanceNode::Fatness/Scaling),
// which every node attachment (not just morph meshes) needs: a NON-morph attachment gets Fatness=0/Scaling=1 (a
// no-op, matching how it renders today), but an MMS morph-mesh attachment (facial morphs, bow/crossbow draw
// meshes) gets a real inflate-along-normal + rescale so it fits the model's Fatness slider the same way D3D11
// does — omitting it left morph attachments mismatched in size against the (Fatness-affected) skeletal body,
// which reads as the attachment poking through / clipping against the head. VSDepthAttach needs the vertex
// NORMAL for the inflate, unlike the plain VOB depth prepass, so it takes the fuller VS_IN (not VS_DEPTH_IN).
float3 ApplyAttachFatness( float3 pos, float3 nrm, float2 fatnessScaling )
{
    return ( pos + fatnessScaling.x * nrm ) * fatnessScaling.y;
}

VS_OUT VSMainAttach( VS_IN i )
{
    VS_OUT o;
    float3 localPos = ApplyAttachFatness( i.pos, i.nrm, i.iwind );

    float3 worldPos = mul( float4( localPos, 1.0 ), i.iworld ).xyz;
    o.clip = mul( float4( worldPos, 1.0 ), ViewProj );
    o.uv  = i.uv;
    o.col = i.icolor;
    o.wpos = worldPos;
    o.wnrm = mul( i.nrm, (float3x3)i.iworld );
    o.fogDist = length( worldPos - CamPosWS );
    return o;
}

VS_DEPTH_OUT VSDepthAttach( VS_IN i )
{
    VS_DEPTH_OUT o;
    float3 localPos = ApplyAttachFatness( i.pos, i.nrm, i.iwind );
    float3 worldPos = mul( float4( localPos, 1.0 ), i.iworld ).xyz;
    o.clip = mul( float4( worldPos, 1.0 ), ViewProj );
    o.uv = i.uv;
    return o;
}

// --- Bindless-diffuse PS variants (ExecuteIndirect VOB path, P2.12) ---
// Identical to PSMain/PSDepthClip/PSShadowClip except the diffuse texture comes from the SRV heap by index
// (b6 MatDiffuseIndex) instead of the t0 descriptor table — ExecuteIndirect can set root constants but not
// descriptor tables, so the whole instanced-VOB pass (each visual/material/mesh a command) collapses into a
// single indirect submit. Only the instanced-VOB PSOs use these; node-attachment PSOs keep the t0 variants.
float4 PSMainBindless( VS_OUT i ) : SV_TARGET
{
    Texture2D difTex = ResourceDescriptorHeap[MatDiffuseIndex];
    float4 t = difTex.Sample( smp, i.uv );
    clip( t.a - 0.5 );
    float3 N = normalize( i.wnrm );
    if ( MatNormalIndex != 0xffffffff )
    {
        Texture2D nrmTex = ResourceDescriptorHeap[MatNormalIndex];
        N = PerturbNormal( N, i.wpos, nrmTex, i.uv, smp );
    }
    float3 orm = SampleOrm( MatOrmIndex, i.uv );
    float3 albedo = SrgbToLinear( t.rgb );
    albedo = DelightDiffuse( albedo );
    float vertLighting = i.col.g;
    float shadow = ComputeSunShadow( i.wpos, N, vertLighting );
    // Scene wetness (rain) — see World.hlsl's PSMain for why this runs after the cascade lookup.
    float3 V = normalize( CamPosWS - i.wpos );
    float wetSheen;
    float wetness = ApplySceneWetness( i.wpos, V, N, albedo, orm.g, wetSheen );
    float ssao = SampleScreenSpaceAO( i.wpos );
    float3 rgb = ComputeSunLightingPBR( i.wpos, N, albedo, vertLighting, shadow, orm.g, orm.b, orm.r, ssao );
    rgb *= lerp( 1.0, 0.8, wetness );
    rgb += AccumTiledPointLights( i.clip.xy, i.wpos, N, albedo, orm.g, orm.b );
    rgb += wetSheen * ( 1.0 + shadow ) * SrgbToLinear( SunColor ) * SunIntensity;
    float f = saturate( ( i.fogDist - FogNear ) / max( 1.0, FogFar - FogNear ) );
    return float4( lerp( rgb, SrgbToLinear( FogColor ), f ), 1.0 );
}

// --- BLENDED instanced VOBs (cobwebs, hanging cloth, magic sheets) ---
// Port of D3D11's DrawFrameAlphaMeshes. Every VOB material whose alpha func is a real blend mode (BLEND/ADD)
// is peeled out of the opaque ExecuteIndirect set by BuildVobDrawCommands and drawn here instead, blended and
// WITHOUT writing depth. It cannot go through PSMainBindless: that one clip()s at a - 0.5 and returns alpha 1,
// which is exactly what makes a spider web read as a solid slab.
//
// LIT, unlike D3D11's PS_Simple for this pass. D3D11 can leave these unlit because of how its transparency
// pass is exposed; on this backend the scene target holds linear radiance that is auto-exposed and tonemapped
// later, so an unlit surface at full albedo blows out against the dimly-lit room around it (the same defect
// the decal pass had). Modulating by the raw per-instance ground colour instead is no fix either — indoor
// vobs carry a flat 0.05 grey there (DEFAULT_LIGHTMAP_POLY_COLOR) because indoor light comes entirely from the
// point lights, so that would render cobwebs nearly black. So: the full Forward+ evaluation, identical to
// PSMainBindless apart from the alpha handling, which keeps a peeled material shading exactly as it did while
// it was still in the opaque set.
//
// No fog term: this one PS serves both BLEND and ADD, and lerping toward the fog colour brightens an additive
// surface rather than fading it. D3D11 doesn't fog its alpha meshes either.
float4 PSAlphaBlendBindless( VS_OUT i ) : SV_TARGET
{
    Texture2D difTex = ResourceDescriptorHeap[MatDiffuseIndex];
    float4 t = difTex.Sample( smp, i.uv );
    float3 N = normalize( i.wnrm );
    if ( MatNormalIndex != 0xffffffff )
    {
        Texture2D nrmTex = ResourceDescriptorHeap[MatNormalIndex];
        N = PerturbNormal( N, i.wpos, nrmTex, i.uv, smp );
    }
    float3 orm = SampleOrm( MatOrmIndex, i.uv );
    float3 albedo = SrgbToLinear( t.rgb );
    albedo = DelightDiffuse( albedo );
    float shadow = ComputeSunShadow( i.wpos, N, i.col.g );
    float ssao = SampleScreenSpaceAO( i.wpos );
    float3 rgb = ComputeSunLightingPBR( i.wpos, N, albedo, i.col.g, shadow, orm.g, orm.b, orm.r, ssao );
    rgb += AccumTiledPointLights( i.clip.xy, i.wpos, N, albedo, orm.g, orm.b );
    return float4( rgb, t.a * i.col.a );
}

float4 PSDepthClipBindless( VS_DEPTH_OUT i ) : SV_TARGET
{
    Texture2D difTex = ResourceDescriptorHeap[MatDiffuseIndex];
    clip( difTex.Sample( smp, i.uv ).a - 0.5 );
    return float4( 0, 0, 0, 1 );
}

void PSShadowClipBindless( VS_DEPTH_OUT i )
{
    Texture2D difTex = ResourceDescriptorHeap[MatDiffuseIndex];
    clip( difTex.Sample( smp, i.uv ).a - 0.5 );
}

// --- G-buffer prepass variants: depth + motion vectors + normals (see include/MotionVectors.hlsl) ---
// New entry points rather than an extension of VSDepth/VSDepthAttach, because those blobs also build the CSM
// cascade caster PSOs, which bind no render targets and never bind b5.
//
// Both take the FULLER VS_IN (VSDepth's own VS_DEPTH_IN has no NORMAL) plus the previous-frame instance matrix.
// That matrix costs nothing to fetch: VobInstanceInfo::prevWorld has been sitting at offset 64 of the instance
// stream all along — D3D12RenderQueue::PushStaticVob fills it — it simply was never declared in an input layout.
#include "include/MotionVectors.hlsl"

struct VS_GBUF_IN
{
    float3   pos       : POSITION;
    float3   nrm       : NORMAL;
    float2   uv        : TEXCOORD0;
    float4x4 iworld    : INSTANCE_WORLD_MATRIX;
    float4x4 iprevworld: INSTANCE_PREV_WORLD_MATRIX;   // VobInstanceInfo::prevWorld (@64)
    float2   iwind     : INSTANCE_WINDFLUENCE;
};
struct VS_GBUF_OUT
{
    float4 clip     : SV_POSITION;
    float2 uv       : TEXCOORD0;
    float3 wnrm     : TEXCOORD1;
    float4 currClip : TEXCOORD2;
    float4 prevClip : TEXCOORD3;
};

VS_GBUF_OUT VSDepthGBuf( VS_GBUF_IN i )
{
    VS_GBUF_OUT o;
    float3 localPos = ApplyVobWind( i.pos, i.iwind, i.iworld );
    float3 worldPos = mul( float4( localPos, 1.0 ), i.iworld ).xyz;
    o.clip = mul( float4( worldPos, 1.0 ), ViewProj );
    o.uv   = i.uv;
    o.wnrm = mul( i.nrm, (float3x3)i.iworld );
    o.currClip = mul( float4( worldPos, 1.0 ), UnjitteredViewProj );
    // The SWAYED local position is reused for the previous frame, so a wind-swayed vob reports only its rigid
    // motion. This deliberately differs from D3D11's VS_ExInstancedObj.hlsl, which feeds the UN-swayed `position`
    // to M_PrevWorld and therefore reports the entire sway displacement as if it happened in a single frame —
    // for a tree canopy that is a large bogus velocity every frame, which TAA/FSR resolve as smeared foliage.
    // Under-reporting sway (leaves ghost slightly) is the far smaller error; correcting it properly needs last
    // frame's wind phase, which is a follow-up, not a prerequisite.
    o.prevClip = mul( float4( localPos, 1.0 ), mul( i.iprevworld, PrevViewProj ) );
    return o;
}

VS_GBUF_OUT VSDepthAttachGBuf( VS_GBUF_IN i )
{
    VS_GBUF_OUT o;
    float3 localPos = ApplyAttachFatness( i.pos, i.nrm, i.iwind );
    float3 worldPos = mul( float4( localPos, 1.0 ), i.iworld ).xyz;
    o.clip = mul( float4( worldPos, 1.0 ), ViewProj );
    o.uv   = i.uv;
    o.wnrm = mul( i.nrm, (float3x3)i.iworld );
    o.currClip = mul( float4( worldPos, 1.0 ), UnjitteredViewProj );
    // Attachments (weapons/heads/held items) ride the bone they hang off, so their prevWorld is a genuinely
    // different matrix each frame — this is what gives a swung sword real motion vectors.
    o.prevClip = mul( float4( localPos, 1.0 ), mul( i.iprevworld, PrevViewProj ) );
    return o;
}

GBUF_OUT PSDepthClipBindlessGBuf( VS_GBUF_OUT i )
{
    Texture2D difTex = ResourceDescriptorHeap[MatDiffuseIndex];
    clip( difTex.Sample( smp, i.uv ).a - 0.5 );
    return MakeGBufOut( i.currClip, i.prevClip, i.wnrm );
}
