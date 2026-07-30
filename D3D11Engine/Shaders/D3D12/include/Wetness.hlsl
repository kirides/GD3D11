#pragma once

// Scene-wetness ("wet ground") for the D3D12 Forward+ passes — the port of D3D11's
// PS_DS_AtmosphericScattering.hlsl ApplySceneWettness/ApplyRainNormalDeformation plus the bits of its
// litPixel call site that the wetness feeds (the 0.8x sun dimming and the shadow-boosted specWet add).
//
// D3D11 does this ONCE, in the deferred lighting pass, because every opaque pixel is already in the
// G-buffer by then. Forward+ has no such pass, so the effect lives here and is called from each lit
// pixel shader (World.hlsl / Vob.hlsl / Skeletal.hlsl) — same math, applied per fragment.
//
// Two deliberate divergences from the D3D11 spec, both because the D3D12 backend is PBR + world-space:
//   * D3D11 stores VIEW-space normals in its G-buffer, so it rotates into world space, deforms, and
//     rotates back. Forward+ normals are already world-space, so the round-trip is dropped.
//   * D3D11 modulates a Blinn-Phong (specIntensity, specPower) pair. The PBR analogue of "lerp
//     specPower toward 150" is pulling ROUGHNESS toward smooth, which tightens the existing
//     Cook-Torrance sun/point-light highlights on its own. D3D11's reflection-cubemap term
//     (TX_ReflectionCube / reflect_cube.dds) has no D3D12 equivalent yet, so only the three
//     fixed-direction sheen highlights it adds on top of the cube survive here.
//
// #include this AFTER the including shader has declared, in its own ShadowCB (the shared per-frame
// shadow/wetness constant buffer — see D3D12GraphicsEngine::UploadWetnessConstants):
//   float4x4 RainViewProj; float SceneWetness; float RainFxWeight; float RainTime;
//   uint RainShadowIndex; uint DistortionIndex; float RainShadowMapSize;
// ...and the samplers `smp` (wrapping, for the distortion texture) and `shadowCmp` (LESS_EQUAL PCF).

static const float WET_WEIGHT_BIAS = -0.55;
static const float WET_WEIGHT_MUL  = 0.7;

// Returns 1 where rain reaches this world position, 0 where geometry above it blocks the rain. The rain
// shadowmap is a single ORTHOGRAPHIC normal-Z slice cast along the rain velocity (RenderRainShadowmap),
// so this mirrors D3D11's rain call into ComputeShadowValueDirect: bias 0.0001, softnessScale 2.5,
// 4x4 PCF. Read bindlessly, like Rain.hlsl's IsWet().
//
// OUTSIDE the rain camera the answer is 1 (EXPOSED), not 0. The map only covers a slab around the
// player, so "outside" means "no occluder information", and the overwhelmingly common case out there is
// open sky — defaulting to "sheltered" instead drew a hard dry ring at the map border, very visible as
// clean-lit ground whenever the player looked down a hill or across a valley (the far terrain leaves the
// slab both laterally and in depth). This also matches what D3D11 does: ComputeShadowValueDirect
// initialises shadow=1 and only samples inside [0,1] UVs.
float SampleRainReach( float3 wpos )
{
    Texture2D rainMap = ResourceDescriptorHeap[RainShadowIndex];

    float4 clip = mul( float4( wpos, 1.0 ), RainViewProj );
    clip.xyz /= clip.w;                                       // no-op for the ortho cast; kept for parity with IsWet()
    float2 uv = clip.xy * float2( 0.5, -0.5 ) + 0.5;
    if ( any( uv < 0.0 ) || any( uv > 1.0 ) || clip.z < 0.0 || clip.z > 1.0 )
        return 1.0;                                           // outside the rain camera → assume open sky

    const float texStep = 2.5 / max( RainShadowMapSize, 1.0 );   // softnessScale 2.5, in texels
    const float bias = 0.0001;
    float sum = 0.0;
    [unroll] for ( float y = -1.5; y <= 1.5; y += 1.0 )
    [unroll] for ( float x = -1.5; x <= 1.5; x += 1.0 )
        sum += rainMap.SampleCmpLevelZero( shadowCmp, uv + float2( x, y ) * texStep, clip.z - bias );
    return saturate( sum / 16.0 );
}

