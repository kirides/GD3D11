//--------------------------------------------------------------------------------------
// Compute Shader - Scalable Ambient Obscurance (SAO)
// Screen-space AO using 2D spiral sampling pattern
// Reads depth + normals, writes AO to R8_UNORM UAV
//--------------------------------------------------------------------------------------

#include "DS_Defines.h"
#include "DepthReconstruction.h"

cbuffer SAOConstantBuffer : register( b0 )
{
    float4 SAO_ProjParams;    // x = 1/P._11, y = 1/P._22, z = P._34, w = P._33
    float  SAO_Radius;        // World-space AO radius
    float  SAO_Bias;          // Normal bias to reduce self-occlusion
    float  SAO_Intensity;     // Darkening strength
    int    SAO_NumSamples;    // Spiral sample count (16-48)
    float2 SAO_InvResolution; // 1/width, 1/height
    float  SAO_BlurSharpness; // Bilateral blur edge preservation (unused here)
    float  SAO_Pad;
};

SamplerState SS_Linear : register( s0 );
Texture2D TX_Depth   : register( t0 );
Texture2D TX_Normals : register( t1 );

RWTexture2D<float> OutputAO : register( u0 );

float LinearizeDepth( float d )
{
    return LinearizeDepthReverseZInfinite( d );
}

float3 VSPositionFromDepth( float depth, float2 texcoord )
{
    return ReconstructVSPositionFromDepthReverseZInfinite( depth, texcoord, SAO_ProjParams.xy );
}

float3 DecodeNormal( float4 enc )
{
    // GBuffer normals are RG16_SNORM - already in [-1,1] range
    return DecodeNormalGBuffer( enc.xy );
}

#if SAO_RECONSTRUCT_NORMALS
// Depth-only fallback (Forward+ without the smooth-normals pre-pass): reconstruct a
// view-space normal from neighbouring depth samples, picking the closest neighbour per
// axis to keep silhouettes crisp.
float3 VSPositionAtOffset( float2 texcoord, float2 offset )
{
    float2 uv = texcoord + offset * SAO_InvResolution;
    float rawDepth = TX_Depth.SampleLevel( SS_Linear, uv, 0 ).r;
    return VSPositionFromDepth( rawDepth, uv );
}

float3 ReconstructViewNormal( float3 centerPos, float2 texcoord )
{
    float3 l = VSPositionAtOffset( texcoord, float2( -1,  0 ) );
    float3 r = VSPositionAtOffset( texcoord, float2(  1,  0 ) );
    float3 d = VSPositionAtOffset( texcoord, float2(  0, -1 ) );
    float3 u = VSPositionAtOffset( texcoord, float2(  0,  1 ) );

    float3 dpdx = ( abs( r.z - centerPos.z ) < abs( centerPos.z - l.z ) ) ? ( r - centerPos ) : ( centerPos - l );
    float3 dpdy = ( abs( u.z - centerPos.z ) < abs( centerPos.z - d.z ) ) ? ( u - centerPos ) : ( centerPos - d );

    float3 n = normalize( cross( dpdx, dpdy ) );
    if ( dot( n, -normalize( centerPos ) ) < 0.0 )
        n = -n;
    return n;
}
#endif

// Golden-angle spiral sampling
float2 GetSpiralSample( int index, int count )
{
    float r = sqrt( ( float(index) + 0.5 ) / float(count) );
    float theta = float(index) * 2.39996323; // Golden angle
    float sinT, cosT;
    sincos( theta, sinT, cosT );
    return float2( r * cosT, r * sinT );
}

// Interleaved Gradient Noise for per-pixel rotation
float InterleavedGradientNoise( float2 pos )
{
    float3 magic = float3( 0.06711056f, 0.00583715f, 52.9829189f );
    return frac( magic.z * frac( dot( pos, magic.xy ) ) );
}

[numthreads(8, 8, 1)]
void CSMain( uint3 DTid : SV_DispatchThreadID )
{
    uint2 outSize;
    OutputAO.GetDimensions( outSize.x, outSize.y );

    if ( DTid.x >= outSize.x || DTid.y >= outSize.y )
        return;

    float2 texcoord = ( float2( DTid.xy ) + 0.5 ) * SAO_InvResolution;

    // Reconstruct view-space position
    float rawDepth = TX_Depth.SampleLevel( SS_Linear, texcoord, 0 ).r;
    float3 viewPos = VSPositionFromDepth( rawDepth, texcoord );

    // Skip sky / far plane
    if ( viewPos.z > 50000.0 )
    {
        OutputAO[DTid.xy] = 1.0;
        return;
    }

    // Decode view-space normal from GBuffer, or reconstruct from depth (Forward+ fallback)
#if SAO_RECONSTRUCT_NORMALS
    float3 viewNormal = ReconstructViewNormal( viewPos, texcoord );
#else
    float4 normalSample = TX_Normals.SampleLevel( SS_Linear, texcoord, 0 );
    float3 viewNormal = DecodeNormal( normalSample );
#endif

    // Project world-space radius to screen-space pixel radius
    float projScale = float(outSize.y) / ( 2.0 * SAO_ProjParams.y );
    float screenRadius = SAO_Radius * projScale / viewPos.z;
    screenRadius = max( screenRadius, 3.0 ); // Minimum 3 pixels

    // Per-pixel random rotation angle for temporal noise
    float rotAngle = InterleavedGradientNoise( float2( DTid.xy ) ) * 6.28318530718;
    float sinR, cosR;
    sincos( rotAngle, sinR, cosR );
    float2x2 rotMatrix = float2x2( cosR, -sinR, sinR, cosR );

    float occlusion = 0.0;
    int numSamples = SAO_NumSamples;

    for ( int i = 0; i < numSamples; i++ )
    {
        // Spiral sample in screen space
        float2 spiralOffset = GetSpiralSample( i, numSamples );

        // Apply per-pixel rotation
        spiralOffset = mul( spiralOffset, rotMatrix );

        float2 sampleUV = texcoord + spiralOffset * screenRadius * SAO_InvResolution;

        // Skip out-of-bounds samples
        if ( sampleUV.x < 0.0 || sampleUV.x > 1.0 || sampleUV.y < 0.0 || sampleUV.y > 1.0 )
            continue;

        // Reconstruct sample view-space position
        float sampleRawDepth = TX_Depth.SampleLevel( SS_Linear, sampleUV, 0 ).r;
        float3 sampleViewPos = VSPositionFromDepth( sampleRawDepth, sampleUV );

        // Vector from center to sample
        float3 delta = sampleViewPos - viewPos;
        float dist2 = dot( delta, delta );
        float radiusSq = SAO_Radius * SAO_Radius;

        // Skip too-close or beyond-radius samples
        if ( dist2 < 0.0001 || dist2 > radiusSq )
            continue;

        // Alchemy AO / SAO formula:
        // dot(n, normalize(v)) with smooth distance attenuation
        float invDist = rsqrt( dist2 );
        float NdotD = max( dot( viewNormal, delta * invDist ) - SAO_Bias, 0.0 );

        // Falloff: 1 - dist²/R² (linear in squared distance, much smoother than (1-dist/R)²)
        float falloff = 1.0 - dist2 / radiusSq;

        occlusion += NdotD * falloff;
    }

    occlusion /= max( numSamples, 1 );
    float ao = saturate( 1.0 - occlusion * SAO_Intensity );

    OutputAO[DTid.xy] = ao;
}
