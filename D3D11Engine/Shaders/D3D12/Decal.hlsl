// Decals (blood splatter, arrows stuck in walls, cobwebs, hanging cloth, spell marks). Gothic models a lot of
// these as camera-aligned or wall-mounted zCDecal quads; DrawDecalList (D3D12Scene.cpp) draws them in two
// passes — alpha-tested/opaque with the lit scene (PSMainLit), then blended over it (PSMainBlend).
//
// The opaque pass is LIT, and so is the alpha-BLENDED half of the transparent pass (ADD/MUL/MUL2 stay unlit —
// see PSMainBlend). They used to emit the raw (linearized) texel with no lighting at all, which is what
// made every cobweb in a torch-lit cellar read as a blazing white slab: the D3D12 scene target holds LINEAR
// radiance that is auto-exposed and tonemapped later, so a wall lit by one distant torch lands far below 1.0
// while an unlit decal sat at its full ~0.9 albedo — and the eye adaptation, seeing a dark room, then scaled
// that difference up rather than down. D3D11 gets away with an unlit PS_Transparency here because its
// transparency pass writes into a differently-exposed buffer; on this backend anything that is not emissive
// (particles/FX are, decals are not) has to go through the same Forward+ evaluation as the world and the VOBs.
//
// The lighting is the World/Vob one minus the per-material maps a decal doesn't have: no normal map, no ORM
// (fully rough dielectric), no vertex lighting (vertLighting = 1, the neutral value for the *AO* terms it
// feeds). The quad's own plane normal carries the shading, flipped toward the viewer because decals render
// with CULL_NONE.
cbuffer FrameCB : register(b0) { float4x4 ViewProj; };   // default column-major packing (see world shader)
cbuffer FogCB   : register(b1) { float3 FogColor; float FogNear; float3 CamPosWS; float FogFar; };
// ProjA/ProjB/NearZ/FarZ feed PBRLighting.hlsl's ComputeZSlice (clustered Forward+, P2.14) — see World.hlsl.
cbuffer LightCB : register(b2) {
    uint LightCount; uint NumTilesX; uint LimitLightIntensity; uint PointShadowLowIndex; uint PointShadowDynIndex;
    float ProjA; float ProjB; float NearZ; float FarZ;
};

#include "include/ForwardPlusTypes.hlsl"

// Forward+ tiled point lights (root-descriptor SRVs + per-cluster mask) — same t1/t2 the world pass binds.
StructuredBuffer<GPULight>  Lights        : register(t1);
StructuredBuffer<LightGrid> LightGridBuf  : register(t2);

Texture2D    tx  : register(t0);
SamplerState smp : register(s0);   // linear CLAMP: a decal is a single [0,1] sprite; wrap would bleed the edge

// Must stay byte-identical to World/Vob/Skeletal.hlsl's ShadowCB and the CPU-side upload structs.
cbuffer ShadowCB : register(b3)
{
    float4x4 CascadeViewProj[NUM_CSM_CASCADES];
    float3   SunDirWS;          float ShadowMapSize;
    float3   SunColor;          float SunIntensity;
    float3   CascadeTexelWorld; float AmbientStrength;
    float    ShadowAOStrength;  float WorldAOStrength;
    float    SkyOccStrength;    float SunSpecularEnabled;
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
    uint     SkyIrradianceIndex; uint  SkySpecularIndex;  float SkySpecularMips; float SkyIblIntensity;
};
Texture2DArray          ShadowMap : register(t4);
SamplerComparisonState  shadowCmp : register(s2);
TextureCubeArray        PointShadowCubes : register(t5);
// Simple-SSAO mask (bindless, set once per frame — see D3D12GraphicsEngine::RenderSSAO/m_ActiveAOMaskSrvSlot).
cbuffer AOCB : register(b7) { uint AoMaskIndex; };
SamplerState smpAoClamp : register(s1);
#include "include/ScreenSpaceAO.hlsl"
// SrgbToLinear, ComputeSunShadow, ComputeSunLightingPBR and AccumTiledPointLights live here — the same
// implementations World.hlsl/Vob.hlsl shade with, so a decal cannot drift away from the surface it sits on.
#include "include/PBRLighting.hlsl"

// A decal has no material maps: fully rough, non-metallic, unoccluded. Named constants so the two entry
// points can't drift apart — the blended pass must shade identically to the opaque one, or the same cobweb
// would change brightness with nothing but its alpha func.
static const float kDecalRoughness = 1.0;
static const float kDecalMetallic  = 0.0;
static const float kDecalAO        = 1.0;

