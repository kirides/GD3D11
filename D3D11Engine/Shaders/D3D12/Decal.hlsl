cbuffer FrameCB : register(b0) { float4x4 ViewProj; };   // default column-major packing (see world shader)

Texture2D    tx  : register(t0);
SamplerState smp : register(s0);

struct VS_IN
{
    float3   pos    : POSITION;
    float2   uv     : TEXCOORD0;
    float4x4 iworld : INSTANCE_WORLD_MATRIX;   // per-instance model matrix (world*offset*scale)
    float4   icolor : INSTANCE_COLOR;          // .a = ghost alpha, .rgb unused
};
struct VS_OUT { float4 clip : SV_POSITION; float2 uv : TEXCOORD0; float alpha : TEXCOORD1; };

VS_OUT VSMain( VS_IN i )
{
    VS_OUT o;
    float3 worldPos = mul( float4( i.pos, 1.0 ), i.iworld ).xyz;
    o.clip  = mul( float4( worldPos, 1.0 ), ViewProj );
    o.uv    = i.uv;
    o.alpha = i.icolor.a;
    return o;
}

float4 PSMainLit( VS_OUT i ) : SV_TARGET   // opaque / alpha-test cutout, fully opaque output
{
    float4 t = tx.Sample( smp, i.uv );
    clip( t.a - 0.5 );
    return float4( pow( t.rgb, 2.2 ), 1.0 );   // linearize for the linear HDR buffer
}

float4 PSMainBlend( VS_OUT i ) : SV_TARGET // transparent — the PSO blend state picks add/alpha/modulate
{
    float4 t = tx.Sample( smp, i.uv );
    return float4( pow( t.rgb, 2.2 ), t.a * i.alpha );   // linearize rgb; alpha unchanged
}
