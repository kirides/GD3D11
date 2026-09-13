//--------------------------------------------------------------------------------------
// Native 2D UI (D3D12) — UIRenderer2D batches. Same math as Shaders/VS_UI2D.hlsl + PS_UI2D.hlsl,
// but the texture comes from the SRV heap per vertex, so one draw spans every UI texture.
//--------------------------------------------------------------------------------------

cbuffer UI2DConstants : register( b0 )
{
    float2 UI_PixelToNdc;   // 2 / render target size
    float  UI_Scale;        // GothicUIScale
    float  UI_Pad;
};

SamplerState smpPointClamp  : register( s0 );
SamplerState smpLinearClamp : register( s1 );
SamplerState smpPointWrap   : register( s2 );
SamplerState smpLinearWrap  : register( s3 );

// Matches UIVertexParams in UIRenderer2D.h.
#define PARAM_TEXTURE_INDEX_MASK 0x000FFFFFu
#define PARAM_LINEAR             (1u << 20)
#define PARAM_WRAP               (1u << 21)
#define PARAM_MODE_SHIFT         22
#define MODE_ADD                 1
#define MODE_OPAQUE              2
#define MODE_TEXONLY             3
#define PARAM_IGNORE_TEX_ALPHA   (1u << 24)
#define PARAM_UNTEXTURED         (1u << 25)

struct VS_IN  { float2 pos : POSITION; float2 uv : TEXCOORD0; float4 dif : DIFFUSE; uint params : PARAMS; };
struct VS_OUT { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; float4 dif : TEXCOORD1; nointerpolation uint params : TEXCOORD2; };

VS_OUT VSMain( VS_IN i )
{
    VS_OUT o;
    float2 ndc = i.pos * UI_Scale * UI_PixelToNdc;
    o.pos = float4( ndc.x - 1.0, 1.0 - ndc.y, 0.0, 1.0 );
    o.uv = i.uv;
    o.dif = i.dif.bgra;   // Gothic BGRA DWORD read as RGBA
    o.params = i.params;
    return o;
}

#ifdef LINEARIZE_OUTPUT
float3 SrgbToLinear( float3 c )
{
    return select( c <= 0.04045, c / 12.92, pow( ( c + 0.055 ) / 1.055, 2.4 ) );
}
#endif

float4 SampleUI( Texture2D tex, float2 uv, float2 dx, float2 dy, uint params )
{
    uint s = ( ( params & PARAM_LINEAR ) != 0 ? 1u : 0u ) + ( ( params & PARAM_WRAP ) != 0 ? 2u : 0u );
    if ( s == 0 ) return tex.SampleGrad( smpPointClamp, uv, dx, dy );
    if ( s == 1 ) return tex.SampleGrad( smpLinearClamp, uv, dx, dy );
    if ( s == 2 ) return tex.SampleGrad( smpPointWrap, uv, dx, dy );
    return tex.SampleGrad( smpLinearWrap, uv, dx, dy );
}

float4 PSMain( VS_OUT i ) : SV_TARGET
{
    float2 dx = ddx( i.uv );
    float2 dy = ddy( i.uv );

    float4 texel = 1.0;
    if ( ( i.params & PARAM_UNTEXTURED ) == 0 ) {
        Texture2D tex = ResourceDescriptorHeap[NonUniformResourceIndex( i.params & PARAM_TEXTURE_INDEX_MASK )];
        texel = SampleUI( tex, i.uv, dx, dy, i.params );
    }

    uint mode = ( i.params >> PARAM_MODE_SHIFT ) & 3;
    float3 rgb = mode == MODE_TEXONLY ? texel.rgb : texel.rgb * i.dif.rgb;
#ifdef LINEARIZE_OUTPUT
    rgb = SrgbToLinear( saturate( rgb ) );
#endif
    if ( mode == MODE_TEXONLY || mode == MODE_OPAQUE )
        return float4( rgb, 1.0 );

    float a = ( i.params & PARAM_IGNORE_TEX_ALPHA ) != 0 ? i.dif.a : texel.a * i.dif.a;
    return float4( rgb * a, mode == MODE_ADD ? 0.0 : a );
}
