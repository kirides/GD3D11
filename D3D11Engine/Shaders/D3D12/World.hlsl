// Default (column-major) matrix packing — matches D3D11's VS_ExPacked, which reads the same
// row-major XMFLOAT4X4 bytes we upload here, so mul(float4(pos,1), ViewProj) is byte-for-byte identical.
cbuffer WorldCB : register(b0) { float4x4 ViewProj; };
cbuffer FogCB   : register(b1) { float3 FogColor; float FogNear; float3 CamPosWS; float FogFar; };
cbuffer LightCB : register(b2) { uint LightCount; uint NumTilesX; uint LimitLightIntensity; uint _lpad; };   // Forward+ tiled: light count + tiles/row

#include "include/ForwardPlusTypes.hlsl"

// Bound as a ROOT descriptor SRV (no descriptor-table slot). The per-tile grid produced by the light-cull
// compute (DispatchLightCulling) narrows this loop to only the lights that touch each 16x16 screen tile.
StructuredBuffer<GPULight>  Lights        : register(t1);   // root SRV — all visible lights, indexed by the grid
StructuredBuffer<LightGrid> LightGridBuf  : register(t2);   // root SRV — per-tile {Offset,Count}
StructuredBuffer<uint>      LightIndexBuf : register(t3);   // root SRV — per-tile light-index slices

Texture2D    tx  : register(t0);
SamplerState smp : register(s0);

// CSM sun-shadow sampling (P2.9c-4a). b3 = the per-frame shadow constants (cascade view-projs + sun dir +
// darkening strength + per-cascade world texel size); t4 = the D32 cascade array (normal-Z, 1.0 == far);
// s2 = a LESS_EQUAL PCF comparison sampler. Uploaded row-major, read column-major → mul(pos, CascadeVP[c])
// matches the caster exactly. Shadow modulates the BAKED vertex lighting (darken sun-facing surfaces the
// sun can't reach); replacing baked lighting with a full computed sun term (FP_ComputeSunLighting) is later.
cbuffer ShadowCB : register(b3)
{
    float4x4 CascadeViewProj[NUM_CSM_CASCADES];
    float3   SunDirWS;          float ShadowMapSize;    // dir TOWARD sun; shadow-map resolution
    float3   SunColor;          float SunIntensity;     // sun color (sRGB) + strength (0 when sun below horizon)
    float3   CascadeTexelWorld; float AmbientStrength;  // world units/texel; SQ_ShadowStrength (ambient/sky term)
    float    ShadowAOStrength;  float WorldAOStrength;  float2 _shpad;   // vertLighting -> AO modulation weights
    // --- Scene wetness (rain) tail, uploaded separately by UploadWetnessConstants after the rain shadow
    // pass has computed this frame's rain camera. Keep in sync with Vob.hlsl/Skeletal.hlsl and the
    // WetnessCBData struct on the CPU side. RainShadowIndex/DistortionIndex are 0xFFFFFFFF when the rain
    // shadowmap / distortion2.dds isn't available, which disables the effect entirely.
    float4x4 RainViewProj;
    float    SceneWetness;      float RainFxWeight;     float RainTime;   uint RainShadowIndex;
    uint     DistortionIndex;   float RainShadowMapSize; float2 _wetpad;
    // --- Screen-space AO reprojection tail, uploaded by UploadAoReprojConstants (kAoReprojCbOffset). The AO
    // mask is computed from the PREVIOUS frame's complete depth, so it is sampled through the camera that
    // produced it — see include/ScreenSpaceAO.hlsl. Keep in sync with Vob.hlsl/Skeletal.hlsl + AoReprojCBData.
    float4x4 AoPrevViewProj;
    uint     AoPrevDepthIndex;  float AoPrevProjZX;      float AoPrevProjZY;  float AoReprojValid;
};
Texture2DArray          ShadowMap : register(t4);
SamplerComparisonState  shadowCmp : register(s2);
// Per-material bindless indices (root consts b6): SM6.6 ResourceDescriptorHeap[...] indices for this material's
// diffuse + normal + ORM maps. The world mesh is drawn via ExecuteIndirect (P2.11), which sets these four per
// draw — so the diffuse is sampled bindless too (no per-draw descriptor table). MatNormalIndex == 0xFFFFFFFF ->
// no normal map (skip perturb); MatOrmIndex is always valid (1x1 default = AO 1 / rough 0.5 / metal 0) and its
// top 2 bits pack the FxMap's channel layout for SampleOrm() to decode (see PBRLighting.hlsl).
// MatNormalStrength scales the normal-map perturb: 1.0 for a real material normalmap, or the weak
// DEFAULT_NOISE_NORMALMAP_STRENGTH (0.10) when it's raining and MatNormalIndex is instead the rain-distortion
// texture standing in for a missing normalmap (wet-ground look, mirrors D3D11GraphicsEngine::BindTextureNRFX).
cbuffer MaterialCB : register(b6) { uint MatNormalIndex; uint MatOrmIndex; uint MatDiffuseIndex; float MatNormalStrength; };
TextureCubeArray        PointShadowCubes : register(t5);   // point-light shadow cubes (P2.10d), R16 linear depth
// Simple-SSAO mask (bindless, set once per frame — see D3D12GraphicsEngine::RenderSSAO/m_ActiveAOMaskSrvSlot).
// Points at the white 1x1 texture (mask = no occlusion) when SSAO is disabled/unavailable.
cbuffer AOCB : register(b7) { uint AoMaskIndex; };
// Point-clamp for the AO mask: MUST be Sample-based (normalized UV, CLAMP addressing), not Load — Load() with
// raw pixel coords returns 0 out-of-bounds, which the 1x1 "AO disabled" fallback always is at screen res.
SamplerState smpAoClamp : register(s1);
// SampleScreenSpaceAO — reprojects a world position into the previous-frame AO mask. Needs AOCB/smpAoClamp
// and the ShadowCB reprojection tail above, hence the include lands here.
#include "include/ScreenSpaceAO.hlsl"