struct VS_IN
{
    float3   pos    : POSITION;
    float2   uv     : TEXCOORD0;
    float4x4 iworld : INSTANCE_WORLD_MATRIX;   // per-instance model matrix (world*offset*scale)
    float4   icolor : INSTANCE_COLOR;          // .a = ghost alpha, .r = shade-lit flag, .gb unused
};
struct VS_OUT
{
    float4 clip    : SV_POSITION;
    float2 uv      : TEXCOORD0;
    float  alpha   : TEXCOORD1;
    float3 wpos    : TEXCOORD2;
    float3 wnrm    : TEXCOORD3;
    float  fogDist : TEXCOORD4;
    float  lit     : TEXCOORD5;
};

VS_OUT VSMain( VS_IN i )
{
    VS_OUT o;
    float3 worldPos = mul( float4( i.pos, 1.0 ), i.iworld ).xyz;
    o.clip  = mul( float4( worldPos, 1.0 ), ViewProj );
    o.uv    = i.uv;
    o.alpha = i.icolor.a;
    o.lit   = i.icolor.r;
    o.wpos  = worldPos;
    // The quad lies in its local XY plane, so +Z is its face normal. The instance matrix's non-uniform XY
    // scale (DecalSize * 2, y negated) doesn't rotate that axis, so transforming it by the 3x3 is exact up to
    // sign — and the sign is resolved per pixel below, since decals draw with CULL_NONE.
    o.wnrm  = mul( float3( 0.0, 0.0, 1.0 ), (float3x3)i.iworld );
    o.fogDist = distance(worldPos, CamPosWS);
    return o;
}

// Shared by both entry points: the Forward+ evaluation at the decal's surface. `svpos` is SV_Position.xyz,
// which AccumTiledPointLights needs to find the light cluster (xy = screen tile, z = hardware depth -> Z slice).
float3 ShadeDecal( float3 albedo, float3 wpos, float3 nrm, float3 svpos )
{
    float3 N = normalize( nrm );
    // Double-sided: shade the face actually turned toward the camera, else a wall decal seen from its "back"
    // side (or any camera-aligned quad whose +Z ended up pointing away) shades as if it faced into the wall.
    if ( dot( N, normalize( CamPosWS - wpos ) ) < 0.0 ) N = -N;

    float shadow = ComputeSunShadow( wpos, N, 1.0 );
    float ssao   = SampleScreenSpaceAO( svpos.xy );
    // vertLighting = 1: decals carry no Gothic vertex/ground light, and 1.0 is the neutral value for the two
    // AO terms it feeds inside ComputeSunLightingPBR (lerp(1, vertLighting, strength) == 1).
    float3 rgb = ComputeSunLightingPBR( wpos, N, albedo, 1.0, shadow,
                                        kDecalRoughness, kDecalMetallic, kDecalAO, ssao );
    rgb += AccumTiledPointLights( svpos, wpos, N, albedo, kDecalRoughness, kDecalMetallic );
    return rgb;
}

float4 PSMainLit( VS_OUT i ) : SV_TARGET   // opaque / alpha-test cutout, fully opaque output
{
    float4 t = tx.Sample( smp, i.uv );
    clip( t.a - 0.5 );
    float3 rgb = ShadeDecal( SrgbToLinear( t.rgb ), i.wpos, i.wnrm, i.clip.xyz );
    // Distance fog, like every other opaque surface (D3D11 draws this pass with PS_World_NoMV, which fogs).
    float f = saturate( ( i.fogDist - FogNear ) / max( 1.0, FogFar - FogNear ) );
    return float4( lerp( rgb, SrgbToLinear( FogColor ), f ), 1.0 );
}

float4 PSMainBlend( VS_OUT i ) : SV_TARGET // transparent — the PSO blend state picks add/alpha/modulate
{
    float4 t = tx.Sample( smp, i.uv );
    float3 albedo = SrgbToLinear( t.rgb );
    // Only the true alpha-blend modes (BLEND / BLEND_TEST) are shaded. That is the case the "blazing white
    // cobweb" fix above was actually about; ADD and MUL/MUL2 must stay unlit, exactly like D3D11 draws all of
    // them with the unlit PS_Transparency:
    //   * ADD is emissive by construction (candle flames, spell glows). Shading it turns the flame in a dark
    //     cellar into a near-black quad added onto the scene, i.e. it disappears.
    //   * MUL/MUL2 multiply against the ALREADY LIT scene colour, so shading the source applies the lighting a
    //     second time and a floor stain reads as a black blob.
    // Same split Fx.hlsl makes for the quad marks, which carry the identical blend modes.
    // Deliberately NOT fogged: this one PS is shared by all the blend modes, where lerping toward the fog
    // colour would brighten (ADD) or darken (MUL) the surface instead of fading it into the distance.
    float3 rgb = ( i.lit != 0.0 ) ? ShadeDecal( albedo, i.wpos, i.wnrm, i.clip.xyz ) : albedo;
    return float4( rgb, t.a * i.alpha );
}
