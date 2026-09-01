// Default (column-major) matrix packing — matches D3D11's VS_ExPacked, which reads the same
// row-major XMFLOAT4X4 bytes we upload here, so mul(float4(pos,1), ViewProj) is byte-for-byte identical.
cbuffer WorldCB : register(b0) { float4x4 ViewProj; };
#include "include/FogCB.hlsl"
#include "include/ForwardPlusTypes.hlsl"
// LightCB (LIGHTCB_REGISTER = b2, the default) — light count + tiles/row + cluster Z-slice basis.
#include "include/LightCB.hlsl"

// Bound as a ROOT descriptor SRV (no descriptor-table slot). The per-cluster mask produced by the light-cull
// compute (DispatchLightCulling) narrows this loop to only the lights visible in this pixel's cluster.
StructuredBuffer<GPULight>  Lights        : register(t1);   // root SRV — all visible lights, indexed by the grid
StructuredBuffer<LightGrid> LightGridBuf  : register(t2);   // root SRV — per-cluster 64-bit membership mask

Texture2D    tx  : register(t0);
SamplerState smp : register(s0);

// CSM sun-shadow sampling (P2.9c-4a). b3 = the per-frame shadow constants (cascade view-projs + sun dir +
// darkening strength + per-cascade world texel size); t4 = the D32 cascade array (normal-Z, 1.0 == far);
// s2 = a LESS_EQUAL PCF comparison sampler. Uploaded row-major, read column-major → mul(pos, CascadeVP[c])
// matches the caster exactly. Shadow modulates the BAKED vertex lighting (darken sun-facing surfaces the
// sun can't reach); replacing baked lighting with a full computed sun term (FP_ComputeSunLighting) is later.
// (SHADOWCB_REGISTER defaults to b3, which is what World wants.)
#include "include/ShadowCB.hlsl"
Texture2DArray          ShadowMap : register(t4);
SamplerComparisonState  shadowCmp : register(s2);
// Per-material bindless indices (root consts b6): SM6.6 ResourceDescriptorHeap[...] indices for this material's
// diffuse + normal + ORM maps. The world mesh is drawn via ExecuteIndirect (P2.11), which sets these four per
// draw — so the diffuse is sampled bindless too (no per-draw descriptor table). MatNormalIndex == 0xFFFFFFFF ->
// no normal map (skip perturb); MatOrmIndex is always valid (1x1 default = AO 1 / rough 0.5 / metal 0) and its
// top 2 bits pack the FxMap's channel layout for SampleOrm() to decode (see PBRLighting.hlsl).
// World also gets MatNormalStrength (the world-only extra field — see include/MaterialCB.hlsl).
#define MATERIALCB_EXTRA_FIELDS float MatNormalStrength;
#include "include/MaterialCB.hlsl"
#undef MATERIALCB_EXTRA_FIELDS
TextureCubeArray        PointShadowCubes : register(t5);   // point-light shadow cubes (P2.10d), R16 linear depth
#include "include/AOCB.hlsl"
// Point-clamp for the AO mask: MUST be Sample-based (normalized UV, CLAMP addressing), not Load — Load() with
// raw pixel coords returns 0 out-of-bounds, which the 1x1 "AO disabled" fallback always is at screen res.
SamplerState smpAoClamp : register(s1);
// SampleScreenSpaceAO — a plain screen-space read of THIS frame's AO mask (RenderSSAO builds it from this
// frame's depth prepass, so no reprojection). Needs AOCB/smpAoClamp above, hence the include lands here.
#include "include/ScreenSpaceAO.hlsl"

// Octahedral normal decode — matches Shaders/VertexPacking.h DecodeOctNormal (the packed 36-byte vertex
// stores the normal as R16G16_SNORM at offset 12; world-mesh normals are already world-space).
float3 DecodeOctNormal( float2 e )
{
    float3 n = float3( e.xy, 1.0 - abs( e.x ) - abs( e.y ) );
    float t = saturate( -n.z );
    n.xy += select(n.xy >= 0., -t, t);
    return normalize( n );
}

// Packed tangent decode — matches Shaders/VertexPacking.h DecodeTangent / CPU VertexPacking::EncodeTangent
// (R10G10B10A2_UNORM at offset 16, already world-space like the normal — the world mesh has no per-instance
// transform). xyz = unit tangent, w = bitangent handedness sign; PerturbNormal reconstructs B as cross(N,T)*w.
float4 DecodeTangent( float4 packed )
{
    float3 t = packed.xyz * 2.0 - 1.0;
    float sign = packed.w > 0.5 ? 1.0 : -1.0;
    return float4( normalize( t ), sign );
}

