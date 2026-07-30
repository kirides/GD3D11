cbuffer Viewport : register(b0) { float2 V_ViewportPos; float2 V_ViewportSize; };

Texture2D    TX_TextureY : register(t0);
Texture2D    TX_TextureU : register(t1);
Texture2D    TX_TextureV : register(t2);
SamplerState smp : register(s0);

struct VS_IN  { float3 pos:POSITION; float3 nrm:NORMAL; float2 t0:TEXCOORD0; float2 t1:TEXCOORD1; float4 dif:DIFFUSE; };
struct VS_OUT { float4 pos:SV_POSITION; float2 uv:TEXCOORD0; };

// Same pre-transformed (XYZRHW) 2D transform as UI.hlsl's VSMain — Bink frames are drawn through the
// same DrawVertexArray path (VS_TransformedEx equivalent), just with a YUV-sampling PS instead of the
// FF texture-stage emulation.
VS_OUT VSMain( VS_IN i ) {
    VS_OUT o;
    float rhw = i.nrm.x;                 // rhw stored in Normal.x
    float2 ndc;
    ndc.x = ((2.0 * (i.pos.x - V_ViewportPos.x)) / V_ViewportSize.x) - 1.0;
    ndc.y = 1.0 - ((2.0 * (i.pos.y - V_ViewportPos.y)) / V_ViewportSize.y);
    float actualW = 1.0 / rhw;
    o.pos = float4(float3(ndc, i.pos.z) * actualW, actualW);
    o.uv  = i.t0;
    return o;
}

// Mirrors D3D11's PS_Video.hlsl (BT.601 limited-range YUV -> RGB).
float4 PSMain( VS_OUT i ) : SV_TARGET {
    const float3 offset = {-0.0627451017, -0.501960814, -0.501960814};
    const float3 Rcoeff = {1.1644,  0.0000,  1.7927};
    const float3 Gcoeff = {1.1644, -0.2132, -0.5329};
    const float3 Bcoeff = {1.1644,  2.1124,  0.0000};

    float3 yuv;
    yuv.x = TX_TextureY.Sample(smp, i.uv).r;
    yuv.y = TX_TextureU.Sample(smp, i.uv).r;
    yuv.z = TX_TextureV.Sample(smp, i.uv).r;
    yuv += offset;

    float4 color;
    color.r = saturate(dot(yuv, Rcoeff));
    color.g = saturate(dot(yuv, Gcoeff));
    color.b = saturate(dot(yuv, Bcoeff));
    color.a = 1.0;
    return color;
}
