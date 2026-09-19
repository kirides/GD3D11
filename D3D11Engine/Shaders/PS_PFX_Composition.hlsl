//--------------------------------------------------------------------------------------
// PostFX Composition Uber Shader
// Merges SAO, HeightFog, and GodRays into a single full-screen pass.
// Permutation macros: COMPOSE_SAO, COMPOSE_HEIGHTFOG, COMPOSE_GODRAYS
//--------------------------------------------------------------------------------------

#if COMPOSE_HEIGHTFOG
#include "DepthReconstruction.h"
#include "HeightfogColor.h"
#endif

//--------------------------------------------------------------------------------------
// Constant Buffers
//--------------------------------------------------------------------------------------
#if COMPOSE_HEIGHTFOG
cbuffer PFXBuffer : register( b0 )
{
    float4 HF_ProjParams;
    matrix HF_InvView;
    float3 HF_CameraPosition;
    float HF_FogHeight;

    float HF_HeightFalloff;
    float HF_GlobalDensity;
    float HF_WeightZNear;
    float HF_WeightZFar;

    float3 HF_FogColorMod;
    float HF_pad2;

    float2 HF_ProjAB;
    float2 HF_JitterOffset;
};
#endif

//--------------------------------------------------------------------------------------
// Textures and Samplers
//--------------------------------------------------------------------------------------
SamplerState SS_Linear : register( s0 );

#if COMPOSE_SAO
Texture2D TX_SAO : register( t1 );
#endif

#if COMPOSE_GODRAYS
Texture2D TX_GodRays : register( t2 );
#endif

#if COMPOSE_HEIGHTFOG
Texture2D TX_Depth : register( t3 );
#endif

//--------------------------------------------------------------------------------------
// HeightFog helpers (inlined from PS_PFX_Heightfog.hlsl)
//--------------------------------------------------------------------------------------
#if COMPOSE_HEIGHTFOG
float3 VSPositionFromDepth( float depth, float2 vTexCoord )
{
    float3 pos = ReconstructVSPositionFromDepthReverseZInfinite( depth, vTexCoord - HF_JitterOffset, HF_ProjParams.xy );

    // Sky pixels (depth == 0, reversed-Z "infinite") reconstruct to an
    // extremely large (~1e8) view-space position. Right at the horizon its
    // vertical component swings between huge positive/negative values across
    // adjacent pixels, destabilizing the height-falloff term below. Clamp to
    // a bound comfortably beyond HF_WeightZFar (where fog weight w already
    // saturates to 1.0) so far/sky pixels get smooth, fully-saturated fog
    // instead of a discontinuity.
    float len = length( pos );
    float maxLen = HF_WeightZFar * 2.0f;
    if ( len > maxLen )
        pos *= maxLen / len;

    return pos;
}

HeightfogParams MakeHeightfogParams()
{
    HeightfogParams p;
    p.CameraPosition = HF_CameraPosition;
    p.FogHeight = HF_FogHeight;
    p.HeightFalloff = HF_HeightFalloff;
    p.GlobalDensity = HF_GlobalDensity;
    p.WeightZNear = HF_WeightZNear;
    p.WeightZFar = HF_WeightZFar;
    return p;
}

float4 ComputeHeightFog( float2 texcoord )
{
    HeightfogParams p = MakeHeightfogParams();

    float expDepth = TX_Depth.Sample( SS_Linear, texcoord ).r;
    float3 worldPos = mul( float4( VSPositionFromDepth( expDepth, texcoord ), 1 ), HF_InvView ).xyz;

    return float4( HeightfogColor( p, worldPos, HF_FogColorMod ), HeightfogCoverage( p, worldPos ) );
}
#endif

//--------------------------------------------------------------------------------------
// Input / Output structures
//--------------------------------------------------------------------------------------
struct PS_INPUT
{
    float2 vTexcoord  : TEXCOORD0;
    float3 vEyeRay    : TEXCOORD1;
    float4 vPosition  : SV_POSITION;
};

//--------------------------------------------------------------------------------------
// Pixel Shader
//--------------------------------------------------------------------------------------
// Premultiplied output, blended with ONE / INV_SRC_ALPHA: dst*(1-fog.a) + fog.rgb*fog.a + godrays. The
// blend does the lerp-then-add, so the scene is never read as a texture.
float4 PSMain( PS_INPUT Input ) : SV_TARGET
{
    float3 color = 0;
    float coverage = 0;

    // Composition order: HeightFog (alpha blend) -> GodRays (additive)
    // (Ambient occlusion is applied in the lighting pass, on indirect light only.)

#if COMPOSE_HEIGHTFOG
    float4 fog = ComputeHeightFog( Input.vTexcoord );
    color = fog.rgb * fog.a;
    coverage = fog.a;
#endif

#if COMPOSE_GODRAYS
    color += TX_GodRays.Sample( SS_Linear, Input.vTexcoord ).rgb;
#endif

    return float4( color, coverage );
}
