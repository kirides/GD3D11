// Debug/editor line renderer (D3D12LineRenderer). Port of D3D11's VS_Lines + VS_Lines_XYZRHW + PS_Lines:
// an unlit, alpha-blended LINELIST drawn straight onto the tonemapped swapchain backbuffer after the scene
// resolve. Vertices are the CPU-side LineVertex (float4 position + float4 RGBA color, 32 bytes) uploaded
// verbatim into the per-frame line ring — no material, no texture, no fog.
//
// Both entry points share one root signature (b0 ViewProj root constants, b1 viewport root constants);
// each draw sets both so no root parameter is ever left stale, even though a given VS reads only one.
// Default (column-major) matrix packing, same as World.hlsl/Preview.hlsl — mul(float4(pos,1), M) is
// byte-for-byte identical to the row-major XMFLOAT4X4 upload.
cbuffer ViewProjCB : register(b0) { float4x4 ViewProj; };
cbuffer ViewportCB : register(b1) { float2 ViewportPos; float2 ViewportSize; float2 _VpPad0, _VpPad1; };

struct VS_IN  { float4 pos : POSITION; float4 color : COLOR; };
struct VS_OUT { float4 clip : SV_POSITION; float4 color : TEXCOORD0; };

// World-space lines (BaseLineRenderer::AddLine). D3D11 binds an identity world matrix here, so this is
// just the view-projection. The world matrix is reversed-Z, like every other D3D12 3D pass.
VS_OUT VSMain( VS_IN i )
{
    VS_OUT o;
    o.clip = mul( float4( i.pos.xyz, 1.0 ), ViewProj );
    // Parity with D3D11's VS_Lines: position.w doubles as a per-vertex depth scale. Every world-space
    // LineVertex constructor leaves it at 1.0 (nothing in the codebase passes a different zScale), so this
    // is a no-op in practice — kept so a future caller behaves the same on both backends.
    o.clip.z *= i.pos.w;
    o.color = i.color;
    return o;
}

/** Transforms a pre-transformed xyzrhw coordinate into clip space (verbatim from D3D11's VS_Lines_XYZRHW). */
float4 TransformXYZRHW( float4 xyzrhw )
{
    // Viewport coordinates -> normalized device coordinates.
    float3 ndc;
    ndc.x = ((2 * (xyzrhw.x - ViewportPos.x)) / ViewportSize.x) - 1;
    ndc.y = 1 - ((2 * (xyzrhw.y - ViewportPos.y)) / ViewportSize.y);
    ndc.z = xyzrhw.z;

    // rhw is 1/w, so undo the perspective divide by dividing by it.
    float actualW = 1.0f / xyzrhw.w;
    return float4( ndc.xyz * actualW, actualW );
}

// Screen-space lines (BaseLineRenderer::AddLineScreenSpace — zCRndD3D's hooked DrawLine/DrawLineZ).
VS_OUT VSScreen( VS_IN i )
{
    VS_OUT o;
    o.clip = TransformXYZRHW( i.pos );
    o.color = i.color;
    return o;
}

float4 PSMain( VS_OUT i ) : SV_TARGET
{
    // Target is the linear HDR scene colour, but LineVertex carries the same gamma-space zCOLOR values D3D11
    // writes straight into its LDR backbuffer — decode like albedo so the tonemap's re-encode lands back on
    // the requested colour. Alpha is a blend weight and stays linear.
    return float4( pow( max( i.color.rgb, 0.0 ), 2.2 ), i.color.a );
}