// DelightDiffuse, SamplePointShadow, ComputeSunShadow, the Cook-Torrance PBR helpers, PerturbNormal/
// CotangentFrame, ComputeSunLightingPBR and AccumTiledPointLights are shared with Vob.hlsl/Skeletal.hlsl.
#include "include/PBRLighting.hlsl"
// Wet-ground / scene-wetness (rain): tri-planar ripple normals, desaturate+darken, glossier roughness and
// the additive wet sheen. Needs the ShadowCB wetness tail above plus `smp`/`shadowCmp`.
#include "include/Wetness.hlsl"

struct VS_IN  { float3 pos : POSITION; float2 nrm : NORMAL; float4 tan : TANGENT; float2 uv : TEXCOORD0; float4 col : DIFFUSE; };
struct VS_OUT { float4 clip : SV_POSITION; float2 uv : TEXCOORD0; float4 col : TEXCOORD1; float fogDist : TEXCOORD2; float3 wpos : TEXCOORD3; float3 wnrm : TEXCOORD4; float4 wtan : TEXCOORD5; };

VS_OUT VSMain( VS_IN i )
{
    VS_OUT o;
    o.clip = mul( float4( i.pos, 1.0 ), ViewProj );
    o.uv  = i.uv;
    o.col = i.col;
    o.wpos = i.pos;                          // world verts are already world-space
    o.wnrm = DecodeOctNormal( i.nrm );       // already world-space
    o.wtan = DecodeTangent( i.tan );         // already world-space, same basis as wnrm
    o.fogDist = distance(i.pos, CamPosWS);
    return o;
}

// Quad marks (zCQuadMark — blood splatter, spell ground marks). Same lit path as the world mesh: this VS only
// differs in its INPUT, so PSMain below is reused verbatim (which is what gets quad marks the identical
// SrgbToLinear -> DelightDiffuse albedo treatment, CSM shadows, tiled point lights, SSAO, wetness and sky IBL).
// D3D11's equivalent is DrawQuadMarks binding VS_Ex + PS_World.
//
// Two input differences from VSMain: the geometry is the CPU-side ExVertexStruct (stride 60, full-precision
// float3 normal at offset 12, not the packed 36-byte world vertex), and the vertices are in the mark's
// connected-vob LOCAL space, so a per-mark world matrix has to be applied here.
//
// That matrix rides in b4 — the wind CB slot of the shared World root signature, which this file never reads
// (only Vob.hlsl's VSMain does) and which is already VS-visible with room for 12 DWORDs. A cbuffer that an
// entry point doesn't reference emits no binding, so declaring it here cannot disturb VSMain/VSTransparent or
// the ExecuteIndirect command signatures anchored to this root sig. Only 3 of the 4 matrix rows fit, hence the
// explicit dot form below instead of a mul(): with the codebase's verbatim row-major upload read column-major
// (see Preview.hlsl), `mul(float4(pos,1), World).x` IS `dot(float4(pos,1), row0)` — translation sits at
// _14/_24/_34 because Gothic's vob matrices are column-vector form. The 4th row is (0,0,0,1) for a rigid
// transform and contributes nothing to xyz.
cbuffer QuadMarkCB : register(b4) { float4 QmWorldRow0; float4 QmWorldRow1; float4 QmWorldRow2; };

struct VS_IN_QUADMARK { float3 pos : POSITION; float3 nrm : NORMAL; float2 uv : TEXCOORD0; float4 col : DIFFUSE; };

VS_OUT VSQuadMark( VS_IN_QUADMARK i )
{
    VS_OUT o;
    const float4 lpos = float4( i.pos, 1.0 );
    const float3 wpos = float3( dot( lpos, QmWorldRow0 ), dot( lpos, QmWorldRow1 ), dot( lpos, QmWorldRow2 ) );
    // Normal: the same basis without the translation column. Quad-mark matrices are rigid (rotation +
    // translation), so no inverse-transpose is needed; PSMain normalizes again after the perturb anyway.
    const float3 wnrm = float3( dot( i.nrm, QmWorldRow0.xyz ), dot( i.nrm, QmWorldRow1.xyz ), dot( i.nrm, QmWorldRow2.xyz ) );

    o.clip = mul( float4( wpos, 1.0 ), ViewProj );
    o.uv  = i.uv;
    o.col = i.col;
    o.wpos = wpos;
    o.wnrm = normalize( wnrm );
    o.wtan = float4( 0, 0, 0, 0 );   // no packed tangent on this CPU-side vertex — PerturbNormal falls back
                                      // to the derivative frame here, same as D3D11's untangented quad marks.
    o.fogDist = distance(wpos, CamPosWS);
    return o;
}