// Octahedral normal decode — matches Shaders/VertexPacking.h DecodeOctNormal (the packed 36-byte vertex
// stores the normal as R16G16_SNORM at offset 12; world-mesh normals are already world-space).
float3 DecodeOctNormal( float2 e )
{
    float3 n = float3( e.xy, 1.0 - abs( e.x ) - abs( e.y ) );
    float t = saturate( -n.z );
    n.xy += select(n.xy >= 0., -t, t);
    return normalize( n );
}

// DelightDiffuse, SamplePointShadow, ComputeSunShadow, the Cook-Torrance PBR helpers, PerturbNormal/
// CotangentFrame, ComputeSunLightingPBR and AccumTiledPointLights are shared with Vob.hlsl/Skeletal.hlsl.
#include "include/PBRLighting.hlsl"
// Wet-ground / scene-wetness (rain): tri-planar ripple normals, desaturate+darken, glossier roughness and
// the additive wet sheen. Needs the ShadowCB wetness tail above plus `smp`/`shadowCmp`.
#include "include/Wetness.hlsl"

struct VS_IN  { float3 pos : POSITION; float2 nrm : NORMAL; float2 uv : TEXCOORD0; float4 col : DIFFUSE; };
struct VS_OUT { float4 clip : SV_POSITION; float2 uv : TEXCOORD0; float4 col : TEXCOORD1; float fogDist : TEXCOORD2; float3 wpos : TEXCOORD3; float3 wnrm : TEXCOORD4; };

VS_OUT VSMain( VS_IN i )
{
    VS_OUT o;
    o.clip = mul( float4( i.pos, 1.0 ), ViewProj );
    o.uv  = i.uv;
    o.col = i.col;
    o.wpos = i.pos;                          // world verts are already world-space
    o.wnrm = DecodeOctNormal( i.nrm );       // already world-space
    o.fogDist = length( i.pos - CamPosWS );
    return o;
}

float4 PSMain( VS_OUT i ) : SV_TARGET
{
    Texture2D difTex = ResourceDescriptorHeap[MatDiffuseIndex];   // bindless diffuse (ExecuteIndirect, P2.11)
    float4 t = difTex.Sample( smp, i.uv );
    clip( t.a - 0.5 );                        // fixed alpha-test cutout (opaque textures have a==1 -> kept)
    float3 N = normalize( i.wnrm );
    if ( MatNormalIndex != 0xffffffff )       // bindless normal map (BC5/BC1, Z reconstructed) if this material has one
    {
        Texture2D nrmTex = ResourceDescriptorHeap[MatNormalIndex];
        N = PerturbNormal( N, i.wpos, nrmTex, i.uv, smp, MatNormalStrength );
    }
    float3 orm = SampleOrm( MatOrmIndex, i.uv );   // AO/Roughness/Metallic, decoded per the material's FxMap layout
    float3 albedo = SrgbToLinear( t.rgb );    // linearize for PBR (all HDR-buffer values are linear now)
    albedo = DelightDiffuse( albedo );
    float vertLighting = i.col.g;             // Gothic baked vertex lighting (green channel) as the AO modulator
    float shadow = ComputeSunShadow( i.wpos, N );
    // Scene wetness (rain). Deliberately AFTER the cascade lookup: D3D11 also samples the sun shadow with
    // the undeformed normal and only then runs ApplySceneWettness. Perturbs N/albedo/roughness in place.
    float3 V = normalize( CamPosWS - i.wpos );
    float wetSheen;
    float wetness = ApplySceneWetness( i.wpos, V, N, albedo, orm.g, wetSheen );
    float ssao = SampleScreenSpaceAO( i.wpos );
    float3 rgb = ComputeSunLightingPBR( i.wpos, N, albedo, vertLighting, shadow, orm.g, orm.b, orm.r, ssao );
    rgb *= lerp( 1.0, 0.8, wetness );   // D3D11 dims the SUN light color 20% where the surface is wet
    rgb += AccumTiledPointLights( i.clip.xy, i.wpos, N, albedo, orm.g, orm.b );
    // Additive wet sheen (D3D11's specWet, boosted where the sun actually reaches: specWet += specWet * shadow).
    rgb += wetSheen * ( 1.0 + shadow ) * SrgbToLinear( SunColor ) * SunIntensity;
    // Linear distance fog toward the (linearized) atmosphere color — keeps the HDR buffer consistently linear.
    float f = saturate( ( i.fogDist - FogNear ) / max( 1.0, FogFar - FogNear ) );
    rgb = lerp( rgb, SrgbToLinear( FogColor ), f );
    return float4( rgb, 1.0 );
}