// Animated tri-planar rain-ripple normal. 1:1 with D3D11's ApplyRainNormalDeformation apart from
// staying in world space (see the header note). `wsNormal` must be normalized — the weight
// normalization below relies on at least one component being >= 1/sqrt(3), which only holds for a
// unit vector.
float3 ApplyRainNormalDeformation( float3 wsNormal, float3 wsPosition )
{
    Texture2D distTex = ResourceDescriptorHeap[DistortionIndex];

    const float scale = 1000.0;
    float2 uv[4] =
    {
        wsPosition.zy / scale,
        wsPosition.xz / ( scale * 2 ),
        wsPosition.xz / ( scale * 2 ),
        wsPosition.xy / scale
    };

    float groundSpeed = 0.1 * RainFxWeight;
    float downSpeed   = 0.2 * RainFxWeight;
    uv[0] += float2( 0, RainTime * downSpeed );
    uv[1] += float2( RainTime * groundSpeed, RainTime * groundSpeed );
    uv[2]  = uv[2] * float2( 0.8, 1.2 ) + float2( -RainTime * groundSpeed * 0.7, RainTime * groundSpeed * 0.4 );
    uv[3] += float2( 0, RainTime * downSpeed );

    // Per-axis blend weights, with the blending zone tightened up (bias/mul) so each planar
    // projection dominates near its own axis instead of smearing across all three.
    float3 weights = abs( wsNormal );
    weights = max( ( weights + WET_WEIGHT_BIAS ) * WET_WEIGHT_MUL, 0.0 );
    weights /= ( weights.x + weights.y + weights.z ).xxx;
    weights.xz *= 0.6;
    weights.y  *= 0.7;

    float3 dist[3] =
    {
        normalize( distTex.Sample( smp, uv[0] ).zyx * 2 - 1 ),
        normalize( distTex.Sample( smp, uv[1] ).xzy * 2 - 1 ) * 0.5 +
        normalize( distTex.Sample( smp, uv[2] ).xzy * 2 - 1 ) * 0.5,
        normalize( distTex.Sample( smp, uv[3] ).xyz * 2 - 1 )
    };

    weights *= weights;   // inline pow(weights, 4)
    weights *= weights;

    const float distWeight = 0.9;
    float3 n = wsNormal;
    [unroll]
    for ( int i = 0; i < 3; ++i )
        n = lerp( n, dist[i], weights[i] * distWeight );

    return normalize( n );
}

// Applies scene wetness at one fragment. N / albedo / roughness are modified in place; `sheen` returns
// D3D11's specAdd (the additive wet shine), which the caller adds AFTER lighting so it isn't scaled by
// N.L. Returns localWettness [0,1] — 0 means "nothing was changed", which callers use to skip the
// remaining wetness work. Call with the UNPERTURBED-by-wetness normal and BEFORE the sun shadow lookup,
// matching D3D11 (it computes `shadow` from the G-buffer normal, then deforms).
float ApplySceneWetness( float3 wpos, float3 V, inout float3 N, inout float3 albedo,
                         inout float roughness, out float sheen )
{
    sheen = 0.0;
    if ( SceneWetness <= 0.0 || RainShadowIndex == 0xffffffff || DistortionIndex == 0xffffffff )
        return 0.0;   // not raining / drying done, or the rain map / distortion2.dds never loaded

    float wetness = SampleRainReach( wpos ) * SceneWetness;
    if ( wetness < 0.001 ) return 0.0;   // matches D3D11's pixelWettnes cutoff

    float3 wetNormal = ApplyRainNormalDeformation( N, wpos );

    // Downward-facing surfaces (undersides, ceilings) stay dry; upward-facing ones collect the most
    // water. Both terms are D3D11's, with its pow(x,4) / squaring kept inline.
    float wDot  = saturate( dot( wetNormal, float3( 0, -1, 0 ) ) );
    float wDot2 = wDot * wDot;
    wetness *= 1.0 - wDot2 * wDot2;
    float exposure = saturate( dot( wetNormal, float3( 0, 1, 0 ) ) );
    wetness *= exposure * exposure;
    if ( wetness <= 0.0 ) return 0.0;

    // Only ripple while it is actually RAINING (RainFxWeight), not while the ground is drying out.
    N = normalize( lerp( N, wetNormal, RainFxWeight * wetness * 0.5 ) );

    // PBR stand-in for D3D11's "specPower -> 150, specIntensity -> 0": water is smooth, so pull
    // roughness toward glossy and let Cook-Torrance tighten the existing highlights.
    roughness = lerp( roughness, 0.10, wetness );

    // Desaturate + darken (D3D11's wetPixel).
    float lum = dot( albedo, float3( 0.3333, 0.3333, 0.3333 ) );
    albedo = lerp( albedo, lerp( lum.xxx, albedo, 0.75 ) * 0.75, wetness );

    // The three fixed-direction sheen highlights. D3D11 builds l1 already in view space and rotates
    // l2/l3 world->view; here all three are world-space (the effect is a stylized sheen, not a
    // physical reflection, so the absolute directions only set where the shine sits on the ground).
    const float specPower = 150.0;
    float3 l1 = normalize( float3(  0.0,   0.5,  -1.0   ) );
    float3 l2 = normalize( float3( -0.333, 0.533, 0.333 ) );
    float3 l3 = normalize( float3(  0.0,   0.566, -0.666 ) );
    float s1 = saturate( dot( N, normalize( l1 + V ) ) );
    float s2 = saturate( dot( N, normalize( l2 + V ) ) );
    float s3 = saturate( dot( N, normalize( l3 + V ) ) );
    float reflection = pow( s1, specPower ) * 0.7 + pow( s2, specPower ) * 0.7 + pow( s3, specPower ) * 0.6;

    sheen = reflection * wetness * lerp( 0.08, 0.10, RainFxWeight );
    return wetness;
}