float4 PSMain( VS_OUT i ) : SV_TARGET
{
    Texture2D difTex = ResourceDescriptorHeap[MatDiffuseIndex];   // bindless diffuse (ExecuteIndirect, P2.11)
    float4 t = difTex.Sample( smp, i.uv );
    clip( t.a - 0.5 );                        // fixed alpha-test cutout (opaque textures have a==1 -> kept)
    float3 N = normalize( i.wnrm );
    if ( MatNormalIndex != 0xffffffff )       // bindless normal map (BC5/BC1, Z reconstructed) if this material has one
    {
        Texture2D nrmTex = ResourceDescriptorHeap[MatNormalIndex];
        N = PerturbNormal( N, i.wpos, i.wtan, nrmTex, i.uv, smp, MatNormalStrength );
    }
    float3 orm = SampleOrm( MatOrmIndex, i.uv );   // AO/Roughness/Metallic, decoded per the material's FxMap layout
    float3 albedo = SrgbToLinear( t.rgb );    // linearize for PBR (all HDR-buffer values are linear now)
    albedo = DelightDiffuse( albedo );
    float vertLighting = i.col.g;             // Gothic baked vertex lighting (green channel) as the AO modulator
    float shadow = ComputeSunShadow( i.wpos, N, vertLighting );
    // Scene wetness (rain). Deliberately AFTER the cascade lookup: D3D11 also samples the sun shadow with
    // the undeformed normal and only then runs ApplySceneWettness. Perturbs N/albedo/roughness in place.
    float3 V = normalize( CamPosWS - i.wpos );
    float wetness = ApplySceneWetness( i.wpos, N, albedo, orm.g );
    float ssao = SampleScreenSpaceAO( i.clip.xy );
    float3 rgb = ComputeSunLightingPBR( i.wpos, N, albedo, vertLighting, shadow, orm.g, orm.b, orm.r, ssao );
    rgb *= mad(wetness, 0.8 - 1.0, 1.0);   // D3D11 dims the SUN light color 20% where the surface is wet
    rgb += AccumTiledPointLights( i.clip.xyz, i.wpos, N, albedo, orm.g, orm.b );
    // No fixed-direction wet sheen here — see ApplySceneWetness's header comment for why D3D12 drops that
    // D3D11 hack in favor of the real Cook-Torrance sun specular (already fed by the roughness dip above)
    // plus the opaque-surface SSR below.
    // Opaque-surface SSR (temporal, D3D12 only) — additive, physically-weighted reflection sheen; 0
    // confidence on any miss reproduces today's output exactly. The weight MUST be PBR_FresnelSchlick, not
    // an ad hoc curve — see PBRLighting.hlsl's EvaluateOpaqueSSR header comment for why (a stronger weight
    // shipped and fed back into runaway brightness on the first GPU test).
    {
        float ssrConfidence;
        float3 ssrColor = EvaluateOpaqueSSR( i.wpos, N, V, orm.g, ssrConfidence );
        float3 ssrF0 = lerp( float3( 0.04, 0.04, 0.04 ), albedo, orm.b );
        float3 ssrFresnel = PBR_FresnelSchlick( saturate( dot( N, V ) ), ssrF0 );
        rgb += ssrColor * ssrConfidence * ssrFresnel;
    }
    // Linear distance fog toward the (linearized) atmosphere color — keeps the HDR buffer consistently linear.
    float f = saturate( ( i.fogDist - FogNear ) / max( 1.0, FogFar - FogNear ) );
    rgb = lerp( rgb, SrgbToLinear( FogColor ), f );
    return float4( rgb, 1.0 );
}

