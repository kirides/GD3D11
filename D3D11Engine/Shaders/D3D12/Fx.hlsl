//--------------------------------------------------------------------------------------
// Gothic FX geometry (D3D12) — quad marks and poly strips.
//
// Both are CPU-built ExVertexStruct triangle lists that D3D11 draws unlit through the fixed-function
// emulation (PS_Simple / PS_Simple_FF): color = texture * vertexColor * textureFactor. No lighting,
// no shadows, no fog.
//   * quad marks (zCQuadMark)  — blood splatter, spell ground marks; per-mark world matrix.
//   * poly strips (zCPolyStrip) — weapon/spell trails and lightning flashes; world-space, identity matrix.
//
// D3D11's DrawQuadMarks binds PS_World rather than PS_Simple, i.e. its quad marks nominally go through
// the lit world shader. That is not reproduced here: Forward+ world lighting would need the tile grid,
// cascades and AO plumbed into a pass that draws a handful of blood splats. It costs very little, because
// the quad-mark vertices already carry Gothic's baked static light (zCVertFeature::lightStatic) in their
// vertex color — multiplying it in gives essentially the shipped look. D3D11's own MUL/MUL2 quad marks
// (DrawMQuadMarks) use PS_Simple for the same reason, as does every poly strip.
//--------------------------------------------------------------------------------------

cbuffer FxViewProjCB : register( b0 ) { float4x4 ViewProj; };
cbuffer FxWorldCB    : register( b1 ) { float4x4 World; };
// Per-pass behaviour flags. Each D3D11 pass binds a different pixel shader; rather than two D3D12 shader
// permutations for four lines of difference, the caller selects the behaviour it needs:
//   FX_ALPHA_TEST    — PS_World's unconditional DoAlphaTest(color.a) against FF_AlphaRef (quad marks)
//   FX_VERTEX_COLOR  — PS_Simple's `color *= Input.vDiffuse` (MUL/MUL2 quad marks, poly strips)
// They are deliberately never both set: PS_World does NOT modulate RGB by the vertex color (it writes it
// into the G-buffer alpha instead), and PS_Simple does NOT alpha-test.
#define FX_ALPHA_TEST   1
#define FX_VERTEX_COLOR 2

cbuffer FxMaterialCB : register( b2 )
{
    uint  DiffuseIndex;     // SRV heap slot of the diffuse texture (bindless)
    uint  Flags;            // FX_* bits above
    float AlphaRef;         // Gothic's live FF_AlphaRef (GraphicsState), the same value D3D11 uploads
    float _FxPad;
};

SamplerState smp : register( s0 );

struct VS_IN {
    float3 pos   : POSITION;
    float2 uv    : TEXCOORD0;
    float4 color : DIFFUSE;
};

struct VS_OUT {
    float4 clip  : SV_POSITION;
    float2 uv    : TEXCOORD0;
    float4 color : COLOR0;
};

VS_OUT VSMain( VS_IN i )
{
    VS_OUT o;
    float4 wpos = mul( float4( i.pos, 1.0f ), World );
    o.clip = mul( wpos, ViewProj );
    o.uv = i.uv;
    // The DWORD vertex color arrives as R8G8B8A8 but Gothic packs zCOLOR as BGRA — swizzle to recover RGB.
    // (D3D11's PS_Simple multiplies it unswizzled, i.e. with R/B transposed; that is a latent bug there,
    // not something to reproduce. Same call made in the world-transparency port.)
    o.color = i.color.bgra;
    return o;
}

// The scene target is linear HDR (albedo is sRGB-decoded in every lit pass), so the sampled texel has to be
// linearized on the way in — D3D11 blends these into its HDR buffer un-linearized.
float3 SrgbToLinear( float3 c )
{
    return select( c <= 0.04045, c / 12.92, pow( ( c + 0.055 ) / 1.055, 2.4 ) );
}

float4 PSMain( VS_OUT i ) : SV_TARGET
{
    Texture2D<float4> difTex = ResourceDescriptorHeap[DiffuseIndex];
    float4 t = difTex.Sample( smp, i.uv );

    // Quad marks: PS_World's "WorldMesh can always do the alphatest" — clip regardless of the material's
    // alpha func. Load-bearing: Gothic's blood splats are zMAT_ALPHA_FUNC_NONE, i.e. drawn UNBLENDED, so
    // without the clip their fully-transparent black texture background rasterizes as a solid black quad.
    if ( Flags & FX_ALPHA_TEST )
        clip( t.a - AlphaRef );

    // Poly strips / MUL quad marks: PS_Simple's `color *= Input.vDiffuse`. PS_World deliberately does NOT do
    // this — it writes the vertex color into the G-buffer alpha and leaves RGB at the texture value, so
    // modulating here would blacken a mark sitting on geometry with dark baked lightStatic.
    float4 c = float4( SrgbToLinear( t.rgb ), t.a );
    if ( Flags & FX_VERTEX_COLOR )
        c *= i.color;

    return c;
}
