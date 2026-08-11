//--------------------------------------------------------------------------------------
// Underwater screen effect (D3D12) — port of D3D11GraphicsEngine::DrawUnderwaterEffects.
//
// D3D11 implements this as a call into the generic blur post-FX with a custom final-copy shader:
//     PfxRenderer->BlurTexture( HDRBackBuffer, false, 0.10f, UNDERWATER_COLOR_MOD,
//                               PShaderID::PS_PFX_UnderwaterFinal );
// which expands to three passes (D3D11PFX_Blur::RenderBlur):
//     1. quarter-res horizontal Gaussian of the frame, multiplied by the colour mod  (PS_PFX_GaussBlur)
//     2. quarter-res vertical Gaussian,                 multiplied by the colour mod (PS_PFX_GaussBlur)
//     3. full-res copy back over the frame with a time-animated UV distortion        (PS_PFX_UnderwaterFinal)
// The blue tint therefore lands TWICE — B_ColorMod is written once into the blur CB and pass 2 never
// clears it — so the effective tint is UNDERWATER_COLOR_MOD^2 = (0.25, 0.49, 1.0). That is the shipped
// D3D11 look, so it is reproduced here rather than "fixed".
//
// Divergence from D3D11: the two blur passes are compute here (they write a UAV instead of rendering a
// quarter-res viewport into a pooled RTV), which keeps the effect off the RTV heap entirely. Only the
// final composite has to be a graphics pass, because it writes the swapchain/display target — which has
// no UAV. The maths of all three passes is unchanged.
//
// Both source textures are fetched bindlessly (SM6.6 ResourceDescriptorHeap) from root constants.
//--------------------------------------------------------------------------------------

//--------------------------------------------------------------------------------------
// Pass 1+2: separable Gaussian, quarter resolution
//--------------------------------------------------------------------------------------
cbuffer UnderwaterBlurCB : register( b0 )
{
    uint   BlurSrcIndex;       // SRV heap slot of the source (pass 1: the LDR frame copy; pass 2: pass 1's output)
    uint   BlurOutIndex;       // UAV heap slot of the destination
    float2 BlurTexelStep;      // (1/w, 0) horizontal, (0, 1/h) vertical — in DESTINATION (quarter-res) texels
    float4 BlurColorMod;       // UNDERWATER_COLOR_MOD, applied by both passes (see the file header)
    float2 BlurOutRes;         // destination size in texels
    float  BlurSize;           // D3D11's `scale` argument, 0.10 for the underwater call
    float  BlurPad;
};

SamplerState smpLinearClamp : register( s0 );

// Verbatim from Shaders/GaussBlur.h (DoBlurPass) — the same 13-tap kernel and weights the D3D11 blur uses.
static const float GAUSS_PixelKernel[13] =
{
    -6, -5, -4, -3, -2, -1, 0, 1, 2, 3, 4, 5, 6
};

static const float GAUSS_BlurWeights[13] =
{
    0.0048748912161282396,
    0.0146449825619271,
    0.03602084467215462,
    0.072537073483924,
    0.11959341596728278,
    0.16143422587153644,
    0.1784124116152771,
    0.16143422587153644,
    0.11959341596728278,
    0.072537073483924,
    0.03602084467215462,
    0.0146449825619271,
    0.0048748912161282396,
};

[numthreads( 8, 8, 1 )]
void CSBlur( uint3 tid : SV_DispatchThreadID )
{
    if ( any( tid.xy >= (uint2)BlurOutRes ) ) return;

    Texture2D<float4>   src = ResourceDescriptorHeap[BlurSrcIndex];
    RWTexture2D<float4> dst = ResourceDescriptorHeap[BlurOutIndex];

    // Pass 1's source is the FULL-res frame while the step is a QUARTER-res texel: that is D3D11's downscale,
    // which renders into a quarter-res viewport with the same 0..1 UVs and a quarter-res B_PixelSize.
    const float2 uv = ( tid.xy + 0.5f ) / BlurOutRes;

    float4 c = 0;
    [unroll] for ( int i = 0; i < 13; ++i )
    {
        const float2 t = uv + GAUSS_PixelKernel[i] * BlurTexelStep * BlurSize;
        c += src.SampleLevel( smpLinearClamp, saturate( t ), 0 ) * GAUSS_BlurWeights[i];
    }

    dst[tid.xy] = c * BlurColorMod;
}

//--------------------------------------------------------------------------------------
// Pass 3: full-res composite — port of PS_PFX_UnderwaterFinal.hlsl
//--------------------------------------------------------------------------------------
cbuffer UnderwaterCompositeCB : register( b0 )
{
    uint  CompBlurIndex;         // SRV heap slot of the pass-2 (blurred, tinted) quarter-res image
    uint  CompDistortionIndex;   // SRV heap slot of distortion2.dds (falls back to the 1x1 white texture)
    float CompTime;              // RI_Time — GothicAPI::GetTimeSeconds()
    float CompPad;
};

SamplerState smpCompClamp : register( s0 );
SamplerState smpCompWrap  : register( s1 );   // the distortion UVs grow without bound with time — must wrap

struct VS_OUT { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

VS_OUT VSFullscreen( uint vid : SV_VertexID )
{
    VS_OUT o;
    o.uv  = float2( ( vid << 1 ) & 2, vid & 2 );          // (0,0)(2,0)(0,2) — one triangle covering the screen
    o.pos = float4( o.uv * float2( 2, -2 ) + float2( -1, 1 ), 0, 1 );
    return o;
}

float4 PSComposite( VS_OUT i ) : SV_TARGET
{
    Texture2D<float4> blurred    = ResourceDescriptorHeap[CompBlurIndex];
    Texture2D<float4> distortion = ResourceDescriptorHeap[CompDistortionIndex];

    float2 uv = i.uv;

    // NOTE: D3D11's PS_PFX_UnderwaterFinal reads this "depth" out of TX_Distortion, not out of TX_Depth (which
    // it binds at t3 and then never samples). Since the value lands in [0,1] and is divided by 500 below, the
    // un-distortion lerp it drives is a ~0.2% effect, i.e. effectively inert. Mirrored as-is: substituting the
    // real depth buffer here would visibly change the effect away from the shipped D3D11 look.
    const float depth = distortion.Sample( smpCompWrap, uv ).r;

    // Two distortion vectors, scrolled at different rates and scales.
    uv += ( distortion.Sample( smpCompWrap, 0.2f * i.uv + CompTime * 0.005f ).xy * 2 - 1 ) * 0.004f;
    uv += ( distortion.Sample( smpCompWrap, 0.1f * i.uv * float2( -0.7f, 0.8f ) + CompTime * 0.01f ).xy * 2 - 1 ) * 0.006f;

    uv = saturate( uv );
    uv = lerp( uv, i.uv, depth / 500.0f );

    return float4( blurred.Sample( smpCompClamp, uv ).rgb, 1.0f );
}