// =====================================================================================================
// Alpha-blended world mesh (ice, glass, magic barriers) — the port of D3D11's DrawMeshInfoListAlphablended.
//
// These surfaces are deliberately NOT lit, on either backend: D3D11 routes any material whose alpha func is
// BLEND/ADD to PS_Simple_FF (see D3D11ForwardPlusRenderer/D3D11DeferredRenderer::BindShaderForTexture — the
// Forward+ renderer explicitly bounces them to the deferred fallback shaders), which is just
//     color = texture * vertexDiffuse * FF textureFactor
// with the blend mode coming from the material's alpha func and depth-write off. `TextureFactor` is the
// per-material tint the CPU computes: the material color when its alpha < 255 (that IS the ice/glass
// translucency), or the env-map strength ramped by sun height for env-mapped materials.
// =====================================================================================================
// SunHeight is Gothic's AC_LightPos.y (sun height above the horizon), which the portal/waterfall-foam
// variants below need for their day/night darkening. It is passed as a scalar root constant rather than
// pulling in the whole Atmosphere CB the D3D11 shaders include, since that is all either one reads.
// EnvCamPosWS/EnvCubeIndex are read only by PSTransparentEnv (the env-map overlay stage). They live here
// rather than in a CB of their own because this pass's root signature is private to it, and CamPosWS is
// otherwise only in FogCB (b1) which this root signature does not carry — referencing that would emit a b1
// binding the PSO has no root parameter for. EnvCubeIndex == 0xFFFFFFFF means the cube is unavailable.
cbuffer TransparencyCB : register(b5) {
    float4 TextureFactor;
    float SunHeight; float3 EnvCamPosWS;
    uint EnvCubeIndex; float3 _tpad;
};
// World->view, VS only, used ONLY by VSTransparentPortal (see below). World.hlsl declares nothing else at
// b4 — the shared World.RootSig's b4 (wind) is never read by this file, and an unused declaration emits no
// binding, so this cannot collide with the opaque world/VOB PSOs.
cbuffer TransparencyViewCB : register(b4) { float4x4 TransparencyView; };

struct VST_OUT { float4 clip : SV_POSITION; float2 uv : TEXCOORD0; float4 col : TEXCOORD1; };

// Shares VS_IN + the world input layout with VSMain; NORMAL is simply not read. This ONE blob feeds both
// the blended color PSO and the depth-fill PSO that follows it — same rule as the water Z-prepass: a
// depth-read-only color pass must share the VS *blob* with the pass that wrote that depth, or the two
// disagree by an ULP on coplanar surfaces and shimmer.
VST_OUT VSTransparent( VS_IN i )
{
    VST_OUT o;
    o.clip = mul( float4( i.pos, 1.0 ), ViewProj );
    o.uv = i.uv;
    o.col = i.col;
    return o;
}

float4 PSTransparent( VST_OUT i ) : SV_TARGET
{
    Texture2D difTex = ResourceDescriptorHeap[MatDiffuseIndex];
    float4 c = difTex.Sample( smp, i.uv );
    // Gothic's vertex color is a BGRA DWORD read through an R8G8B8A8 view — recover RGBA (CLAUDE.md).
    // (D3D11's PS_Simple multiplies it unswizzled, i.e. with R/B transposed; that is a latent bug there,
    // not something to reproduce.)
    c *= i.col.bgra;
    c *= TextureFactor;
    // The scene target is linear HDR on D3D12, so the sampled sRGB texel has to be linearized like every
    // other albedo read; the alpha rides through untouched for the blend.
    return float4( SrgbToLinear( c.rgb ), c.a );
}

// --- Env-map overlay stage (ZenGin zRenderManager.cpp:671-712, D3D11: PS_EnvMap.hlsl) ----------------
// ZenGin does not fold env mapping into the base surface: it appends an EXTRA stage to the same zCShader,
// drawn immediately after the base pass over the same geometry with rgbGen IDENTITY (the env texture
// alone, unlit) and alphaGen FACTOR (alpha = env-map strength scaled by the sky fog color's luma, which
// the CPU computes and passes in TextureFactor.a). That is what gives ice its sheen while the base pass
// keeps the material's own thickness.
//
// Divergence shared with D3D11's PS_EnvMap: ZenGin sphere-maps zFlare1.tga by the CAMERA-space reflection
// vector, which swims with the camera; we sample the reflect_cube.dds the water pass already loads, by the
// WORLD-space reflection vector. Needs the world normal, so this gets its own VS.
struct VSTE_OUT { float4 clip : SV_POSITION; float3 wpos : TEXCOORD0; float3 wnrm : TEXCOORD1; };

VSTE_OUT VSTransparentEnv( VS_IN i )
{
    VSTE_OUT o;
    o.clip = mul( float4( i.pos, 1.0 ), ViewProj );
    o.wpos = i.pos;                          // world verts are already world-space
    o.wnrm = DecodeOctNormal( i.nrm );       // already world-space
    return o;
}

