cbuffer Viewport : register(b0) { float2 V_ViewportPos; float2 V_ViewportSize; };

struct TextureStage { int colorop; int colorarg1; int colorarg2; int alphaop; int alphaarg1; int alphaarg2; int2 pad; };
cbuffer FFPipelineConstantBuffer : register(b1)
{
    float  FF_FogWeight;   float3 FF_FogColor;
    float  FF_FogNear;     float  FF_FogFar;   float FF_zNear; float FF_zFar;
    float3 FF_AmbientLighting; float FF_Time;
    float4 FF_TextureFactor;
    float  FF_AlphaRef;    uint   FF_GSwitches; float2 ggs_Pad3;
    TextureStage FF_Stages[2];
};

Texture2D    tx  : register(t0);
SamplerState smp : register(s0);

#define TA_DIFFUSE 0
#define TA_CURRENT 1
#define TA_TEXTURE 2
#define TA_TFACTOR 3
#define TOP_DISABLE    1
#define TOP_SELECTARG1 2
#define TOP_SELECTARG2 3
#define TOP_MODULATE   4
#define TOP_MODULATE2X 5
#define TOP_MODULATE4X 6
#define TOP_ADD        7
#define TOP_SUBTRACT   10
#define GSWITCH_ALPHAREF 2

struct VS_IN  { float3 pos:POSITION; float3 nrm:NORMAL; float2 t0:TEXCOORD0; float2 t1:TEXCOORD1; float4 dif:DIFFUSE; };
struct VS_OUT { float4 pos:SV_POSITION; float2 uv:TEXCOORD0; float2 uv2:TEXCOORD1; float4 dif:TEXCOORD2; };

VS_OUT VSMain( VS_IN i ) {
    VS_OUT o;
    float rhw = i.nrm.x;                 // rhw stored in Normal.x
    float2 ndc;
    ndc.x = ((2.0 * (i.pos.x - V_ViewportPos.x)) / V_ViewportSize.x) - 1.0;
    ndc.y = 1.0 - ((2.0 * (i.pos.y - V_ViewportPos.y)) / V_ViewportSize.y);
    float z = i.pos.z;
#ifdef FORCE_MAX_Z
    // Sky pass (D3D11 VS_TransformedEx_MAX_Z equivalent): the game's own pre-transformed sky verts carry
    // whatever z the fixed-function pipeline baked in, but we want the sky pinned to the far plane so it
    // never occludes real geometry drawn behind it (reversed-Z: far == 0.0) while still depth-testing
    // (GREATER_EQUAL) against whatever the world already wrote.
    z = 0.0;
#endif
    float actualW = 1.0 / rhw;
    o.pos = float4(float3(ndc, z) * actualW, actualW);
    o.uv  = i.t0;
    o.uv2 = i.t1;
    o.dif = i.dif;
    return o;
}

float4 SelectArg( int arg, float4 current, float4 diffuse, float2 uv ) {
    switch ( arg ) {
    case TA_DIFFUSE: return diffuse;
    case TA_CURRENT: return current;
    case TA_TEXTURE: return tx.Sample( smp, uv );
    case TA_TFACTOR: return FF_TextureFactor;
    }
    return float4( 0, 1, 0, 0 );
}

float4 RunStage( int op, int a1, int a2, float4 current, float4 diffuse, float2 uv ) {
    switch ( op ) {
    case TOP_DISABLE:    return 0;
    case TOP_SELECTARG1: return SelectArg( a1, current, diffuse, uv );
    case TOP_SELECTARG2: return SelectArg( a2, current, diffuse, uv );
    case TOP_MODULATE:   return SelectArg( a1, current, diffuse, uv ) * SelectArg( a2, current, diffuse, uv );
    case TOP_MODULATE2X: return SelectArg( a1, current, diffuse, uv ) * SelectArg( a2, current, diffuse, uv ) * 2;
    case TOP_MODULATE4X: return SelectArg( a1, current, diffuse, uv ) * SelectArg( a2, current, diffuse, uv ) * 4;
    case TOP_ADD:        return SelectArg( a1, current, diffuse, uv ) + SelectArg( a2, current, diffuse, uv );
    case TOP_SUBTRACT:   return SelectArg( a1, current, diffuse, uv ) - SelectArg( a2, current, diffuse, uv );
    }
    return SelectArg( a1, current, diffuse, uv );   // graceful fallback for unhandled ops
}

#ifdef LINEARIZE_OUTPUT
float3 SrgbToLinear( float3 c )   // accurate sRGB EOTF — linearize gamma-encoded FF output for the linear HDR buffer
{
    return select( c <= 0.04045, c / 12.92, pow( ( c + 0.055 ) / 1.055, 2.4 ) );
}
#endif

float4 PSMain( VS_OUT i ) : SV_TARGET {
    float4 diffuse = i.dif.bgra;         // vertex color 0xAARRGGBB read as RGBA -> swizzle back to RGBA
    float4 color = RunStage( FF_Stages[0].colorop, FF_Stages[0].colorarg1, FF_Stages[0].colorarg2, diffuse, diffuse, i.uv );
    [branch] if ( FF_Stages[1].colorop != TOP_DISABLE )
        color = RunStage( FF_Stages[1].colorop, FF_Stages[1].colorarg1, FF_Stages[1].colorarg2, color, diffuse, i.uv2 );
    [branch] if ( ( FF_GSwitches & GSWITCH_ALPHAREF ) != 0 )
        clip( color.a - FF_AlphaRef );
#ifdef LINEARIZE_OUTPUT
    // This pass (sky pass, STAGE_DRAW_SKY) writes into the linear HDR scene-color target, but the FF stages
    // above (texture samples, vertex colors, TFactor) are all sRGB-encoded, same as every other lit shader's
    // albedo. Without this, the tonemap's own LinearToSrgb re-encodes already-gamma-encoded values, blowing
    // the sky out far brighter than D3D11's SRGB-backbuffer path (which never re-encodes).
    color.rgb = SrgbToLinear( saturate( color.rgb ) );
#endif
    return color;
}
