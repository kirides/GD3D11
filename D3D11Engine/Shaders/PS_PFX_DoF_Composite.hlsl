//--------------------------------------------------------------------------------------
// Depth of Field - Full-res composite pass
// Reads depth, the upsampled half-res bokeh blur and the focus texture, and blends the blur
// over the scene by per-pixel CoC.
//
// The scene itself is NOT read: lerp(sharp, blur, coc) is exactly SRC_ALPHA/INV_SRC_ALPHA
// blending, so this draws straight onto the scene target with alpha blending enabled and the
// blend unit supplies the sharp half. That is what lets the caller skip the full-res scratch
// texture and the blit back that used to exist only to dodge the read-write hazard.
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
Texture2D TX_Blur  : register( t1 );   // Half-res bokeh (rgb=blur, a=CoC)
Texture2D TX_Depth : register( t2 );   // Full-res hardware depth
Texture2D TX_Focus : register( t3 );   // 1x1 smoothed focus depth

struct PS_INPUT
{
    float2 vTexcoord : TEXCOORD0;
    float3 vEyeRay   : TEXCOORD1;
    float4 vPosition : SV_POSITION;
};

float LinearizeDepth( float d )
{
    return LinearizeDepthReverseZInfinite( d );
}

float4 PSMain( PS_INPUT Input ) : SV_TARGET
{
    float focusDepth = TX_Focus.Load( int3( 0, 0, 0 ) ).r;

    // Minimum CoC over the centre and its 4 neighbours. This erodes the blur zone by 1 pixel at depth
    // discontinuities, preventing bilinear upsample of the half-res blur from fattening thin features
    // like leaves and fences.
    // CoC rises monotonically with 1/depth, so the minimum CoC is the CoC of the MAXIMUM raw depth -
    // one linearize and one saturate instead of five. The depth buffer may be lower-res than this pass,
    // so the taps stay normalized-UV samples off its own dimensions.
    float2 depthSize;
    TX_Depth.GetDimensions( depthSize.x, depthSize.y );
    float2 dtexel = 1.0 / depthSize;

    float d = TX_Depth.Sample( SS_Linear, Input.vTexcoord ).r;
    d = max( d, TX_Depth.Sample( SS_Linear, Input.vTexcoord + float2( -dtexel.x, 0 ) ).r );
    d = max( d, TX_Depth.Sample( SS_Linear, Input.vTexcoord + float2(  dtexel.x, 0 ) ).r );
    d = max( d, TX_Depth.Sample( SS_Linear, Input.vTexcoord + float2( 0, -dtexel.y ) ).r );
    d = max( d, TX_Depth.Sample( SS_Linear, Input.vTexcoord + float2( 0,  dtexel.y ) ).r );

    float minCoC = saturate( ( LinearizeDepth( d ) - focusDepth ) / DoF_FocusRange );

    // Fully sharp - the blend would be a no-op, so skip the blur fetch and leave the target untouched.
    if ( minCoC <= 0.0 )
        discard;

    // Bilinear-upsampled half-res bokeh blur; alpha carries the blend factor for the blend unit.
    float4 blurSample = TX_Blur.Sample( SS_Linear, Input.vTexcoord );
    return float4( blurSample.rgb, smoothstep( 0.0, 1.0, minCoC ) );
}