float4 PSTransparentEnv( VSTE_OUT i ) : SV_TARGET
{
    if ( EnvCubeIndex == 0xFFFFFFFF ) discard;

    float3 eye = normalize( i.wpos - EnvCamPosWS );
    float3 r = normalize( reflect( eye, normalize( i.wnrm ) ) );

    TextureCube envTex = ResourceDescriptorHeap[EnvCubeIndex];
    float3 env = envTex.Sample( smp, r ).rgb;

    // rgbGen IDENTITY: emitted unlit and unmodulated, only the alpha is driven. The scene target is linear
    // HDR here, so the sRGB cube texel is linearized like every other albedo read (same rule PSTransparent
    // follows above).
    return float4( SrgbToLinear( env ), TextureFactor.a );
}

// --- MT_WaterfallFoam (D3D11: PS_WaterfallFoam.hlsl) -------------------------------------------------
// Foam sheets over waterfalls. Neither the vertex color nor the FF texture factor is read (D3D11's shader
// samples the texture and applies its own day/night tint, nothing else) — ported line for line.
float4 PSTransparentFoam( VST_OUT i ) : SV_TARGET
{
    Texture2D difTex = ResourceDescriptorHeap[MatDiffuseIndex];
    float4 colour = difTex.Sample( smp, i.uv );

    // darken / lighten foam based on the day / night cycle
    float colourRGB = clamp( SunHeight + 0.2, 0.1, 0.6 );
    // SunHeight is a root-constant scalar (uniform for the whole draw), not per-pixel data.
    [branch]
    if ( SunHeight <= 0.08 ) {
        colour *= float4( colourRGB, colourRGB, colourRGB + 0.1, 0.80 );   // add blue tint at night
    } else {
        colour *= float4( colourRGB, colourRGB, colourRGB, 0.80 );
    }
    return float4( SrgbToLinear( colour.rgb ), colour.a );
}

// --- MT_Portal (D3D11: PS_PortalDiffuse.hlsl, VS_Ex.hlsl's vViewPosition) ----------------------------
// Gothic 1's forest portals: a texture sheet that fades in with distance and darkens with the sun. Gated
// on RendererSettings.DrawG1ForestPortals, same as D3D11. Needs the VIEW-space position, so it gets its
// own VS; everything else in the pass shares VSTransparent.
struct VSTP_OUT { float4 clip : SV_POSITION; float2 uv : TEXCOORD0; float3 vpos : TEXCOORD1; };

VSTP_OUT VSTransparentPortal( VS_IN i )
{
    VSTP_OUT o;
    o.clip = mul( float4( i.pos, 1.0 ), ViewProj );
    o.uv = i.uv;
    // D3D11's VS_Ex sets vViewPosition = mul(float4(positionWorld,1), M_View); world verts are already
    // world-space here, so this is the same value.
    o.vpos = mul( float4( i.pos, 1.0 ), TransparencyView ).xyz;
    return o;
}

float4 PSTransparentPortal( VSTP_OUT i ) : SV_TARGET
{
    // Ported verbatim from PS_PortalDiffuse, INCLUDING its dimensional oddity: D3D11 writes
    // `distance(Input.vViewPosition, Input.vPosition)`, where vPosition is SV_POSITION — i.e. it measures
    // a view-space position against a PIXEL coordinate (HLSL silently truncates the float4 to float3).
    // That is what produces the fade the game actually ships, so it is reproduced rather than "fixed";
    // the explicit float3() below is only there because DXC warns on the implicit truncation.
    // (clip.z differs from D3D11's by the reversed-Z flip, but it is a 0..1 term inside a magnitude of
    // thousands — far below the fade's 1000-unit ramp.)
    float distFromCamera = distance( i.vpos, float3( i.clip.xy, i.clip.z ) );

    // correct issue with transparency of forest portals for certain camera angles
    if ( i.vpos.x > 0 ) { distFromCamera = mad( i.vpos.x, saturate(i.vpos.x / 9000), distFromCamera ); }

    // start / end distances for fading
    float startFade = 6500.0;
    float completeFade = 5500.0;

    // how much to fade the object by
    float percentageFade = saturate(( distFromCamera - completeFade ) / ( startFade - completeFade ));

    // darken the portals depending on where the sun is in the sky
    float darknessFactor = 4.0;
    // SunHeight is a root-constant scalar (uniform for the whole draw), not per-pixel data.
    [branch]
    if ( SunHeight <= 0.05 ) { darknessFactor += ( 1 - SunHeight ) * 6; }
    else                     { darknessFactor = 7.5 - ( 1 + SunHeight ) * 3; }

    // sample the texture we want to fade out and apply relevant darkness factor for day / night cycle
    Texture2D difTex = ResourceDescriptorHeap[MatDiffuseIndex];
    float4 color = difTex.Sample( smp, i.uv ) / darknessFactor;

    return float4( SrgbToLinear( color.rgb ), percentageFade );
}
