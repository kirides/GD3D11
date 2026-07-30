// Height fog + god-ray composition — D3D12 port of D3D11's PS_PFX_Composition.hlsl (the uber shader that
// merges COMPOSE_HEIGHTFOG and COMPOSE_GODRAYS into one fullscreen blit over the finished scene).
//
// Difference from D3D11, and the reason this needs no scene-color copy: D3D11 renders into the backbuffer
// while sampling a full-resolution COPY of it (TX_Backbuffer) so it can do `lerp(color, fog, fog.a)` in the
// shader. Both composition terms are pure blend ops though — fog is an alpha lerp, god rays are additive —
// so this version emits PREMULTIPLIED output
//
//     rgb = fogColor * fogAlpha + godRays,   a = fogAlpha
//
// and the PSO blends it with SrcBlend=ONE / DestBlend=INV_SRC_ALPHA, which evaluates to exactly the same
// result directly against m_SceneColor. No copy, no extra full-res HDR texture, one pass.
//
// Depth and the god-ray texture are fetched bindlessly (SM6.6 ResourceDescriptorHeap) via b2 root constants;
// the two real constant buffers (b0 height fog, b1 atmosphere) are root CBVs.

#include "include/AtmosphericScattering.hlsl"    // declares cbuffer Atmosphere : register(b1)

//--------------------------------------------------------------------------------------
// Constant buffers
//--------------------------------------------------------------------------------------
// Mirrors ConstantBufferStructs.h's HeightfogConstantBuffer 1:1 (the same struct D3D11 uploads) — the CPU
// side fills it with a verbatim copy of D3D11PfxRenderer::RenderPostFXComposition's setup code.
cbuffer PFXBuffer : register( b0 )
{
    float4 HF_ProjParams;      // x = 1/P._11, y = 1/P._22, z = P._43, w = P._33
    matrix HF_InvView;
    float3 HF_CameraPosition;
    float  HF_FogHeight;

    float  HF_HeightFalloff;
    float  HF_GlobalDensity;
    float  HF_WeightZNear;
    float  HF_WeightZFar;

    float3 HF_FogColorMod;
    float  HF_pad2;

    float2 HF_ProjAB;
    float2 HF_JitterOffset;
};

cbuffer FogCompositeCB : register( b2 )
{
    uint HFC_DepthIndex;      // full-res depth SRV (reversed-Z), heap index
    uint HFC_GodRaysIndex;    // quarter-res god-ray SRV, heap index
    uint HFC_Flags;           // bit0 = height fog enabled, bit1 = god rays enabled
    uint HFC_Pad;
};

#define FOGFLAG_HEIGHTFOG 1u
#define FOGFLAG_GODRAYS   2u

SamplerState SS_LinearClamp : register( s0 );   // god-ray upscale (quarter -> full res)
SamplerState SS_PointClamp  : register( s1 );   // depth (1:1, never filtered across silhouettes)

//--------------------------------------------------------------------------------------
// Height fog (inlined from PS_PFX_Composition.hlsl's COMPOSE_HEIGHTFOG block)
//--------------------------------------------------------------------------------------
// Reversed-Z with an infinite far plane: GothicAPI::GetProjectionMatrix pins _33=0 / _34=NearClip(1.0), so
// depth == 1 / viewZ and the reconstruction is a plain reciprocal — same as D3D11's
// ReconstructVSPositionFromDepthReverseZInfinite (Shaders/DepthReconstruction.h).
float3 VSPositionFromDepth( float depth, float2 vTexCoord )
{
    float linearZ = rcp( max( depth, 1e-8f ) );
    float2 uv = vTexCoord - HF_JitterOffset;
    float2 ndc = uv * float2( 2.0f, -2.0f ) + float2( -1.0f, 1.0f );
    float3 pos = float3( ndc * HF_ProjParams.xy * linearZ, linearZ );

    // Sky pixels (depth == 0, reversed-Z "infinite") reconstruct to an extremely large (~1e8) view-space
    // position. Right at the horizon its vertical component swings between huge positive/negative values
    // across adjacent pixels, destabilizing the height-falloff term below. Clamp to a bound comfortably
    // beyond HF_WeightZFar (where fog weight w already saturates to 1.0) so far/sky pixels get smooth,
    // fully-saturated fog instead of a discontinuity.
    float len = length( pos );
    float maxLen = HF_WeightZFar * 2.0f;
    if ( len > maxLen )
        pos *= maxLen / len;

    return pos;
}

