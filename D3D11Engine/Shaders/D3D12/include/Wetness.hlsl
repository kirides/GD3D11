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

// Poisson disk for the rain-reach filter. Same first 16 entries as ShadowSampling.h's g_PoissonDisk32
// (duplicated because the D3D12 shaders don't include that D3D11 header); the first 8 form a valid
// distribution on their own and are reused for the inner ring.
static const float2 g_RainPoisson16[16] = {
    float2( -0.94201624, -0.39906216 ), float2(  0.94558609, -0.76890725 ),
    float2( -0.09418410, -0.92938870 ), float2(  0.34495938,  0.29387760 ),
    float2( -0.91588581,  0.45771432 ), float2( -0.81544232, -0.87912464 ),
    float2( -0.38277543,  0.27676845 ), float2(  0.97484398,  0.75648379 ),
    float2(  0.44323325, -0.97511554 ), float2(  0.53742981, -0.47373420 ),
    float2( -0.26496911, -0.41893023 ), float2(  0.79197514,  0.19090188 ),
    float2( -0.24188840,  0.99706507 ), float2( -0.81409955,  0.91437590 ),
    float2(  0.19984126,  0.78641367 ), float2(  0.14383161, -0.14100790 )
};

// ~1m filter radius => ~2m wide wet/dry transition. Kept identical to D3D11's RAIN_WET_BLUR_WORLD.
#define RAIN_WET_BLUR_WORLD 100.0

// Returns 1 where rain reaches this world position, 0 where geometry above it blocks the rain. The rain
// shadowmap is a single ORTHOGRAPHIC normal-Z slice cast along the rain velocity (RenderRainShadowmap).
// Read bindlessly, like Rain.hlsl's IsWet(). 1:1 with D3D11's ComputeRainWetness (ShadowSampling.h) —
// see that function's header for why wetness gets a wide, world-sized, Gaussian-weighted kernel instead
// of the sun shadow's tight silhouette-preserving PCF, and why the bias scales with the kernel.
//
// The kernel radius is in WORLD units, derived from RainViewProj itself, because the two backends cover
// very different world spans with the same 2048² map (D3D11 16384 units, D3D12 kRainShadowWorldSpan
// 49152) — a texel-sized kernel would blur 3x less here than there.
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

    // World -> clip scale per axis (row-vector mul, so the columns of RainViewProj); clip -> uv adds a
    // 0.5 scale on x/y (the y flip doesn't change the radius).
    float sx = length( float3( RainViewProj._11, RainViewProj._21, RainViewProj._31 ) );
    float sy = length( float3( RainViewProj._12, RainViewProj._22, RainViewProj._32 ) );
    float sz = length( float3( RainViewProj._13, RainViewProj._23, RainViewProj._33 ) );

    float2 radiusUV = float2( sx, sy ) * ( 0.5 * RAIN_WET_BLUR_WORLD );
    float zReceiver = clip.z - ( 0.0001 + RAIN_WET_BLUR_WORLD * sz );

    // Gaussian weights, sigma = 0.5 * radius: exp(-r^2 / (2*sigma^2)).
    const float wCenter = 1.0;
    const float rInner  = 0.45;
    const float wInner  = 0.66698;   // exp(-0.45^2 * 2)
    const float wOuter  = 0.13534;   // exp(-1.00^2 * 2)

    float sum = wCenter * rainMap.SampleCmpLevelZero( shadowCmp, uv, zReceiver );
    float weight = wCenter;

    [unroll] for ( int i = 0; i < 8; ++i )
    {
        sum += wInner * rainMap.SampleCmpLevelZero( shadowCmp, uv + g_RainPoisson16[i] * ( rInner * radiusUV ), zReceiver );
        weight += wInner;
    }
    [unroll] for ( int j = 0; j < 16; ++j )
    {
        sum += wOuter * rainMap.SampleCmpLevelZero( shadowCmp, uv + g_RainPoisson16[j] * radiusUV, zReceiver );
        weight += wOuter;
    }

    return smoothstep( 0.0, 1.0, saturate( sum / weight ) );
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

