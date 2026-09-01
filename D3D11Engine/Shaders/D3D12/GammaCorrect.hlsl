//--------------------------------------------------------------------------------------
// Final brightness/contrast correction (D3D12) — port of PS_PFX_GammaCorrectInv.hlsl.
//
// D3D11 applies RendererSettings.BrightnessValue/.GammaValue in its very last swapchain blit, i.e. on the
// FINISHED image: 3D scene plus Gothic's 2D UI/HUD (but not the ImGui overlay, which it draws afterwards).
// This pass sits at the same point in D3D12's frame — Present(), after the UI, before ImGui — reading a
// scratch copy of the display target (m_LdrCopy) and writing it back, since a texture cannot be its own
// SRV and RTV. The engine skips the whole pass, copy included, when both values are 1.0.
//
// EncodedMax is 1.0 in SDR, which reduces the math below to D3D11's saturate(pow(color * B, G)) exactly.
// With real HDR output the display target holds extended-sRGB values with headroom, so contrast is applied
// to the value normalized by that headroom (paper white stays put, only the range above it is shaped) and
// the saturate() doubles as the panel-ceiling clamp.
//--------------------------------------------------------------------------------------

cbuffer GammaCorrectCB : register( b0 )
{
    uint  SrcIndex;      // SRV heap slot of the display-target copy
    float Brightness;    // RendererSettings.BrightnessValue
    float Gamma;         // RendererSettings.GammaValue (the "Contrast" slider)
    float EncodedMax;    // 1.0 in SDR; the encoded display headroom with HDR output
};

struct VS_OUT { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

VS_OUT VSFullscreen( uint vid : SV_VertexID )
{
    VS_OUT o;
    o.uv  = float2( ( vid << 1 ) & 2, vid & 2 );          // (0,0)(2,0)(0,2) — one triangle covering the screen
    o.pos = float4( o.uv * float2( 2, -2 ) + float2( -1, 1 ), 0, 1 );
    return o;
}

float4 PSMain( VS_OUT i ) : SV_TARGET
{
    Texture2D<float4> src = ResourceDescriptorHeap[SrcIndex];
    float4 color = src.Load( int3( i.pos.xy, 0 ) );       // 1:1 blit, no filtering needed

    float3 c = saturate( color.rgb * Brightness / EncodedMax );
    return float4( pow( c, Gamma ) * EncodedMax, color.a );
}
