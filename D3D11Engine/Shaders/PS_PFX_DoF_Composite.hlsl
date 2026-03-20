//--------------------------------------------------------------------------------------
// Depth of Field - Composite pass
// Blends the bokeh-blurred image with the sharp original using stored CoC
//--------------------------------------------------------------------------------------

SamplerState SS_Linear : register( s0 );
Texture2D TX_CoCScene : register( t0 );  // Sharp color (rgb) + encoded CoC (a) from Pass 1
Texture2D TX_Bokeh : register( t1 );     // Bokeh blurred result from Pass 2

struct PS_INPUT
{
    float2 vTexcoord : TEXCOORD0;
    float3 vEyeRay   : TEXCOORD1;
    float4 vPosition : SV_POSITION;
};

float4 PSMain( PS_INPUT Input ) : SV_TARGET
{
    float4 sharpSample = TX_CoCScene.Sample( SS_Linear, Input.vTexcoord );
    float3 sharpColor = sharpSample.rgb;
    // Decode CoC from [0, 1] back to [-1, 1]
    float coc = sharpSample.a * 2.0 - 1.0;
    float3 blurColor = TX_Bokeh.Sample( SS_Linear, Input.vTexcoord ).rgb;

    float absCoC = abs( coc );

    // Smooth blend from sharp to blurred based on circle-of-confusion magnitude
    float blendFactor = smoothstep( 0.0, 1.0, absCoC );

    float3 finalColor = lerp( sharpColor, blurColor, blendFactor );

    return float4( finalColor, 1.0 );
}
