#include "DepthReconstruction.h"
//--------------------------------------------------------------------------------------
// Compute Shader - SAO Bilateral Blur
// Depth-aware separable Gaussian blur for denoising SAO output
// Run twice: once horizontal (BlurDirection = 1,0), once vertical (0,1)
//--------------------------------------------------------------------------------------

cbuffer SAOBlurConstantBuffer : register( b0 )
{
    float2 SAO_Blur_InvResolution; // 1/width, 1/height
    float2 SAO_Blur_Direction;     // (1,0) for horizontal, (0,1) for vertical
    float  SAO_Blur_Sharpness;     // Depth-edge preservation strength
    float3 SAO_Blur_Pad;
    float4 SAO_Blur_ProjParams;    // x = 1/P._11, y = 1/P._22, z = P._34, w = P._33
};

SamplerState SS_Linear : register( s0 );
Texture2D TX_AO    : register( t0 ); // AO input (R8_UNORM or R16_FLOAT)
Texture2D TX_Depth : register( t1 ); // Full-res hardware depth

RWTexture2D<float> OutputAO : register( u0 );

float LinearizeDepth( float d )
{
    return LinearizeDepthReverseZInfinite( d );
}

static const int BLUR_RADIUS = 4;
static const float GAUSSIAN_WEIGHTS[5] = { 0.2270270270, 0.1945945946, 0.1216216216, 0.0540540541, 0.0162162162 };

[numthreads(8, 8, 1)]
void CSMain( uint3 DTid : SV_DispatchThreadID )
{
    uint2 outSize;
    OutputAO.GetDimensions( outSize.x, outSize.y );

    if ( DTid.x >= outSize.x || DTid.y >= outSize.y )
        return;

    float2 texcoord = ( float2( DTid.xy ) + 0.5 ) * SAO_Blur_InvResolution;

    float centerAO = TX_AO.SampleLevel( SS_Linear, texcoord, 0 ).r;
    float centerDepth = LinearizeDepth( TX_Depth.SampleLevel( SS_Linear, texcoord, 0 ).r );

    float totalAO = centerAO * GAUSSIAN_WEIGHTS[0];
    float totalWeight = GAUSSIAN_WEIGHTS[0];

    [unroll]
    for ( int i = 1; i <= BLUR_RADIUS; i++ )
    {
        float2 offset = SAO_Blur_Direction * SAO_Blur_InvResolution * float( i );

        // Positive direction
        float2 uvP = texcoord + offset;
        float aoP = TX_AO.SampleLevel( SS_Linear, uvP, 0 ).r;
        float depthP = LinearizeDepth( TX_Depth.SampleLevel( SS_Linear, uvP, 0 ).r );
        float depthDiffP = abs( depthP - centerDepth );
        float bilateralP = exp( -depthDiffP * SAO_Blur_Sharpness );
        float weightP = GAUSSIAN_WEIGHTS[i] * bilateralP;
        totalAO += aoP * weightP;
        totalWeight += weightP;

        // Negative direction
        float2 uvN = texcoord - offset;
        float aoN = TX_AO.SampleLevel( SS_Linear, uvN, 0 ).r;
        float depthN = LinearizeDepth( TX_Depth.SampleLevel( SS_Linear, uvN, 0 ).r );
        float depthDiffN = abs( depthN - centerDepth );
        float bilateralN = exp( -depthDiffN * SAO_Blur_Sharpness );
        float weightN = GAUSSIAN_WEIGHTS[i] * bilateralN;
        totalAO += aoN * weightN;
        totalWeight += weightN;
    }

    OutputAO[DTid.xy] = totalAO / max( totalWeight, 0.001 );
}
