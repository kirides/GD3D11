//--------------------------------------------------------------------------------------
// PS_FP_ShadowMask.hlsl
// Screen-space shadow mask pre-pass for Forward+ rendering.
//
// Reads the Z-prepass depth buffer, reconstructs world-space position per pixel,
// and outputs a pre-computed CSM shadow value to an R8_UNORM texture.
// PS_Diffuse.hlsl (FORWARD_PLUS branch) samples this texture at t12 instead of
// running ComputeCascadedShadowValueSoft inline per fragment.
//
// Paired with VS_PFX (fullscreen triangle).
// Constant buffer layout matches DS_ScreenQuadConstantBuffer; bound at b0.
//--------------------------------------------------------------------------------------

#ifndef MAX_CSM_CASCADES
#define MAX_CSM_CASCADES 4
#endif

cbuffer DS_ScreenQuadConstantBuffer : register( b0 )
{
    float4 SQ_ProjParams; // x = 1/P._11, y = 1/P._22, z = P._43, w = P._33
    matrix SQ_InvView;
    matrix SQ_View;

    matrix SQ_RainViewProj;

    float3 SQ_LightDirectionVS;
    float  SQ_ShadowmapSize;

    float4 SQ_LightColor;
    matrix SQ_ShadowViewProj[MAX_CSM_CASCADES];

    float SQ_ShadowStrength;
    float SQ_ShadowAOStrength;
    float SQ_WorldAOStrength;
    float SQ_ShadowSoftness;

    uint   SQ_FrameIndex;
    float  SQ_LightSize;
    float2 SQ_Pad;

    // Shadow atlas: per-cascade UV rect (xy = offset, zw = scale)
    float4 SQ_CascadeAtlasRect[MAX_CSM_CASCADES];
};

//--------------------------------------------------------------------------------------
// Textures and Samplers
//--------------------------------------------------------------------------------------
SamplerState           SS_Linear : register( s0 );
SamplerComparisonState SS_Comp   : register( s2 );

Texture2D TX_Depth : register( t2 );

#if SHADOW_ATLAS
Texture2D      TX_ShadowmapAtlas : register( t3 );
#else
Texture2DArray TX_ShadowmapArray : register( t3 );
#endif

Texture2D TX_ShadowBlueNoise : register( t8 );

#include "ShadowSampling.h"
#include "DepthReconstruction.h"

//--------------------------------------------------------------------------------------
// Input / Output structures  (must match VS_PFX output)
//--------------------------------------------------------------------------------------
struct PS_INPUT
{
    float2 vTexCoord : TEXCOORD0;
    float3 vEyeRay   : TEXCOORD1;
    float4 vPosition : SV_POSITION;
};

//--------------------------------------------------------------------------------------
// Reconstruct view-space position from hardware depth (reversed-Z)
//--------------------------------------------------------------------------------------
float3 VSPositionFromDepth( float depth, float2 vTexCoord )
{
    return ReconstructVSPositionFromDepthReverseZInfinite( depth, vTexCoord, SQ_ProjParams.xy );
}

//--------------------------------------------------------------------------------------
// Pixel Shader
// Returns a single shadow factor in [0, 1]:  0 = fully in shadow, 1 = fully lit.
//--------------------------------------------------------------------------------------
float PSMain( PS_INPUT Input ) : SV_TARGET
{
    float2 uv = Input.vTexCoord;

    // Sample depth.  Reversed-Z: sky pixels have depth == 0 (no geometry written).
    float depth = TX_Depth.Sample( SS_Linear, uv ).r;
    if ( !(depth > 0.0f) )
        return 1.0f; // Sky — treat as fully lit

    // Reconstruct positions
    float3 vsPosition = VSPositionFromDepth( depth, uv );
    float3 wsPosition = mul( float4( vsPosition, 1.0f ), SQ_InvView ).xyz;

    float3 wsDx = ddx( wsPosition );
    float3 wsDy = ddy( wsPosition );
    float3 wsNormal = normalize( cross( wsDx, wsDy ) );
    float3 wsLightDirection = normalize( mul( float4( SQ_LightDirectionVS, 0.0f ), SQ_InvView ).xyz );

    float NoL = saturate( abs( dot( wsNormal, wsLightDirection ) ) );
    float slopeScale = sqrt( saturate( 1.0f - NoL * NoL ) );

    int cascadeIndex = GetPrimaryCascadeIndex( wsPosition );
    float texelWorldSize = GetCascadeWorldTexelSize( cascadeIndex );

    const float normalBiasMultiplier = 1.5f;

    float3 biasedWsPosition = wsPosition + wsNormal * (slopeScale * texelWorldSize * normalBiasMultiplier);

    // ComputeCascadedShadowValueSoft is defined in ShadowSampling.h.
    // Pass 1.0 for vertLighting (the shadow mask carries only the cascade shadow;
    // vertex-AO is applied separately in FP_ComputeSunLighting).
    return ComputeCascadedShadowValueSoft( biasedWsPosition, vsPosition.z, 1.0f, 0.0f, Input.vPosition.xy );
}
