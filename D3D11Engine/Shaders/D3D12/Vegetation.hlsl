// GVegetationBox grass cards (P2.12): instanced billboards scattered across a placed box, forward-lit through
// the same Cook-Torrance PBR path as World/Vob/Skeletal. Mirrors D3D11's VS_GrassInstanced.hlsl (wind sway +
// hero-influence push) and PS_Grass.hlsl (ground-tinted alpha-cutout), but resolves lighting directly here
// (Forward+, no G-buffer) instead of D3D11's deferred vDiffuse/vNrm output.
cbuffer FrameCB : register(b0) { float4x4 ViewProj; };
// Per-frame grass parameters (GVegetationBox::PopulateConstantBuffer's D3D11 fields, minus G_NormalVS — this
// path has no deferred G-buffer to feed a view-space normal into, so PSMain below just uses a fixed world-up
// normal directly instead of carrying one through the CB).
cbuffer GrassCB : register(b1)
{
    float G_Time;             float G_WindStrength;    float G_HeroAffectStrength; float G_PrevTime;
    float3 G_PlayerPosWS;     float _gpad1;
};
cbuffer FogCB : register(b2) { float3 FogColor; float FogNear; float3 CamPosWS; float FogFar; };
// ProjA/ProjB/NearZ/FarZ feed PBRLighting.hlsl's ComputeZSlice (clustered Forward+, P2.14) — see World.hlsl.
cbuffer LightCB : register(b3) {
    uint LightCount; uint NumTilesX; uint LimitLightIntensity; uint PointShadowLowIndex; uint PointShadowDynIndex;
    float ProjA; float ProjB; float NearZ; float FarZ;
};

#include "include/ForwardPlusTypes.hlsl"

// Forward+ tiled point lights (root-descriptor SRVs + per-cluster mask) — see World.hlsl for the rationale.
StructuredBuffer<GPULight>  Lights        : register(t2);
StructuredBuffer<LightGrid> LightGridBuf  : register(t3);

Texture2D    tx       : register(t0);   // grass blade texture (GVegetationBox::VegetationTexture)
Texture2D    txGround : register(t1);   // ground/undercoat texture (GVegetationBox::MeshTexture) — tints the blades
SamplerState smp       : register(s0);

