//--------------------------------------------------------------------------------------
// Depth of Field - Circle of Confusion pass
// Computes CoC using temporally-smoothed focus depth from the 1x1 focus texture
//--------------------------------------------------------------------------------------

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
Texture2D TX_Scene : register( t0 );
Texture2D TX_Depth : register( t1 );
Texture2D TX_Focus : register( t2 ); // 1x1 R32_FLOAT - smoothed focus depth

struct PS_INPUT
{
    float2 vTexcoord : TEXCOORD0;
    float3 vEyeRay   : TEXCOORD1;
    float4 vPosition : SV_POSITION;
};

float LinearizeDepth( float d )
{
    return DoF_ProjParams.z / ( d - DoF_ProjParams.w );
}

float4 PSMain( PS_INPUT Input ) : SV_TARGET
{
    float3 color = TX_Scene.Sample( SS_Linear, Input.vTexcoord ).rgb;
    float depth = TX_Depth.Sample( SS_Linear, Input.vTexcoord ).r;
    float linearDepth = LinearizeDepth( depth );

    // Read temporally-smoothed focus distance from the 1x1 resolve texture
    float focusDepth = TX_Focus.SampleLevel( SS_Linear, float2( 0.5, 0.5 ), 0 ).r;

    // Only blur objects behind the focus plane (positive CoC).
    // Near objects (player, close geometry) stay sharp.
    float coc = saturate( ( linearDepth - focusDepth ) / DoF_FocusRange );

    // Encode CoC from [-1, 1] to [0, 1] for UNORM-safe storage
    return float4( color, coc * 0.5 + 0.5 );
}