// Puddles: localized pooled water on FLAT ground, distinct from the generic wet-surface sheen every
// exposed pixel gets above. Without this, a pitched roof and a flat street get the SAME roughness dip
// (0.10) because ApplySceneWetness's only directionality test is "faces up at all" — a 30-degree roof
// pitch passes that easily. Real puddles only form on near-horizontal surfaces, so PUDDLE_FLATNESS_MIN is a
// much stricter dot(N,up) threshold than the generic wetness exposure test above.
//
// The mask itself reuses the already-bound rain distortion texture (distortion2.dds, DistortionIndex) as a
// noise field — sampled PLANAR in world XZ and read STATICALLY (no RainTime scroll): a puddle's SHAPE
// shouldn't animate, only the ripple normal already handles the animated part. Thresholding that noise
// against the current wetness level is what makes puddles GROW as rain accumulates (SceneWetness rising
// drives the threshold down, so more of the noise field clears it) and shrink as the ground dries, instead
// of a fixed blob pattern that pops in at full size the moment it starts raining.
//
// TWO octaves, not one: this texture's own large-scale use elsewhere in this file (ApplyRainNormalDeformation
// samples it at roughly a 3300-10000 world-unit tile, for smooth WAVE features) is exactly what a single
// puddle-scale sample must NOT reuse — at that tile size the field barely varies across a normal courtyard,
// so thresholding it just draws the one edge of a near-linear gradient stretched across the whole visible
// area: a puddle that reads as a straight 1-2 meter-wide line, not a pool. PUDDLE_NOISE_WORLD_SCALE is a
// puddle-appropriate few-metre tile so multiple blobs fit in a courtyard; PUDDLE_DETAIL_WORLD_SCALE is a
// smaller second sample multiplied in purely to break the blob's perimeter into something irregular.
static const float PUDDLE_FLATNESS_MIN = 0.92;           // dot(N,up); excludes pitched roofs, keeps flat ground/rooftops
static const float PUDDLE_NOISE_WORLD_SCALE  = 400.0;    // world units per tile — puddle-placement scale (~4 m)
static const float PUDDLE_DETAIL_WORLD_SCALE = 120.0;    // world units per tile — perimeter-irregularity scale (~1.2 m)

// Returns puddle intensity [0,1] at this fragment: 0 = ordinary wet surface, 1 = fully pooled/mirror-flat.
// `geomN` must be the UNPERTURBED geometric normal (before ApplyRainNormalDeformation's ripple), or a
// rippled wall normal could transiently point "up" and read as flat.
float ComputePuddleMask( float3 geomN, float3 wsPosition, float wetness )
{
    float flatness = saturate( ( dot( geomN, float3( 0, 1, 0 ) ) - PUDDLE_FLATNESS_MIN ) / ( 1.0 - PUDDLE_FLATNESS_MIN ) );
    if ( flatness <= 0.0 ) return 0.0;

    Texture2D distTex = ResourceDescriptorHeap[DistortionIndex];
    float placement = distTex.SampleLevel( smp, wsPosition.xz / PUDDLE_NOISE_WORLD_SCALE, 0 ).r;
    // Offset the second sample's UV origin so it isn't just a scaled copy of the same pattern (which would
    // still line up into bands at some scale ratios) — an arbitrary irrational-ish shift is enough.
    float detail = distTex.SampleLevel( smp, wsPosition.xz / PUDDLE_DETAIL_WORLD_SCALE + 17.31, 0 ).g;
    float noise = placement * 0.7 + detail * 0.3;

    // Higher wetness lowers the threshold the noise has to clear, so puddles spread as rain persists. The
    // 0.08 half-width keeps the pooled/dry boundary from being a hard, aliased edge.
    float threshold = lerp( 0.85, 0.35, saturate( wetness ) );
    float pooled = smoothstep( threshold - 0.08, threshold + 0.08, noise );
    return pooled * flatness;
}