float ComputeVolumetricFog( float3 cameraToWorldPos, float3 posOriginal )
{
    float cVolFogHeightDensityAtViewer = exp( -HF_HeightFalloff );

    float lenOrig = length( posOriginal - HF_CameraPosition );
    float len = length( cameraToWorldPos );
    float fogInt = len * cVolFogHeightDensityAtViewer;
    const float cSlopeThreshold = 0.01;

    float w = saturate( ( lenOrig - HF_WeightZNear ) / ( HF_WeightZFar - HF_WeightZNear ) );

    if ( abs( cameraToWorldPos.y ) > cSlopeThreshold )
    {
        float t = HF_HeightFalloff * cameraToWorldPos.y * w;
        fogInt *= ( abs( t ) > 0.0001 ? ( ( 1.0 - exp( -t ) ) / t ) : 1.0 );
    }

    return exp( -HF_GlobalDensity * w * fogInt );
}

float4 ComputeHeightFog( float2 texcoord )
{
    Texture2D depthTex = ResourceDescriptorHeap[HFC_DepthIndex];

    float expDepth = depthTex.SampleLevel( SS_PointClamp, texcoord, 0 ).r;
    float3 position = VSPositionFromDepth( expDepth, texcoord );
    position = mul( float4( position, 1 ), HF_InvView ).xyz;
    float3 posOriginal = position;
    position -= HF_CameraPosition;
    position.y -= HF_FogHeight;

    float fog = 1.0f - ComputeVolumetricFog( position, posOriginal );
    float3 color = ApplyAtmosphericScatteringGround( position, HF_FogColorMod, true );

    // Darken / lighten the fog based on the day/night cycle (values copied from the D3D11 composition shader).
    float3 nightFogColor = float3( 0.12f, 0.18f, 0.27f );
    float nightTimeBlend = saturate( -AC_LightPos.y * 4.0f );
    color = lerp( color, nightFogColor, nightTimeBlend );

    float darknessFactor = 2.5f;
    if ( AC_LightPos.y > 0.0f ) {
        darknessFactor -= ( AC_LightPos.y * 0.8f );
    }

    // Never let the fog become a 100% solid wall of color.
    float maxFogOpacity = 0.85f;

    return float4( saturate( color / darknessFactor ), saturate( fog ) * maxFogOpacity );
}

//--------------------------------------------------------------------------------------
// Fullscreen triangle (same trick as Tonemap.hlsl / Bloom_Composite.hlsl)
//--------------------------------------------------------------------------------------
struct VS_OUT { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

VS_OUT VSFullscreen( uint vid : SV_VertexID )
{
    VS_OUT o;
    o.uv = float2( ( vid << 1 ) & 2, vid & 2 );
    o.pos = float4( o.uv * float2( 2, -2 ) + float2( -1, 1 ), 0, 1 );
    return o;
}

float4 PSComposite( VS_OUT i ) : SV_TARGET
{
    float3 rgb = 0.0f;
    float  alpha = 0.0f;

    // Composition order matches D3D11: HeightFog (alpha blend) -> GodRays (additive). Both flags are uniform
    // across the draw, so these branches cost nothing.
    if ( HFC_Flags & FOGFLAG_HEIGHTFOG )
    {
        float4 fog = ComputeHeightFog( i.uv );
        rgb = fog.rgb * fog.a;   // premultiplied — see the file header
        alpha = fog.a;
    }

    if ( HFC_Flags & FOGFLAG_GODRAYS )
    {
        Texture2D godRayTex = ResourceDescriptorHeap[HFC_GodRaysIndex];
        rgb += godRayTex.SampleLevel( SS_LinearClamp, i.uv, 0 ).rgb;
    }

    return float4( rgb, alpha );
}
