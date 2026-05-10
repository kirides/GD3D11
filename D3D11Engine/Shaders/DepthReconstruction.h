#ifndef DEPTH_RECONSTRUCTION_H
#define DEPTH_RECONSTRUCTION_H

// Reversed-Z infinite far plane helper set.
// Main camera projection writes depth ~= 1 / viewZ, so linear view-space depth
// can be reconstructed via reciprocal depth.
float LinearizeDepthReverseZInfinite( float depth )
{
    return rcp( max( depth, 1e-8f ) );
}

float2 ReconstructNdcFromTexcoord( float2 texcoord )
{
    return texcoord * float2( 2.0f, -2.0f ) + float2( -1.0f, 1.0f );
}

float3 ReconstructVSPositionFromDepthReverseZInfinite( float depth, float2 texcoord, float2 invProjXY )
{
    float linearZ = LinearizeDepthReverseZInfinite( depth );
    float2 ndc = ReconstructNdcFromTexcoord( texcoord );
    return float3( ndc * invProjXY * linearZ, linearZ );
}

float3 ReconstructVSPositionFromDepthReverseZInfinite( float depth, uint2 pixelCoord, float2 viewportSize, float2 invProjXY )
{
    float2 texcoord = (float2( pixelCoord ) + 0.5f) / viewportSize;
    return ReconstructVSPositionFromDepthReverseZInfinite( depth, texcoord, invProjXY );
}

#endif
