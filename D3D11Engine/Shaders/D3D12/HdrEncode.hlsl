// Final scanout encode for the real-HDR display path (D3D12 only).
//
// Everything upstream — the tonemap resolve, Gothic's 2D UI/HUD, SMAA, the sharpen pass and the ImGui overlay —
// composites into m_HdrDisplay, an FP16 target holding EXTENDED-sRGB values in paper-white units: the usual
// gamma-encoded [0,1] the SDR path produces, except values above 1.0 are allowed and mean "brighter than diffuse
// white". Keeping that one encoding is what lets every one of those passes stay byte-identical to its SDR
// version — none of them needed an HDR variant, and alpha blending still happens in the same perceptual space it
// does on an SDR display, so the HUD looks the way it always has.
//
// This pass is the only place that knows about ST.2084: undo the transfer function, scale into absolute nits with
// the paper-white setting, convert Rec.709 primaries to Rec.2020, and PQ-encode into the R10G10B10A2 swapchain
// that was put into DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020.
//
// EncodeMode 0 is the fallback used when the swapchain refused the HDR colour space after the FP16 display buffer
// had already been committed to: emit the buffer as plain SDR so the frame still shows correctly, just without
// the extra range. Never a black screen.

cbuffer HdrEncodeCB : register(b0)
{
    uint  SrcIndex;         // bindless heap slot of the display buffer (SM6.6 ResourceDescriptorHeap)
    float PaperWhiteNits;   // nit level that 1.0 in the display buffer maps to
    uint  EncodeMode;       // 0 = SDR passthrough, 1 = ST.2084 / Rec.2020
    float _pad;
};

SamplerState smpPoint : register(s0);

struct VS_OUT { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
VS_OUT VSFullscreen( uint vid : SV_VertexID )
{
    VS_OUT o;
    o.uv  = float2( ( vid << 1 ) & 2, vid & 2 );
    o.pos = float4( o.uv * float2( 2, -2 ) + float2( -1, 1 ), 0, 1 );
    return o;
}

// Exact inverse of Tonemap.hlsl's LinearToSrgbExtended — same curve, just not clamped at 1.0.
float3 SrgbToLinearExtended( float3 c )
{
    c = max( c, 0.0 );
    return select( c <= 0.04045, c / 12.92, pow( ( c + 0.055 ) / 1.055, 2.4 ) );
}

// BT.709 -> BT.2020 primaries (linear light).
static const float3x3 Rec709ToRec2020 = {
    { 0.627402, 0.329292, 0.043306 },
    { 0.069095, 0.919544, 0.011360 },
    { 0.016394, 0.088028, 0.895578 },
};

// SMPTE ST.2084 (PQ) inverse-EOTF. Input is linear light normalized so 1.0 == 10000 nits.
float3 LinearToPQ( float3 L )
{
    const float m1 = 0.1593017578125, m2 = 78.84375;
    const float c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    float3 p = pow( saturate( L ), m1 );
    return pow( ( c1 + c2 * p ) / ( 1.0 + c3 * p ), m2 );
}

float4 PSEncode( VS_OUT i ) : SV_TARGET
{
    Texture2D src = ResourceDescriptorHeap[SrcIndex];
    float3 c = src.SampleLevel( smpPoint, i.uv, 0 ).rgb;

    [branch] if ( EncodeMode == 0 )
        return float4( saturate( c ), 1.0 );   // colour-space setup failed — present the SDR image untouched

    float3 linearPaperWhite = SrgbToLinearExtended( c );
    float3 nits = linearPaperWhite * PaperWhiteNits;             // absolute luminance, Rec.709 primaries
    float3 rec2020 = max( 0.0, mul( Rec709ToRec2020, nits ) );   // the matrix can produce small negatives
    return float4( LinearToPQ( rec2020 / 10000.0 ), 1.0 );
}