cbuffer ShadowCB : register(b4)
{
    float4x4 CascadeViewProj[NUM_CSM_CASCADES];
    float3   SunDirWS;          float ShadowMapSize;
    float3   SunColor;          float SunIntensity;
    float3   CascadeTexelWorld; float AmbientStrength;
    float    ShadowAOStrength;  float WorldAOStrength;   // vertLighting -> AO modulation weights
    // How hard baked vertex light gates the sky-IBL AMBIENT term (PBRLighting.hlsl ComputeSunLightingPBR).
    // 0 = the old unoccluded behaviour, 1 = interiors get no sky ambient at all. See the note there.
    float    SkyOccStrength;    float SunSpecularEnabled;
    // Scene-wetness (rain) block. Grass applies no wetness (no Wetness.hlsl include here), but the fields must
    // be declared so the sky-IBL tail below lands at the byte offset UploadSkyIblConstants writes it to — this
    // CB is the same 512-byte resource World/Vob/Skeletal bind, three disjoint writers into one layout.
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
Texture2DArray          ShadowMap : register(t5);
SamplerComparisonState  shadowCmp : register(s2);
// PBRLighting.hlsl's AccumTiledPointLights hard-requires this symbol to exist (it samples it for any nearby
// light that has a shadow cube) even though grass otherwise gets no dedicated point-shadow support here.
TextureCubeArray        PointShadowCubes : register(t6);
// Simple-SSAO mask (bindless, set once per frame — see D3D12GraphicsEngine::RenderSSAO/m_ActiveAOMaskSrvSlot).
// b5: b0..b4 above are all spoken for on this root sig.
cbuffer AOCB : register(b5) { uint AoMaskIndex; };
// Point-clamp for the AO mask — see World.hlsl for why Sample, not Load.
SamplerState smpAoClamp : register(s1);
// SampleScreenSpaceAO — see World.hlsl; needs AOCB/smpAoClamp declared above.
#include "include/ScreenSpaceAO.hlsl"

// DelightDiffuse, ComputeSunShadow, ComputeSunLightingPBR and AccumTiledPointLights are shared with
// World.hlsl/Vob.hlsl/Skeletal.hlsl. PerturbNormal/CotangentFrame go unused since grass has no normal map.
#include "include/PBRLighting.hlsl"

// Same push-away-from-the-player falloff as D3D11's VS_GrassInstanced.hlsl.
static const float grassHeroAffectRange = 45.0f;
static const float grassHeroAffectStrength = 35.0f;

float2 GrassHeroAffectOffsetXZ( float3 wpos, float vertexY )
{
    float3 toGrass = wpos - G_PlayerPosWS;
    float distSqXZ = dot( toGrass.xz, toGrass.xz );
    float distanceFactor = exp( -distSqXZ / ( 2.0f * grassHeroAffectRange * grassHeroAffectRange ) );
    float2 pushDirXZ = distSqXZ > 0.0001f ? toGrass.xz * rsqrt( distSqXZ ) : float2( 1, 0 );
    return pushDirXZ * distanceFactor * vertexY * grassHeroAffectStrength * G_HeroAffectStrength;
}

struct VS_IN
{
    float3   pos    : POSITION;
    float2   uv     : TEXCOORD0;
    float4x4 iworld : INSTANCE_WORLD_MATRIX;   // already transposed on upload (GVegetationBox::InitSpotsRandom)
};
// `clip` is `precise`: VSMain/VSDepth/VSDepthGBuf are three separately-compiled entry points that all feed the
// same textual `mul(GrassWorldPos(...), ViewProj)` into SV_POSITION, but nothing guarantees a compiler schedules
// that expression bit-identically across them — VSDepthGBuf in particular does substantially more surrounding
// work (a second GrassWorldPos evaluation for the previous frame, two extra clip transforms), which was enough
// to shift its rounding and fail the lit pass's GREATER_EQUAL depth test on wind-swayed vertices, i.e. grass
// blades getting depth-clipped, but only when TAA/FSR3/GTAO route the prepass through VSDepthGBuf. `precise`
// pins the FP evaluation order back to what the HLSL literally says, which is Microsoft's documented fix for
// exactly this z-prepass-vs-main-pass mismatch.
struct VS_OUT { precise float4 clip : SV_POSITION; float2 uv : TEXCOORD0; float3 wpos : TEXCOORD1; float fogDist : TEXCOORD2; };

// The instanced blade's swayed world position. Shared by every grass VS — the lit pass, the depth prepass and
// the CSM caster — because a blade that lands at a different position in the prepass than in the color pass
// z-fights against its own depth (the color PSO tests GREATER_EQUAL against what the prepass wrote). The host
// side has the matching obligation: the same GrassCB (G_Time above all) must be pushed to every pass in a
// frame, which is why D3D12GraphicsEngine::MakeGrassConstants exists.
float3 GrassWorldPos( VS_IN i, float time )
{
    float3 wpos = mul( float4( i.pos, 1.0 ), i.iworld ).xyz;

    float wind = sin( i.pos.z * 0.001f ) * 0.5f + 0.5f;
    wind += sin( i.pos.x * 0.001f ) * 0.5f + 0.5f;
    wind += 0.2f;

    wpos.xz += sin( time + wind ) * 2.0f * i.pos.y * G_WindStrength;
    wpos.xz += sin( time * 3.0f + wind ) * 1.55f * i.pos.y * G_WindStrength;
    wpos.xz += sin( time * 5.0f + wind ) * 1.2f * i.pos.y * G_WindStrength;

    if ( G_HeroAffectStrength > 0 )
        wpos.xz += GrassHeroAffectOffsetXZ( wpos, i.pos.y );

    return wpos;
}

VS_OUT VSMain( VS_IN i )
{
    VS_OUT o;
    float3 wpos = GrassWorldPos( i, G_Time );

    o.clip = mul( float4( wpos, 1.0 ), ViewProj );
    o.uv = i.uv;
    o.wpos = wpos;
    o.fogDist = distance(wpos, CamPosWS);
    return o;
}

float4 PSMain( VS_OUT i ) : SV_TARGET
{
    float4 color = tx.Sample( smp, i.uv );

    // Matches D3D11 PS_Grass's desaturate-toward-luma blend (helps grass2 read less like a flat cutout sprite).
    color.rgb = lerp( saturate( dot( float3( 0.333f, 0.333f, 0.333f ), color.rgb ) * 2.0f ), color.rgb, 0.5f );
    clip( color.a * 0.7f - 0.5f );   // hard alpha-cutout — no MSAA scene color on this backend, so no alpha-to-coverage path

    // Ground tint: darken/color the blade toward the texture of the world material underneath it.
    color.rgb *= txGround.SampleLevel( smp, frac( i.wpos.xz / 1000 ), 5 ).rgb * 1.1f;

    // Geometric normal of the actual grass card, from screen-space derivatives of the (wind-swayed) world
    // position. The mesh carries none — GMeshSimple stores SimpleObjectVertexStruct { Position, TexCoord } and
    // drops assimp's normals — and D3D11 fakes a world-up one too (GVegetationBox::PopulateConstantBuffer's
    // G_NormalVS), so derivatives are the only real per-pixel orientation available without changing the vertex
    // format on both backends. Cards are flat quads, so this is exact and constant per triangle; they are drawn
    // CULL_NONE, hence the orient-toward-camera flip. Degenerate fallback for a card seen exactly edge-on.
    float3 ngRaw = cross( ddx( i.wpos ), ddy( i.wpos ) );
    float  ngLen = length( ngRaw );
    float3 Ngeo  = ngLen > 1e-6f ? ngRaw / ngLen : float3( 0, 1, 0 );
    if ( dot( Ngeo, CamPosWS - i.wpos ) < 0.0f ) Ngeo = -Ngeo;

    // Shading normal: the true card normal is near-HORIZONTAL, so lighting straight off it turns a grass field
    // into flat cardboard cutouts that go black under a high sun — which is exactly why this was a hardcoded
    // world-up. Bend most of the way back toward up: the soft rounded-field look survives, but blades now
    // respond to their own (randomly Y-rotated) orientation instead of every blade being lit identically.
    const float3 N = normalize( lerp( Ngeo, float3( 0, 1, 0 ), 0.7f ) );

    float3 albedo = SrgbToLinear( color.rgb );
    albedo = DelightDiffuse( albedo );
    // Bias off the GEOMETRIC normal, not the shading one: ComputeSunShadow's normal offset is slope-scaled by
    // sqrt(1 - dot(N,SunDir)^2), and feeding it the up-bent normal under a high sun drove that to ~0 — the
    // blades then self-shadowed into a uniform black mask (no shadow shape, just "fully occluded"), stepping
    // hard at every cascade split. Ngeo is near-edge-on to the sun, so the bias gets its full magnitude here.
    float shadow = ComputeSunShadow( i.wpos, Ngeo, 1.0 );
    // orm: AO=1 (g_full), roughness=0.9 (matte), metallic=0 — grass has no ORM map, so a diffuse-leaning default.
    // Screen-space AO applies here: grass now joins the depth prepass the mask is built from (VSDepthGBuf below,
    // range-limited by DrawVegetationDepthPrepass), so this pixel's mask entry describes the blade itself and
    // its neighbours rather than the terrain behind it — the reason this used to pass a literal 1.0. Beyond the
    // prepass range limit the blade is absent from the mask and simply reads the terrain's AO, which is a much
    // milder error at that distance than it would be up close.
    float ssao = SampleScreenSpaceAO( i.clip.xy );
    float3 rgb = ComputeSunLightingPBR( i.wpos, N, albedo, 1.0, shadow, 0.9, 0.0, 1.0, ssao );
    rgb += AccumTiledPointLights( i.clip.xyz, i.wpos, N, albedo, 0.9, 0.0 );
    float f = saturate( ( i.fogDist - FogNear ) / max( 1.0, FogFar - FogNear ) );
    return float4( lerp( rgb, SrgbToLinear( FogColor ), f ), 1.0 );
}

// --- Depth-only shadow-caster variant (D3D12GraphicsEngine::CreateGrassShadowCaster/RenderSunShadows) ---
// Same wind sway as VSMain (see the header comment there) so the shadow silhouette doesn't lag the swaying
// blades — mirrors D3D11's VS_GrassInstancedShadow.hlsl. Reuses VS_IN/Grass.RootSig (b0 = the cascade's
// view-proj instead of the camera's, b1 = the same GrassCB).
struct VS_DEPTH_OUT { precise float4 clip : SV_POSITION; float2 uv : TEXCOORD0; };

VS_DEPTH_OUT VSDepth( VS_IN i )
{
    VS_DEPTH_OUT o;
    o.clip = mul( float4( GrassWorldPos( i, G_Time ), 1.0 ), ViewProj );
    o.uv = i.uv;
    return o;
}

void PSShadowClip( VS_DEPTH_OUT i )
{
    // Matches PSMain's cutout threshold (0.7 blade-alpha scale before the 0.5 clip).
    clip( tx.Sample( smp, i.uv ).a * 0.7f - 0.5f );
}

// --- Forward+ DEPTH PREPASS variants (D3D12GraphicsEngine::DrawVegetationDepthPrepass) ------------------------
// Grass joins the depth prepass so the AO mask RenderSSAO builds from that depth contains the blades instead of
// the terrain behind them (and so the light cull bounds its clusters to the grass that is actually in front).
// VSDepth/PSShadowClip above ARE the depth-only variant — this pair only adds the two G-buffer render targets
// the prepass writes when the motion/normal buffers are available (see include/MotionVectors.hlsl).
//
// b6 for MotionCB: b0..b5 are all taken on Grass.RootSig. Only the prepass PSOs reference it, so the lit grass
// PSO and the CSM grass caster (which share this root signature) leave that root parameter unbound, as D3D12
// permits for a parameter no bound shader statically references.
#define MOTIONCB_REGISTER b6
#include "include/MotionVectors.hlsl"

struct VS_DEPTH_GBUF_OUT
{
    precise float4 clip : SV_POSITION;   // see VS_OUT's `clip` for why this must be precise
    float2 uv       : TEXCOORD0;
    float3 wpos     : TEXCOORD1;
    float4 currClip : TEXCOORD2;
    float4 prevClip : TEXCOORD3;
};

VS_DEPTH_GBUF_OUT VSDepthGBuf( VS_IN i )
{
    VS_DEPTH_GBUF_OUT o;
    float3 wpos = GrassWorldPos( i, G_Time );
    o.clip = mul( float4( wpos, 1.0 ), ViewProj );
    o.uv   = i.uv;
    o.wpos = wpos;
    // Re-evaluate the sway at last frame's wind phase (G_PrevTime, carried in the same GrassCB) instead of
    // reusing this frame's swayed `wpos` — the old CAMERA-ONLY approach fed prevClip the CURRENT world
    // position, so the sway itself reported zero velocity and TAA/motion blur smeared visibly swaying grass
    // as if it were static. The hero-push term still uses the CURRENT G_PlayerPosWS for both evaluations
    // (no previous-frame player position is tracked), which under-reports motion only very close to the
    // player — a much smaller error than dropping the wind sway's motion entirely.
    o.currClip = mul( float4( wpos, 1.0 ), UnjitteredViewProj );
    o.prevClip = mul( float4( GrassWorldPos( i, G_PrevTime ), 1.0 ), PrevViewProj );
    return o;
}

GBUF_OUT PSDepthClipGBuf( VS_DEPTH_GBUF_OUT i )
{
    clip( tx.Sample( smp, i.uv ).a * 0.7f - 0.5f );   // identical cutout to PSShadowClip/PSMain

    // The SAME shading normal PSMain lights with, derived the same way (geometric normal from the swayed world
    // position's derivatives, bent 70% toward world-up) — see PSMain for why the raw card normal is unusable.
    // XeGTAO consumes this, so it must describe the surface the lit pass will shade, not the cardboard quad.
    float3 ngRaw = cross( ddx( i.wpos ), ddy( i.wpos ) );
    float  ngLen = length( ngRaw );
    float3 Ngeo  = ngLen > 1e-6f ? ngRaw / ngLen : float3( 0, 1, 0 );
    if ( dot( Ngeo, CamPosWS - i.wpos ) < 0.0f ) Ngeo = -Ngeo;

    return MakeGBufOut( i.currClip, i.prevClip, normalize( lerp( Ngeo, float3( 0, 1, 0 ), 0.7f ) ) );
}
