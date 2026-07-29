cbuffer TonemapCB : register(b0)
{
    float Exposure; float LumWhite; uint ToneMapMode; uint HdrOutput;
    // Display headroom in PAPER-WHITE units when HdrOutput != 0: (HDR max brightness / paper white). 5.0 means
    // the panel can show highlights up to 5x diffuse white. Unused (and pushed as 0) in SDR.
    float DisplayHeadroom; float3 _pad;
};
Texture2D    SceneHDR : register(t0);
SamplerState smp      : register(s0);
// Dynamic exposure (CS_LumReduce/CS_LumAdapt): [0] = this frame's temporally-adapted average scene luminance.
// Mirrors D3D11's `vColor *= HDR_MiddleGray / fLumAvg` exposure adjustment, EXCEPT the "middle gray" target is
// the classic photographic 0.18 (below), not RendererSettings.HDRMiddleGray (0.8) — that setting is tuned for
// D3D11's own compressed tonemap curves (ToneMap_jafEq4/Uncharted2/etc.), which expect a much brighter target
// than most of the operators below. Reusing 0.8 overexposed the whole scene by ~4.4x (0.8/0.18) before the user
// even touched the manual Exposure slider. Exposure stays a user-tunable multiplier layered on top
// (RendererSettings.Exposure, default 1.0). LumWhite mirrors RendererSettings.HDRLumWhite (the whiteScale/
// highlight-rolloff white point several operators below use); ToneMapMode mirrors RendererSettings.HDRToneMap
// (GothicGraphicsState.h's E_HDRToneMap enum — same 0..5 values/order as D3D11's ImGui combo) so the same
// setting picks the operator on both backends.
static const float kMiddleGray = 0.18;
StructuredBuffer<float> AdaptedLum : register(t1);

