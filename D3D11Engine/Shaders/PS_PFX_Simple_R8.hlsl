//--------------------------------------------------------------------------------------
// Simple fullscreen pass that splatters a single-channel (R8) texture
// across all RGB channels. Used for multiply-blending R8 AO buffers.
//--------------------------------------------------------------------------------------

SamplerState SS_Linear : register( s0 );
Texture2D TX_Texture0 : register( t0 );

struct PS_INPUT
{
    float2 vTexcoord  : TEXCOORD0;
    float3 vEyeRay    : TEXCOORD1;
    float4 vPosition  : SV_POSITION;
};

float4 PSMain( PS_INPUT Input ) : SV_TARGET
{
    float r = TX_Texture0.Sample( SS_Linear, Input.vTexcoord ).r;
    return float4( r, r, r, 1.0 );
}
