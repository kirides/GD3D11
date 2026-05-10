//--------------------------------------------------------------------------------------
// Compute Shader - Depth of Field Full-res Composite
// Reads full-res scene + depth, upsampled half-res bokeh blur, and focus texture
// Blends sharp and blurred based on per-pixel CoC, writes to UAV
//--------------------------------------------------------------------------------------

#include "DepthReconstruction.h"

cbuffer DepthOfFieldConstantBuffer : register( b0 )
{
    float DoF_FocusDistance;
    float DoF_FocusRange;
    float DoF_BokehRadius;
    float DoF_MaxBlur;

    float4 DoF_ProjParams;
    float DoF_NearPlane;
    float DoF_FarPlane;
    float DoF_Pad;
    float DoF_Pad2;
};

SamplerState SS_Linear : register( s0 );
Texture2D TX_Scene : register( t0 );   // Full-res sharp scene
Texture2D TX_Blur  : register( t1 );   // Half-res bokeh (rgb=blur, a=CoC)
Texture2D TX_Depth : register( t2 );   // Full-res hardware depth
Texture2D TX_Focus : register( t3 );   // 1x1 smoothed focus depth

RWTexture2D<float4> OutputComposite : register( u0 );

float LinearizeDepth( float d )
{
    return LinearizeDepthReverseZInfinite( d );
}

[numthreads(8, 8, 1)]
void CSMain( uint3 DTid : SV_DispatchThreadID )
{
    uint2 outSize;
    OutputComposite.GetDimensions( outSize.x, outSize.y );

    if ( DTid.x >= outSize.x || DTid.y >= outSize.y )
        return;

    float2 texcoord = ( float2( DTid.xy ) + 0.5 ) / float2( outSize );

    float3 sharpColor = TX_Scene.SampleLevel( SS_Linear, texcoord, 0 ).rgb;

    float focusDepth = TX_Focus.SampleLevel( SS_Linear, float2( 0.5, 0.5 ), 0 ).r;

    // Compute CoC at center and 4 neighbours, use the minimum.
    // This erodes the blur zone by 1 pixel at depth discontinuities.
    float2 depthSize;
    TX_Depth.GetDimensions( depthSize.x, depthSize.y );
    float2 dtexel = 1.0 / depthSize;

    float cocC = saturate( ( LinearizeDepth( TX_Depth.SampleLevel( SS_Linear, texcoord, 0 ).r ) - focusDepth ) / DoF_FocusRange );
    float cocL = saturate( ( LinearizeDepth( TX_Depth.SampleLevel( SS_Linear, texcoord + float2( -dtexel.x, 0 ), 0 ).r ) - focusDepth ) / DoF_FocusRange );
    float cocR = saturate( ( LinearizeDepth( TX_Depth.SampleLevel( SS_Linear, texcoord + float2(  dtexel.x, 0 ), 0 ).r ) - focusDepth ) / DoF_FocusRange );
    float cocU = saturate( ( LinearizeDepth( TX_Depth.SampleLevel( SS_Linear, texcoord + float2( 0, -dtexel.y ), 0 ).r ) - focusDepth ) / DoF_FocusRange );
    float cocD = saturate( ( LinearizeDepth( TX_Depth.SampleLevel( SS_Linear, texcoord + float2( 0,  dtexel.y ), 0 ).r ) - focusDepth ) / DoF_FocusRange );

    float minCoC = min( min( cocC, cocL ), min( cocR, min( cocU, cocD ) ) );

    // Bilinear-upsampled half-res bokeh blur
    float4 blurSample = TX_Blur.SampleLevel( SS_Linear, texcoord, 0 );

    float blendFactor = smoothstep( 0.0, 1.0, minCoC );
    float3 finalColor = lerp( sharpColor, blurSample.rgb, blendFactor );

    OutputComposite[DTid.xy] = float4( finalColor, 1.0 );
}