struct VS_OUT { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
VS_OUT VSFullscreen( uint vid : SV_VertexID )
{
    VS_OUT o;
    o.uv  = float2( ( vid << 1 ) & 2, vid & 2 );          // (0,0)(2,0)(0,2) covering the screen with one triangle
    o.pos = float4( o.uv * float2( 2, -2 ) + float2( -1, 1 ), 0, 1 );
    return o;
}

// The scene HDR buffer is LINEAR (albedo is sRGB-decoded in the lit passes). Re-encode to sRGB/gamma on the way to
// the UNORM swapchain (which stores plain gamma-encoded values, same space as the 2D UI that composites on top).
float3 LinearToSrgb( float3 c )
{
    return select( c <= 0.0031308, c * 12.92, 1.055 * pow( c, 1.0 / 2.4 ) - 0.055 );
}

// --- Selectable tonemap operators (mirrors D3D11 Shaders/hdr.h) ---------------------------------------------
// D3D11's versions each re-apply their own HDR_MiddleGray/fLumAvg exposure adjustment before curving; here the
// exposure divide already happened once in PSTonemap (dynamic-exposure model, see above), so every operator
// below takes the ALREADY-EXPOSED linear color and applies only its curve/white-point shaping.

// ToneMap_jafEq4: Reinhard-extended luminance compression (D3D11's default). Note D3D11 also applies its own
// pow(x,1/2.2) gamma inside this operator; here the single shared LinearToSrgb below the switch handles gamma
// for every operator uniformly instead, so this stays linear like the others.
float3 ToneMap_jafEq4( float3 x )
{
    float lumPixel = dot( x, float3( 0.333, 0.333, 0.333 ) );
    float lumCompressed = ( lumPixel * ( 1.0 + lumPixel / ( LumWhite * LumWhite ) ) ) / ( 1.0 + lumPixel );
    return max( 0.0, lumCompressed * x );
}

float3 Uncharted2Operator( float3 x )
{
    const float A = 0.22, B = 0.3, C = 0.10, D = 0.20, E = 0.01, F = 0.30;
    return ( ( x * ( A * x + C * B ) + D * E ) / ( x * ( A * x + B ) + D * F ) ) - E / F;
}
float3 ToneMap_Uncharted2( float3 x )
{
    const float exposureBias = 2.0;
    float3 curr = Uncharted2Operator( exposureBias * x );
    float3 whiteScale = 1.0 / Uncharted2Operator( LumWhite );
    return curr * whiteScale;
}

// Narkowicz ACES filmic fit: compresses linear HDR into [0,1] with a filmic highlight rolloff. This is the
// engine's long-standing D3D12 default — punchy, but boosts saturation on reds/oranges more than some players
// want for Gothic's darker tone; ToneMap_Simple or Uncharted2 read flatter/more muted.
float3 ACESFilmOperator( float3 x )
{
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return saturate( ( x * ( a * x + b ) ) / ( x * ( c * x + d ) + e ) );
}
float3 ToneMap_ACESFilm( float3 x )
{
    float3 curr = ACESFilmOperator( x );
    float3 whiteScale = ACESFilmOperator( LumWhite );
    return curr * whiteScale;
}

// Marked "broken with gray color" in D3D11's own source (Shaders/hdr.h) — ported as-is for parity/completeness.
float3 PerceptualQuantizerOperator( float3 x )
{
    const float m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    float3 p = pow( x, m1 );
    return pow( ( c1 + c2 * p ) / ( 1.0 + c3 * p ), m2 );
}
float3 ToneMap_PerceptualQuantizer( float3 x )
{
    float3 curr = PerceptualQuantizerOperator( x );
    float3 whiteScale = 1.0 / PerceptualQuantizerOperator( LumWhite );
    return curr * whiteScale;
}

// Simple Reinhard (x / (1+x)) — the flattest, most desaturated of the set; no white-point normalization in
// D3D11 either. Good match for a darker, less "filmic-punchy" look.
float3 ToneMap_Simple( float3 x )
{
    return x / ( 1.0 + x );
}

// RRT+ODT-fitted ACES (Stephen Hill's fit) — a closer approximation of the reference ACES pipeline than the
// Narkowicz curve above; generally reads a little less saturated in the highlights.
static const float3x3 ACESInputMat = { { 0.59719, 0.35458, 0.04823 }, { 0.07600, 0.90834, 0.01566 }, { 0.02840, 0.13383, 0.83777 } };
static const float3x3 ACESOutputMat = { { 1.60475, -0.53108, -0.07367 }, { -0.10208, 1.10813, -0.00605 }, { -0.00327, -0.07276, 1.07602 } };
float3 RRTAndODTFit( float3 v )
{
    float3 a = v * ( v + 0.0245786 ) - 0.000090537;
    float3 b = v * ( 0.983729 * v + 0.4329510 ) + 0.238081;
    return a / b;
}
float3 ACESFittedOperator( float3 color )
{
    color = mul( ACESInputMat, color );
    color = RRTAndODTFit( color );
    color = mul( ACESOutputMat, color );
    return saturate( color );
}
float3 ToneMap_ACESFitted( float3 x )
{
    float3 curr = ACESFittedOperator( x );
    float3 whiteScale = ACESFittedOperator( LumWhite );
    return curr * whiteScale;
}

// --- Real HDR display output ---------------------------------------------------------------------------------
// On an actual HDR scanout the operators above are exactly the wrong thing: every one of them exists to squeeze
// an unbounded linear range into the [0,1] an SDR panel can show, and they do it by crushing (and desaturating)
// everything the display could have reproduced natively. So when HdrOutput is set none of them run. The scene
// keeps its linear values, expressed in PAPER-WHITE units (1.0 == diffuse white == the paper-white slider's nit
// level), and only the part that exceeds what the panel can physically emit is rolled off.
//
// `DisplayHeadroom` (w) is that ceiling in the same units. Everything below the knee is passed through bit-exact,
// so SDR-range material has the same brightness and contrast it would have on an SDR display; above the knee an
// exponential shoulder asymptotes to w, so a 50x-paper-white specular hit lands just under the panel's peak
// instead of clipping to a flat white blob. The shoulder is C1-continuous at the knee (derivative 1), which is
// what keeps a slow-moving highlight from visibly "catching" as it crosses over.
float RollOffLuminance( float lum, float w )
{
    const float knee = 0.75 * w;                 // BT.2390's normalized knee position
    if ( lum <= knee ) return lum;               // untouched — the whole point of not using an SDR curve
    const float shoulder = max( 1e-4, w - knee );
    return knee + shoulder * ( 1.0 - exp( -( lum - knee ) / shoulder ) );
}

float3 ToneMapHDRDisplay( float3 x, float w )
{
    // Roll off on LUMINANCE, not per channel: a per-channel curve pulls bright saturated colours (torch flame,
    // magic bolts) toward white, which is the SDR compromise this path exists to avoid. Channels that still
    // overshoot the ceiling afterwards are clamped, so extremely saturated over-range pixels desaturate only
    // when they genuinely cannot be shown.
    const float lum = max( 1e-5, dot( x, float3( 0.2126, 0.7152, 0.0722 ) ) );
    x *= RollOffLuminance( lum, w ) / lum;
    return clamp( x, 0.0, w );
}

// Extended sRGB transfer function — the standard curve, just not clamped at 1.0. The HDR display buffer stores
// gamma-encoded values with headroom (>1 == brighter than paper white) rather than linear ones, so that Gothic's
// 2D UI/HUD, the ImGui overlay, SMAA and the sharpen pass all keep compositing and blending in exactly the same
// perceptual space they use in SDR — none of those shaders needed an HDR variant. HdrEncode.hlsl inverts this
// right before scanout.
float3 LinearToSrgbExtended( float3 c )
{
    c = max( c, 0.0 );
    return select( c <= 0.0031308, c * 12.92, 1.055 * pow( c, 1.0 / 2.4 ) - 0.055 );
}

float4 PSTonemap( VS_OUT i ) : SV_TARGET
{
    // clamp guards the rare frame where CS_LumAdapt hasn't run yet (partial-sum buffer alloc failure on a
    // resize) and the buffer still holds its D3D12MA DEFAULT_POOLS_NOT_ZEROED creation-time content.
    float lum = clamp( AdaptedLum[0], 0.05, 32.0 );
    float exposureFactor = Exposure * kMiddleGray / ( lum + 0.001 );
    float3 hdr = SceneHDR.Sample( smp, i.uv ).rgb * exposureFactor;

    [branch] if ( HdrOutput != 0 )
        return float4( LinearToSrgbExtended( ToneMapHDRDisplay( hdr, DisplayHeadroom ) ), 1.0 );

    float3 mapped;
    if ( ToneMapMode == 0 )      mapped = ToneMap_jafEq4( hdr );
    else if ( ToneMapMode == 1 ) mapped = ToneMap_Uncharted2( hdr );
    else if ( ToneMapMode == 2 ) mapped = ToneMap_ACESFilm( hdr );
    else if ( ToneMapMode == 3 ) mapped = ToneMap_PerceptualQuantizer( hdr );
    else if ( ToneMapMode == 4 ) mapped = ToneMap_Simple( hdr );
    else                         mapped = ToneMap_ACESFitted( hdr );

    return float4( LinearToSrgb( saturate( mapped ) ), 1.0 );
}
