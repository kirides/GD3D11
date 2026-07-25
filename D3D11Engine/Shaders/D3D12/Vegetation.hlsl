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
    float G_Time;             float G_WindStrength;    float G_HeroAffectStrength; float _gpad0;
    float3 G_PlayerPosWS;     float _gpad1;
};
cbuffer FogCB : register(b2) { float3 FogColor; float FogNear; float3 CamPosWS; float FogFar; };
cbuffer LightCB : register(b3) { uint LightCount; uint NumTilesX; uint LimitLightIntensity; uint _lpad; };

#include "include/ForwardPlusTypes.hlsl"

// Forward+ tiled point lights (root-descriptor SRVs + per-tile grid) — see World.hlsl for the rationale.
StructuredBuffer<GPULight>  Lights        : register(t2);
StructuredBuffer<LightGrid> LightGridBuf  : register(t3);
StructuredBuffer<uint>      LightIndexBuf : register(t4);

Texture2D    tx       : register(t0);   // grass blade texture (GVegetationBox::VegetationTexture)
Texture2D    txGround : register(t1);   // ground/undercoat texture (GVegetationBox::MeshTexture) — tints the blades
SamplerState smp       : register(s0);

cbuffer ShadowCB : register(b4)
{
    float4x4 CascadeViewProj[NUM_CSM_CASCADES];
    float3   SunDirWS;          float ShadowMapSize;
    float3   SunColor;          float SunIntensity;
    float3   CascadeTexelWorld; float AmbientStrength;
    float    ShadowAOStrength;  float WorldAOStrength;  float2 _shpad;
};
Texture2DArray          ShadowMap : register(t5);
SamplerComparisonState  shadowCmp : register(s2);
// PBRLighting.hlsl's AccumTiledPointLights hard-requires this symbol to exist (it samples it for any nearby
// light that has a shadow cube) even though grass otherwise gets no dedicated point-shadow support here.
TextureCubeArray        PointShadowCubes : register(t6);

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
struct VS_OUT { float4 clip : SV_POSITION; float2 uv : TEXCOORD0; float3 wpos : TEXCOORD1; float fogDist : TEXCOORD2; };

VS_OUT VSMain( VS_IN i )
{
    VS_OUT o;
    float3 wpos = mul( float4( i.pos, 1.0 ), i.iworld ).xyz;

    float wind = sin( i.pos.z * 0.001f ) * 0.5f + 0.5f;
    wind += sin( i.pos.x * 0.001f ) * 0.5f + 0.5f;
    wind += 0.2f;

    wpos.xz += sin( G_Time + wind ) * 2.0f * i.pos.y * G_WindStrength;
    wpos.xz += sin( G_Time * 3.0f + wind ) * 1.55f * i.pos.y * G_WindStrength;
    wpos.xz += sin( G_Time * 5.0f + wind ) * 1.2f * i.pos.y * G_WindStrength;

    if ( G_HeroAffectStrength > 0 )
        wpos.xz += GrassHeroAffectOffsetXZ( wpos, i.pos.y );

    o.clip = mul( float4( wpos, 1.0 ), ViewProj );
    o.uv = i.uv;
    o.wpos = wpos;
    o.fogDist = length( wpos - CamPosWS );
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

    const float3 N = float3( 0, 1, 0 );   // fixed up-ish normal; grass blades carry no real per-pixel normal
    float3 albedo = SrgbToLinear( color.rgb );
    albedo = DelightDiffuse( albedo );
    float shadow = ComputeSunShadow( i.wpos, N );
    // orm: AO=1 (g_full), roughness=0.9 (matte), metallic=0 — grass has no ORM map, so a diffuse-leaning default.
    // No screen-space AO: RenderSSAO() computes the mask from the depth PREPASS, which grass never joins (it
    // draws last, in the lit color pass) — at grass's screen pixels the mask reflects whatever's BEHIND the
    // blade (terrain or sky), not the blade itself, so it was reading as spurious heavy occlusion for anything
    // but close-up grass. Revert to unoccluded until grass gets a real depth-prepass pass.
    float3 rgb = ComputeSunLightingPBR( i.wpos, N, albedo, 1.0, shadow, 0.9, 0.0, 1.0, 1.0 );
    rgb += AccumTiledPointLights( i.clip.xy, i.wpos, N, albedo, 0.9, 0.0 );
    float f = saturate( ( i.fogDist - FogNear ) / max( 1.0, FogFar - FogNear ) );
    return float4( lerp( rgb, SrgbToLinear( FogColor ), f ), 1.0 );
}

// --- Depth-only shadow-caster variant (D3D12GraphicsEngine::CreateGrassShadowCaster/RenderSunShadows) ---
// Same wind sway as VSMain (see the header comment there) so the shadow silhouette doesn't lag the swaying
// blades — mirrors D3D11's VS_GrassInstancedShadow.hlsl. Reuses VS_IN/Grass.RootSig (b0 = the cascade's
// view-proj instead of the camera's, b1 = the same GrassCB).
struct VS_DEPTH_OUT { float4 clip : SV_POSITION; float2 uv : TEXCOORD0; };

VS_DEPTH_OUT VSDepth( VS_IN i )
{
    VS_DEPTH_OUT o;
    float3 wpos = mul( float4( i.pos, 1.0 ), i.iworld ).xyz;

    float wind = sin( i.pos.z * 0.001f ) * 0.5f + 0.5f;
    wind += sin( i.pos.x * 0.001f ) * 0.5f + 0.5f;
    wind += 0.2f;

    wpos.xz += sin( G_Time + wind ) * 2.0f * i.pos.y * G_WindStrength;
    wpos.xz += sin( G_Time * 3.0f + wind ) * 1.55f * i.pos.y * G_WindStrength;
    wpos.xz += sin( G_Time * 5.0f + wind ) * 1.2f * i.pos.y * G_WindStrength;

    if ( G_HeroAffectStrength > 0 )
        wpos.xz += GrassHeroAffectOffsetXZ( wpos, i.pos.y );

    o.clip = mul( float4( wpos, 1.0 ), ViewProj );
    o.uv = i.uv;
    return o;
}

void PSShadowClip( VS_DEPTH_OUT i )
{
    // Matches PSMain's cutout threshold (0.7 blade-alpha scale before the 0.5 clip).
    clip( tx.Sample( smp, i.uv ).a * 0.7f - 0.5f );
}
