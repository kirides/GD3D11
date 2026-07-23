cbuffer WorldCB : register(b0) { float4x4 ViewProj; };   // default column-major packing (see world shader)
cbuffer FogCB   : register(b1) { float3 FogColor; float FogNear; float3 CamPosWS; float FogFar; };
cbuffer WaterCB : register(b2) { float TotalTime; float WaterAlpha; float2 _wpad; };

Texture2D    tx  : register(t0);
SamplerState smp : register(s0);

struct VS_IN  { float3 pos : POSITION; float2 uv : TEXCOORD0; float2 scroll : TEXCOORD1; float4 col : DIFFUSE; };
struct VS_OUT { float4 clip : SV_POSITION; float2 uv : TEXCOORD0; float4 col : TEXCOORD1; float fogDist : TEXCOORD2; };

VS_OUT VSMain( VS_IN i )
{
    VS_OUT o;
    o.clip = mul( float4( i.pos, 1.0 ), ViewProj );
    float2 ani = i.scroll * TotalTime;   // scroll delta (TexCoord2) * total time (ms), like VS_ExWater
    ani -= floor( ani );                 // wrap to [0,1) so the float stays precise over long sessions
    o.uv = i.uv + ani;
    o.col = i.col;
    o.fogDist = length( i.pos - CamPosWS );
    return o;
}

float4 PSMain( VS_OUT i ) : SV_TARGET
{
    float4 t = tx.Sample( smp, i.uv );
    float3 rgb = pow( t.rgb, 2.2 ) * i.col.bgr;   // linearize (HDR buffer is linear; ~pow2.2 approximates sRGB)
    float f = saturate( ( i.fogDist - FogNear ) / max( 1.0, FogFar - FogNear ) );
    rgb = lerp( rgb, pow( FogColor, 2.2 ), f );
    return float4( rgb, WaterAlpha );
}
