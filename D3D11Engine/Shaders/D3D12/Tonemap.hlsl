cbuffer TonemapCB : register(b0) { float Exposure; };
Texture2D    SceneHDR : register(t0);
SamplerState smp      : register(s0);

struct VS_OUT { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
VS_OUT VSFullscreen( uint vid : SV_VertexID )
{
    VS_OUT o;
    o.uv  = float2( ( vid << 1 ) & 2, vid & 2 );          // (0,0)(2,0)(0,2) covering the screen with one triangle
    o.pos = float4( o.uv * float2( 2, -2 ) + float2( -1, 1 ), 0, 1 );
    return o;
}

// Narkowicz ACES filmic fit: compresses linear HDR into [0,1] with a filmic highlight rolloff, so bright sun +
// stacked additive point lights keep their color/detail instead of clipping to flat white.
float3 ACESFilm( float3 x )
{
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return saturate( ( x * ( a * x + b ) ) / ( x * ( c * x + d ) + e ) );
}
// The scene HDR buffer is LINEAR (albedo is sRGB-decoded in the lit passes). Re-encode to sRGB/gamma on the way to
// the UNORM swapchain (which stores plain gamma-encoded values, same space as the 2D UI that composites on top).
float3 LinearToSrgb( float3 c )
{
    return select( c <= 0.0031308, c * 12.92, 1.055 * pow( c, 1.0 / 2.4 ) - 0.055 );
}
float4 PSTonemap( VS_OUT i ) : SV_TARGET
{
    float3 hdr = SceneHDR.Sample( smp, i.uv ).rgb * Exposure;
    return float4( LinearToSrgb( ACESFilm( hdr ) ), 1.0 );
}
