//--------------------------------------------------------------------------------------
// Post-tonemap sharpening (D3D12) — port of D3D11's two SHARPEN_* modes.
//
//   PSSimple : unsharp mask, port of CS_PFX_Sharpen.hlsl (SHARPEN_SIMPLE)
//   PSCas    : AMD FidelityFX CAS, port of PS_PFX_CAS.hlsl  (SHARPEN_CAS, the default mode)
//
// Both run as a fullscreen triangle on the tonemapped LDR image: the swapchain is copied into a
// scratch texture (m_LdrCopy) and sharpened back onto the swapchain RTV, since a texture cannot be
// its own SRV and RTV. That is the same copy-then-sharpen shape D3D11 uses (RenderSimpleSharpen
// copies the backbuffer into the pfx temp buffer first; D3D11PFX_CAS::Apply does the same), minus
// D3D11's extra round-trip copy back out of its intermediate buffer.
//
// The source texture is fetched bindlessly (SM6.6 ResourceDescriptorHeap) from a root constant.
//--------------------------------------------------------------------------------------

cbuffer SharpenCB : register( b0 )
{
    // CAS constants, produced on the CPU by ffxCasSetup (same call D3D11PFX_CAS::Apply makes).
    uint4 CasConst0;
    uint4 CasConst1;
    uint  SrcIndex;             // SRV heap slot of the LDR copy
    float SharpenStrength;      // RendererSettings.SharpenFactor (simple mode only)
    float2 TextureSize;         // render resolution in pixels (simple mode only)
};

SamplerState smpLinearClamp : register( s0 );

struct VS_OUT { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

VS_OUT VSFullscreen( uint vid : SV_VertexID )
{
    VS_OUT o;
    o.uv  = float2( ( vid << 1 ) & 2, vid & 2 );          // (0,0)(2,0)(0,2) — one triangle covering the screen
    o.pos = float4( o.uv * float2( 2, -2 ) + float2( -1, 1 ), 0, 1 );
    return o;
}

//--------------------------------------------------------------------------------------
// SHARPEN_SIMPLE — unsharp mask. Verbatim math from CS_PFX_Sharpen.hlsl: subtract a 3x3 box blur
// to get the high-frequency mask, clamp it to positive, add it back scaled by the strength.
//--------------------------------------------------------------------------------------
float4 PSSimple( VS_OUT i ) : SV_TARGET
{
    Texture2D<float4> src = ResourceDescriptorHeap[SrcIndex];

    float4 source = src.SampleLevel( smpLinearClamp, i.uv, 0 );

    float3 blurred = 0.0f;
    const int N = 3;
    [unroll] for ( int y = 0; y < N; y++ )
    {
        [unroll] for ( int x = 0; x < N; x++ )
        {
            float2 offset = float2( x - N / 2, y - N / 2 ) * ( 1.0f / TextureSize );
            blurred += src.SampleLevel( smpLinearClamp, i.uv + offset, 0 ).rgb;
        }
    }
    blurred /= ( N * N );

    float3 mask = max( 0.0f, source.rgb - blurred );

    return source + float4( mask * SharpenStrength, 0.0f );
}

//--------------------------------------------------------------------------------------
// SHARPEN_CAS — AMD FidelityFX Contrast Adaptive Sharpening, shared verbatim with the D3D11 path
// (Shaders/FidelityFX is backend-neutral; only the texture fetch differs, bindless here vs. t0 there).
//--------------------------------------------------------------------------------------
#define FFX_GPU
#define FFX_HLSL
#include "../FidelityFX/ffx_core.h"

FfxFloat32x3 casLoad( FFX_PARAMETER_IN FfxInt32x2 position )
{
    Texture2D<float4> src = ResourceDescriptorHeap[SrcIndex];
    return src.Load( FfxInt32x3( position, 0 ) ).rgb;
}

void casInput( inout FfxFloat32 r, inout FfxFloat32 g, inout FfxFloat32 b )
{
    // No input transform — the image is already in the display space CAS expects (same as D3D11).
}

#include "../FidelityFX/cas/ffx_cas.h"

float4 PSCas( VS_OUT i ) : SV_TARGET
{
    uint2 pixelPos = uint2( i.pos.xy );

    FfxFloat32x3 color;
    ffxCasFilter( color[0], color[1], color[2], pixelPos, CasConst0, CasConst1, true );

    return float4( color, 1.0f );
}
