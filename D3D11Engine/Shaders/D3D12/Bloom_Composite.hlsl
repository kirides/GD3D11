// Additively composites the final (mip-0) bloom pyramid result onto the HDR scene-color target. Fullscreen
// triangle (same trick as Tonemap.hlsl's VSFullscreen), drawn with additive blend so the PS output is simply
// added on top of whatever's already in the render target — no need to read the destination.
cbuffer BloomCompositeCB : register(b0) { float B_Intensity; }
Texture2D    TX_Bloom : register(t0);
SamplerState smp      : register(s0);

struct VS_OUT { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
VS_OUT VSFullscreen( uint vid : SV_VertexID )
{
    VS_OUT o;
    o.uv  = float2( ( vid << 1 ) & 2, vid & 2 );
    o.pos = float4( o.uv * float2( 2, -2 ) + float2( -1, 1 ), 0, 1 );
    return o;
}

float4 PSComposite( VS_OUT i ) : SV_TARGET
{
    float3 bloom = TX_Bloom.Sample( smp, i.uv ).rgb;
    return float4( bloom * B_Intensity, 1.0 );
}