// Applies scene wetness at one fragment. N / albedo / roughness are modified in place. Returns
// localWettness [0,1] — 0 means "nothing was changed", which callers use to skip the remaining wetness
// work. Call with the UNPERTURBED-by-wetness normal and BEFORE the sun shadow lookup, matching D3D11 (it
// computes `shadow` from the G-buffer normal, then deforms).
//
// NO STYLIZED SHEEN HERE (deliberate D3D12 divergence from D3D11, decided 2026-08-28). D3D11's
// PS_DS_AtmosphericScattering.hlsl adds three fixed-direction "sparkle" highlights on top of this
// (`l1 = normalize(0,0.5,-1)`, etc. — literal constants, never SunDirWS or any real light/reflection
// direction). It reads as a decorative hack because it is one: those directions don't track the sun, the
// camera, or the environment, so the highlight they produce sits in a fixed relationship to the world/view
// regardless of actual lighting conditions — reported by the maintainer as "random specular highlights that
// stay in one place" and correctly identified as looking wrong. D3D12 already has a CORRECT alternative for
// exactly this ("wet ground should look shinier"): the roughness dip a few lines below feeds directly into
// ComputeSunLightingPBR's Cook-Torrance sun term (PBRLighting.hlsl), which DOES respond properly to the
// real sun direction, view angle and roughness, and puddled pixels (near-mirror roughness) additionally get
// EvaluateOpaqueSSR's real environment reflection. Porting the fixed-direction sheen would only add fake,
// redundant energy on top of lighting that is already physically correct.
float ApplySceneWetness( float3 wpos, inout float3 N, inout float3 albedo, inout float roughness )
{
    if ( SceneWetness <= 0.0 || RainShadowIndex == 0xffffffff || DistortionIndex == 0xffffffff )
        return 0.0;   // not raining / drying done, or the rain map / distortion2.dds never loaded

    float wetness = SampleRainReach( wpos ) * SceneWetness;
    if ( wetness < 0.001 ) return 0.0;   // matches D3D11's pixelWettnes cutoff

    float3 geomN = N;   // captured before ApplyRainNormalDeformation ripples it — see ComputePuddleMask's requirement
    float3 wetNormal = ApplyRainNormalDeformation( N, wpos );

    // Downward-facing surfaces (undersides, ceilings) stay dry; upward-facing ones collect the most
    // water. Both terms are D3D11's, with its pow(x,4) / squaring kept inline.
    float wDot  = saturate( dot( wetNormal, float3( 0, -1, 0 ) ) );
    float wDot2 = wDot * wDot;
    wetness *= 1.0 - wDot2 * wDot2;
    float exposure = saturate( dot( wetNormal, float3( 0, 1, 0 ) ) );
    wetness *= exposure * exposure;
    if ( wetness <= 0.0 ) return 0.0;

    // Puddles: a stricter, flat-ground-only pool on top of the generic wet-surface sheen above. See
    // ComputePuddleMask's header comment for why this needs its own (much tighter) flatness test.
    float puddle = ComputePuddleMask( geomN, wpos, wetness );

    // Only ripple while it is actually RAINING (RainFxWeight), not while the ground is drying out. Pooled
    // water stays closer to a flat mirror than a rippling wet wall, so puddle pixels get less of the
    // tri-planar ripple deformation blended in.
    N = normalize( lerp( N, wetNormal, RainFxWeight * wetness * 0.5 * ( 1.0 - puddle * 0.8 ) ) );

    // PBR stand-in for D3D11's "specPower -> 150, specIntensity -> 0": water is smooth, so pull
    // roughness toward glossy and let Cook-Torrance tighten the existing highlights. Puddle pixels go all
    // the way to near-mirror (0.02) instead of the generic wet-surface floor (0.10) — this is also what
    // clears EvaluateOpaqueSSR's roughness gate (PBRLighting.hlsl), so a puddle shows an actual reflection
    // of its surroundings rather than just the fixed-direction sheen below.
    float targetRoughness = lerp( 0.10, 0.02, puddle );
    roughness = lerp( roughness, targetRoughness, wetness );

    // Desaturate + darken (D3D11's wetPixel) — pooled water darkens further still, matching how a puddle
    // reads visually darker/more reflective than a merely damp surface under overcast rain light.
    float lum = dot( albedo, float3( 0.3333, 0.3333, 0.3333 ) );
    float darkenAmount = lerp( 0.75, 0.55, puddle );
    albedo = lerp( albedo, lerp( lum.xxx, albedo, darkenAmount ) * darkenAmount, wetness );

    return wetness;
}
