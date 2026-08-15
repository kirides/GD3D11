#pragma once

// Shared Forward+ lighting math for World.hlsl / Vob.hlsl / Skeletal.hlsl — previously triplicated verbatim
// across all three; now a single source of truth. Ported from the D3D11 feat/pbr branch (Shaders/include/
// PointLightShadows.h): Cook-Torrance GGX PBR, CSM sun shadow, point-light shadow cubes, and the tiled
// point-light accumulator.
//
// #include this AFTER the including shader has already declared (with its own, per-shader register slots):
//   #include "include/ForwardPlusTypes.hlsl"   (GPULight, LightGrid, TILE_SIZE, MAX_ACTIVE_LIGHTS, NUM_Z_SLICES,
//                                                NUM_CSM_CASCADES)
//   cbuffer FogCB    { ...; float3 CamPosWS; ...; }
//   cbuffer LightCB  { uint LightCount; uint NumTilesX; uint LimitLightIntensity; ...;
//                      float ProjA; float ProjB; float NearZ; float FarZ; }   -- the last 4 feed ComputeZSlice
//                      (ProjA/ProjB = reversed-Z depth->viewZ terms, matches LightCull.hlsl's CullCB;
//                      NearZ/FarZ = the log-distributed cluster Z range, also matching CullCB)
//   cbuffer ShadowCB { float4x4 CascadeViewProj[NUM_CSM_CASCADES]; float3 SunDirWS; float ShadowMapSize;
//                      float3 SunColor; float SunIntensity; float3 CascadeTexelWorld; float AmbientStrength;
//                      float ShadowAOStrength; float WorldAOStrength; ...; }
//   StructuredBuffer<GPULight> Lights; StructuredBuffer<LightGrid> LightGridBuf;
//   Texture2DArray ShadowMap; SamplerComparisonState shadowCmp; TextureCubeArray PointShadowCubes;
// The LOW-RES static cube array is not a declared binding: it is fetched bindlessly (SM6.6
// ResourceDescriptorHeap) from LightCB's PointShadowLowIndex, so adding the second tier cost no root
// signature changes — LightCB already had a spare 4th root constant in every one of these shaders.
// (HLSL globals are visible by name across the whole translation unit regardless of #include position, but
// they must be DECLARED — i.e. appear earlier in the file — before this header's functions reference them.)

// De-lights diffuse textures by lifting baked shadows and softening baked highlights
float3 DelightDiffuse( float3 linearAlbedo )
{
    float luminance = dot( linearAlbedo, float3( 0.2126, 0.7152, 0.0722 ) );
    // Normalize luminance variations caused by baked directional light
    float delightFactor = 1.0 / max( sqrt( luminance + 1e-4 ), 0.2 );
    return saturate( linearAlbedo * lerp( 1.0, delightFactor, 0.5 ) );
}

// Point-light shadow: returns 1 = lit, 0 = occluded. The cube stores the NATURAL hyperbolic z of the caster's
// 90-deg PerspectiveFovLH(near 15, far range*2). Reconstruct the same z from the fragment: the depth on a cube
// face is driven by the DOMINANT-AXIS distance (the face's view-space z), so zView = max(|dx|,|dy|,|dz|), then
// apply the LH projection z-map. Most acne bias is the PSO's hardware slope bias; add a small normal offset +
// constant. 4-tap rotated-disk PCF softens the edges; a camera-distance fade is applied at the call site.
// `encodedIndex` is GPULight::ShadowCubeIndex: the low 30 bits are the cube slot, bit 30 (kShadowTierLow)
// selects the LOW-RES static cube array (kStaticCubeSize^2, fetched bindlessly via LightCB's
// PointShadowLowIndex) over the full-res dynamic one (PointShadowCubes). `lightPos`/`range` are the CUBE's
// origin and far-plane basis — GPULight::ShadowOrigin /
// ShadowRange, NOT the light's own position/range, because co-located static lights share one clustered cube.
//
// The low-res tier is 4x coarser per axis, so its texels cover 4x more world space at the same distance: the
// normal-offset bias and the PCF disk are scaled to match, otherwise a 32^2 cube acnes badly on the flat walls
// it exists to occlude. It is deliberately blurrier — the static tier is there to stop room-to-room bleed, not
// to resolve shadow detail.
float SamplePointShadow( int encodedIndex, float3 wpos, float3 N, float3 lightPos, float range )
{
    int   slot   = encodedIndex & kShadowSlotMask;
    bool  lowRes = ( encodedIndex & kShadowTierLow ) != 0;
    float coarse = lowRes ? 4.0 : 1.0;
    // Full-res slots keep their STATIC depth in PointShadowCubes and this frame's moving (skeletal) casters in a
    // SEPARATE array; taking the min of the two comparisons is "occluded by either", which is exactly what the
    // old composited cube produced — minus the per-slot CopyTextureRegion that used to build it every frame.
    // The bit is only set for slots whose overlay actually has casters, so a light with no NPC nearby pays for
    // no extra sample at all.
    bool  hasDyn = ( encodedIndex & kShadowHasDynamic ) != 0;

    float3 d  = ( wpos + N * ( range * 0.01 * coarse ) ) - lightPos;   // normal-offset bias (world-space, uniform)
    float3 ad = abs( d );
    float  zView = max( ad.x, max( ad.y, ad.z ) );            // dominant cube-axis depth = the face's view-space z
    const float n = 15.0;
    float  f = range * 2.0;
    float  compareDepth = ( f / ( f - n ) ) * ( 1.0 - n / zView ) - 0.001 * coarse;   // same LH hyperbolic z the caster wrote
    float3 L = normalize( d );

    // P2.10e polish: 4-tap rotated-disk PCF on a basis perpendicular to L (cube sampling follows the offset dir,
    // so a small angular offset lands on neighbouring texels). Softens the previously single-tap hard edges. The
    // offset grows a little with distance so the world-space penumbra stays roughly constant across the range.
    float3 up = abs( L.y ) < 0.99 ? float3( 0, 1, 0 ) : float3( 1, 0, 0 );
    float3 t  = normalize( cross( up, L ) );
    float3 bt = cross( L, t );
    float  r  = ( 0.006 + 0.010 * saturate( zView / f ) ) * coarse;
    static const float2 kDisk[4] = { float2( 0.7, 0.7 ), float2( -0.7, 0.7 ), float2( 0.7, -0.7 ), float2( -0.7, -0.7 ) };

    // The low-res static tier is deliberately blurry (see the header note): a 4x-coarse cube is there to stop
    // room-to-room bleed, not to resolve edges, and its disk radius `r` is already scaled 4x to match. Four
    // taps on it mostly re-average the same kStaticCubeSize^2 texel neighbourhood, so it takes a 2-tap
    // diagonal (kDisk[0]/kDisk[3], opposite corners — the widest pair) for half the texture ops. The full-res
    // tier keeps all four, and with it the optional dynamic-overlay min, where the resolution is actually
    // there to be filtered.
    const int  taps   = lowRes ? 2 : 4;
    const int  stride = lowRes ? 3 : 1;
    float sh = 0.0;
    [loop]
    for ( int s = 0; s < taps; ++s )
    {
        float3 o = normalize( L + ( kDisk[s * stride].x * t + kDisk[s * stride].y * bt ) * r );
        if ( lowRes )
        {
            TextureCubeArray lowCubes = ResourceDescriptorHeap[PointShadowLowIndex];
            sh += lowCubes.SampleCmpLevelZero( shadowCmp, float4( o, (float)slot ), compareDepth );
        }
        else
        {
            float s = PointShadowCubes.SampleCmpLevelZero( shadowCmp, float4( o, (float)slot ), compareDepth );
            if ( hasDyn )
            {
                TextureCubeArray dynCubes = ResourceDescriptorHeap[PointShadowDynIndex];
                s = min( s, dynCubes.SampleCmpLevelZero( shadowCmp, float4( o, (float)slot ), compareDepth ) );
            }
            sh += s;
        }
    }
    return sh / (float)taps;
}

// PCF footprint, expressed as a WORLD radius rather than a texel count. The kernel used to step
// texel * (1 + c*1.5), i.e. the filtered area grew ~4x per cascade split (~1.8 -> 12 -> 94 world units at a
// 2048^2 map with the default splits). On sub-texel, densely self-occluding geometry — grass above all — the
// far cascades then averaged a footprint that is essentially 100% blade-covered and collapsed into a solid,
// shapeless "everything is shadowed" mask, with a razor-sharp step at every split. Targeting a fixed world
// radius keeps the blur physically comparable across cascades; the clamp keeps every tap at least one texel
// apart (no sparse, aliasing kernel in the near cascade) and at most two (near shadows stay crisp).
static const float kPcfWorldRadius = 12.0;   // world units, half-width of the 5x5 kernel
static const float kPcfMinTexels   = 1.0;
static const float kPcfMaxTexels   = 2.0;

// Fraction of a cascade's footprint (per edge) used to cross-fade into the next cascade. Without it the
// cascade switch is a hard discontinuity — invisible on large flat surfaces, glaring on grass, where the two
// cascades disagree about how self-shadowed a blade is. D3D11 blends the same way (ShadowSampling.h's
// GetCascadeUVAndBounds blendFactor -> `shadow = lerp(shadow, shadowNext, blendFactor)`).
static const float kCascadeBlendBand = 0.12;

float SampleCascadePCF( int c, float2 uv, float depth )
{
    const float texel = 1.0 / ShadowMapSize;
    // World extent this cascade covers = texels * world-units-per-texel. The kernel reaches 2 taps out per
    // axis, so the step that yields a kPcfWorldRadius half-width is radius / (2 * cascadeWorld) in UV.
    float cascadeWorld = max( CascadeTexelWorld[c] * ShadowMapSize, 1e-4 );
    float pcfStep = clamp( kPcfWorldRadius / ( 2.0 * cascadeWorld ),
                           kPcfMinTexels * texel, kPcfMaxTexels * texel );

    // Uniformity pre-test before committing to the full 5x5. Sample the kernel's centre and its four
    // corners; when all five agree the footprint is entirely lit or entirely occluded, the remaining 20
    // taps cannot return anything else, so bail with that value. Open sun and deep shade are the large
    // majority of shaded pixels, so the common case becomes 5 taps instead of 25 and only transition
    // pixels pay 30. shadowCmp is a COMPARISON_MIN_MAG_LINEAR sampler, so a tap that straddles a shadow
    // edge comes back fractional and fails both tests — the predicate errs towards the full kernel.
    //
    // The pre-taps sit at the kernel's full extent, so fooling it needs an occluder that fits entirely
    // between a corner and the centre, i.e. sub-2-texel detail. Dense sub-texel casters (grass — see the
    // kPcfWorldRadius note above) are the one case that can read "solid" for a footprint the full kernel
    // would have averaged to a partial value; that is the first thing to check if far-cascade vegetation
    // shadows go back to looking like a flat black mask.
    const float2 kPre[5] = { float2( 0, 0 ), float2( -2, -2 ), float2( 2, -2 ), float2( -2, 2 ), float2( 2, 2 ) };
    float pre = 0.0;
    [unroll] for ( int p = 0; p < 5; ++p )
        pre += ShadowMap.SampleCmpLevelZero( shadowCmp, float3( uv + kPre[p] * pcfStep, c ), depth );
    if ( pre == 0.0 ) return 0.0;
    if ( pre == 5.0 ) return 1.0;

    float sh = 0.0;
    [unroll] for ( int y = -2; y <= 2; ++y )
    [unroll] for ( int x = -2; x <= 2; ++x )
        sh += ShadowMap.SampleCmpLevelZero( shadowCmp, float3( uv + float2( x, y ) * pcfStep, c ), depth );
    return sh / 25.0;
}

// Returns 1.0 = fully lit, 0.0 = fully occluded. Picks the first cascade whose footprint contains the point
// (0 tightest), applies a per-cascade world-space normal bias, and does a PCF tap. Mirrors the D3D11
// ComputeCascadedShadowValueSoft selection/bounds (GetCascadeUVAndBounds) minus the blue-noise/PCSS machinery.
//
// `N` is the BIAS normal and does not have to be the shading normal: what it is for is pushing the lookup off
// the caster, so it must describe the real surface. Vegetation.hlsl deliberately passes the card's geometric
// normal here while shading with an up-bent one — feeding the faked world-up normal in would drive slopeScale
// (and therefore the whole bias) to ~0 on near-edge-on blades, which is what made grass self-shadow itself
// into a flat black mask.
//
// Bias matches D3D11's scheme (Shaders/ShadowSampling.h ComputeCascadedShadowValueSoft + its PS_Diffuse/
// PS_FP_ShadowMask/PS_DS_AtmosphericScattering call sites): the real bias is the world-space normal offset,
// SLOPE-SCALED so it's ~0 for a surface facing the sun and only grows at grazing angles — a flat offset (as
// this used to be) over-biases front-facing ground/wall contacts and detaches the shadow from its caster
// (peter-panning). The NDC-depth compare bias is a tiny constant (D3D11: 0.000003) only meant to fight
// self-shadow z-fighting at zero slope — NOT a general-purpose bias. This cascade's ortho depth range
// (orthoFar-orthoNear, see ComputeCascadeMatrices) commonly spans several thousand world units, so the old
// 0.0015 constant here was ~500x too large — worth 10+ world units of erroneous depth offset on its own.
//
// `vertLighting` is Gothic's baked static vertex light, and it is what this returns when NO cascade claims the
// point — exactly D3D11's scheme, where `float shadow = vertLighting;` is the initialiser of both PS_Diffuse's
// shadow and ComputeCascadedShadowValueSoft's (ShadowSampling.h), surviving whenever the cascade search
// fails. Returning a flat 1.0 here (as this used to) hands full sun to any interior past the CSM range.
float ComputeSunShadow( float3 wpos, float3 N, float vertLighting )
{
    const float margin = 1.5 / ShadowMapSize;   // (the kernel step now lives in SampleCascadePCF)
    const float constantDepthBias = 0.000003;

    float rawNoL = dot( N, SunDirWS );
    float shadowNoL = saturate( rawNoL );
    float slopeScale = sqrt( saturate( 1.0 - shadowNoL * shadowNoL ) );

    [unroll]
    for ( int c = 0; c < NUM_CSM_CASCADES; ++c )
    {
        float3 biased = wpos + N * ( slopeScale * CascadeTexelWorld[c] );   // normal bias, slope- and texel-scaled
        float4 sp = mul( float4( biased, 1.0 ), CascadeViewProj[c] );
        float2 uv = sp.xy * float2( 0.5, -0.5 ) + 0.5;
        if ( uv.x > margin && uv.x < 1.0 - margin && uv.y > margin && uv.y < 1.0 - margin &&
             sp.z >= 0.0 && sp.z <= 1.0 )
        {
            float sh = SampleCascadePCF( c, uv, sp.z - constantDepthBias );

            // Cross-fade out of this cascade near its footprint edge. `edge` is the distance to the nearest
            // border in UV (0 at the border, 0.5 dead centre), so blend ramps 0 -> 1 across the outer band.
            float2 toBorder = min( uv, 1.0 - uv );
            float  edge = min( toBorder.x, toBorder.y );
            float  blend = saturate( 1.0 - edge / kCascadeBlendBand );

            if ( blend > 0.0 )
            {
                if ( c + 1 < NUM_CSM_CASCADES )
                {
                    // Re-project into the next (coarser) cascade with ITS bias and blend in, when it covers
                    // this point. Cheap: only the ~12% of pixels sitting in a band pay the second 5x5 tap.
                    const int n = min( c + 1, NUM_CSM_CASCADES - 1 );
                    float3 biasedNext = wpos + N * ( slopeScale * CascadeTexelWorld[n] );
                    float4 spNext = mul( float4( biasedNext, 1.0 ), CascadeViewProj[n] );
                    float2 uvNext = spNext.xy * float2( 0.5, -0.5 ) + 0.5;
                    if ( uvNext.x > margin && uvNext.x < 1.0 - margin &&
                         uvNext.y > margin && uvNext.y < 1.0 - margin &&
                         spNext.z >= 0.0 && spNext.z <= 1.0 )
                        sh = lerp( sh, SampleCascadePCF( n, uvNext, spNext.z - constantDepthBias ), blend );
                }
                else
                {
                    // Last cascade: nothing coarser to fall into, and the loop's fallthrough is "lit" — so
                    // fade to lit here too instead of ending the shadow range on a hard edge.
                    sh = lerp( sh, 1.0, blend );
                }
            }
            return sh;
        }
    }
    return vertLighting;   // outside all cascades → fall back to baked light (see the header note)
}

// The bindless ORM SRV index (MatOrmIndex, b6) packs the FxMap's channel layout into its top 2 bits — see
// D3D12GraphicsEngine.cpp's EncodeOrmSlot (mirrors MyDirectDrawSurface7::AvailableMaterials). This lets one
// bound texture stand in for whatever _ORM/_OR/_R map the material actually shipped, instead of every material
// paying for a full 3-channel _ORM.DDS. Keep the two in sync.
static const uint ORM_INDEX_MASK  = 0x3FFFFFFFu;
static const uint ORM_FORMAT_SHIFT = 30;

// Returns {AO, Roughness, Metallic}, decoded per the packed format:
//   0 = ORM       (_ORM.DDS, or the 1x1 default when the material has no FxMap): r=AO g=Roughness b=Metallic
//   1 = AoRoughness (_OR.DDS): r=AO g=Roughness, Metallic defaults to 0
//   2 = Roughness   (_R.DDS):  r=Roughness only, AO defaults to 1, Metallic to 0
float3 SampleOrm( uint packedOrmIndex, float2 uv )
{
    Texture2D ormTex = ResourceDescriptorHeap[packedOrmIndex & ORM_INDEX_MASK];
    float4 s = ormTex.Sample( smp, uv );
    uint fmt = packedOrmIndex >> ORM_FORMAT_SHIFT;
    if ( fmt == 1 ) return float3( s.r, s.g, 0.0 );
    if ( fmt == 2 ) return float3( 1.0, s.r, 0.0 );
    return s.rgb;
}

// --- Cook-Torrance GGX PBR (ported verbatim from the D3D11 feat/pbr branch: Shaders/include/PointLightShadows.h) ---
static const float PBR_PI = 3.14159265;

float3 SrgbToLinear( float3 c )   // accurate sRGB EOTF — linearize gamma-encoded albedo so lighting is done in linear space
{
    return select( c <= 0.04045, c / 12.92, pow( ( c + 0.055 ) / 1.055, 2.4 ) );
}

float  PBR_SafeRoughness( float r ) { return max( saturate( r ), 0.045 ); }
float  PBR_DistributionGGX( float NdotH, float roughness )
{
    float a = roughness * roughness;
    float a2 = a * a;
    float denom = NdotH * NdotH * ( a2 - 1.0 ) + 1.0;
    return a2 / max( PBR_PI * denom * denom, 1e-4 );
}
float  PBR_GeometrySchlickGGX( float NdotX, float roughness )
{
    float r = roughness + 1.0;
    float k = ( r * r ) / 8.0;
    return NdotX / max( NdotX * ( 1.0 - k ) + k, 1e-4 );
}
float  PBR_GeometrySmith( float NdotV, float NdotL, float roughness )
{
    return PBR_GeometrySchlickGGX( NdotV, roughness ) * PBR_GeometrySchlickGGX( NdotL, roughness );
}
float  PBR_Pow5( float x ) { float x2 = x * x; return x2 * x2 * x; }
float3 PBR_FresnelSchlick( float cosTheta, float3 F0 ) { return F0 + ( 1.0 - F0 ) * PBR_Pow5( saturate( 1.0 - cosTheta ) ); }

// Full Cook-Torrance (energy-conserving diffuse + specular). attenuation folds in falloff/shadow; NdotL applied here.
// specularScale scales ONLY the specular lobe (diffuse is untouched): 0 turns the light into a pure area-brightener.
// Mirrors D3D11's `spec = PLS_CalcBlinnPhongLighting(...) * light.Color.w` in ForwardPlusLighting.hlsl /
// CS_TiledShading.hlsl / PS_DS_PointLight*.hlsl — see ApplyTiledLight for who passes 0.
float3 PBR_DirectLighting( float3 baseColor, float3 lightColor, float3 N, float3 V, float3 L,
                           float roughness, float metallic, float attenuation, float specularScale )
{
    float NdotL = saturate( dot( N, L ) );
    // NdotV deliberately does NOT gate the whole function (D3D11's FP_ComputeSunLighting has no view-
    // dependent cutoff on its diffuse/ambient response either — only its specular term reads V at all).
    // saturate(dot(N,V)) legitimately touches exactly 0 at ordinary shading-normal/silhouette grazing —
    // e.g. standing in the street looking up along a shallow roof pitch — and early-returning 0.0 there
    // used to black out the ENTIRE direct contribution (diffuse included) on exactly those grazing-viewed
    // slopes, which is a much harder failure than mere dimming. Only the specular denominator below needs
    // NdotV guarded against zero, and it already is (max(...,1e-4)).
    if ( NdotL <= 0.0 || attenuation <= 0.0 ) return 0.0;
    float NdotV = saturate( dot( N, V ) );
    float3 H = normalize( V + L );
    float NdotH = saturate( dot( N, H ) );
    float VdotH = saturate( dot( V, H ) );
    float  cr = PBR_SafeRoughness( roughness * roughness );   // perceptual->physical (the branch squares here)
    float  cm = saturate( metallic );
    float3 F0 = lerp( float3( 0.04, 0.04, 0.04 ), baseColor, cm );
    float  D = PBR_DistributionGGX( NdotH, cr );
    float  G = PBR_GeometrySmith( NdotV, NdotL, cr );
    float3 F = PBR_FresnelSchlick( VdotH, F0 );
    float3 specular = ( D * G * F ) / max( 4.0 * NdotV * NdotL, 1e-4 ) * specularScale;
    float3 kD = ( 1.0 - F ) * ( 1.0 - cm );
    float3 diffuse = kD * baseColor / PBR_PI;
    return ( diffuse + specular ) * lightColor * ( NdotL * attenuation );
}

// --- Sky image-based lighting (indirect diffuse + specular) -----------------------------------------------
// Consumes the two cubes built by Shaders/D3D12/SkyIbl.hlsl, whose bindless indices arrive in the ShadowCB
// sky tail (SkyIrradianceIndex / SkySpecularIndex / SkySpecularMips / SkyIblIntensity — see the CB declaration
// in the including shader). Replaces the flat greyscale ambient that used to be the ENTIRE indirect term:
// that had no specular at all, so metals (where the Cook-Torrance kD is 0) went black outside direct light,
// and roughness had no influence on ambient whatsoever.

// Karis' analytic environment BRDF ("Mobile" approximation from the UE4 course notes). Stands in for the
// split-sum DFG lookup table, which saves shipping and binding a LUT texture; the error is well under the
// precision this scene needs. Returns the (scale, bias) applied to F0.
float2 EnvBRDFApprox( float NdotV, float roughness )
{
    const float4 c0 = float4( -1.0, -0.0275, -0.572, 0.022 );
    const float4 c1 = float4( 1.0, 0.0425, 1.04, -0.04 );
    float4 r = roughness * c0 + c1;
    float a004 = min( r.x * r.x, exp2( -9.28 * NdotV ) ) * r.x + r.y;
    return float2( -1.04, 1.04 ) * a004 + r.zw;
}

// Specular occlusion (Lagarde, "Moving Frostbite to PBR"). Applying the diffuse AO term directly to indirect
// specular is wrong — it over-occludes glossy surfaces, whose reflection lobe is far narrower than the
// hemisphere the AO was integrated over. This widens the effective occlusion as roughness grows.
float ComputeSpecularOcclusion( float NdotV, float ao, float roughness )
{
    return saturate( pow( abs( NdotV + ao ), exp2( -16.0 * roughness - 1.0 ) ) - 1.0 + ao );
}

// Returns the full indirect term (diffuse + specular). `ao` is the combined diffuse occlusion
// (vertex-light AO * material AO * SSAO); the caller has already folded those together.
float3 EvaluateSkyIBL( float3 N, float3 V, float3 albedo, float roughness, float metallic, float ao )
{
    TextureCube irrTex  = ResourceDescriptorHeap[SkyIrradianceIndex];
    TextureCube specTex = ResourceDescriptorHeap[SkySpecularIndex];

    float  NdotV = saturate( dot( N, V ) );
    float3 R = reflect( -V, N );
    float  cm = saturate( metallic );
    float  cr = saturate( roughness );
    float3 F0 = lerp( float3( 0.04, 0.04, 0.04 ), albedo, cm );

    // Diffuse: irradiance * albedo, with the metal/Fresnel energy removed exactly as the direct term does.
    float3 kD = ( 1.0 - F0 ) * ( 1.0 - cm );
    float3 irradiance = irrTex.SampleLevel( smp, N, 0 ).rgb;
    float3 diffuse = irradiance * albedo * kD * ao;

    // Specular: split-sum. Perceptual roughness maps to mip via sqrt so the sharp end of the chain gets the
    // resolution it needs (a linear map spends too many mips on the mirror-like range nothing in Gothic uses).
    float  mip = sqrt( cr ) * ( SkySpecularMips - 1.0 );
    float3 prefiltered = specTex.SampleLevel( smp, R, mip ).rgb;
    float2 ab = EnvBRDFApprox( NdotV, cr );
    float3 specular = prefiltered * ( F0 * ab.x + ab.y ) * ComputeSpecularOcclusion( NdotV, ao, cr );

    // SkyIblIntensity is NOT the raw user knob — UploadSkyIblConstants premultiplies it with the ambient
    // strength and the radiance normalization, so this one factor is the complete ambient scale.
    return ( diffuse + specular ) * SkyIblIntensity;
}

// Tangent-space normal-map support (ported from feat/pbr Toolbox.h). Both conventions below are the D3D11
// path's (Shaders/Toolbox.h) verbatim — D3D12ShaderBackend injects the same two -D macros the D3D11
// ShaderRegistry's normalmappingConfigurationBuilder does, so a normal map decodes identically on both
// backends. Do not hardcode either of them here.
#ifndef NORMAL_MAP_MODE
// 1 = OpenGL (Y+)
// 2 = DirectX (Y-)
#define NORMAL_MAP_MODE 1
#endif

#ifndef NORMAL_MAP_RESTORE_Z
// NORMAL_MAP_RESTORE_Z reconstructs B from RG, which is what makes BC5 (2-channel) normal maps usable.
// 0 = sample the stored XYZ (three-channel maps only).
#define NORMAL_MAP_RESTORE_Z 1
#endif

// `p` = the surface position the derivatives are taken of. Byte-for-byte the same function as Toolbox.h's
// cotangent_frame, and it MUST be called the same way: D3D11 passes -position (see PerturbNormal below).
// The handedness comparison is the calibrated convention bias of this pipeline, not a free choice — if
// normal-mapped specular looks mirrored, flip it in BOTH files at once so the backends never drift apart.
float3x3 CotangentFrame( float3 N, float3 p, float2 uv )
{
    float3 dp1 = ddx( p ), dp2 = ddy( p );
    float2 duv1 = ddx( uv ), duv2 = ddy( uv );
    float3 dp2perp = cross( dp2, N ), dp1perp = cross( N, dp1 );
    float3 T = dp2perp * duv1.x + dp1perp * duv2.x;
    float3 B = dp2perp * duv1.y + dp1perp * duv2.y;
    float handedness = ( duv1.x * duv2.y - duv1.y * duv2.x ) < 0.0 ? 1.0 : -1.0;
    T *= handedness;
    float invmax = rsqrt( max( dot( T, T ), dot( B, B ) ) );
    return float3x3( T * invmax, B * invmax, N );
}
// strength scales the unpacked XY before Z is reconstructed — 1.0 is a full-strength normalmap; a lower value
// (e.g. the wet-ground distortion fallback in World.hlsl) flattens the perturb toward the base normal N.
// Mirrors Toolbox.h's perturb_normal_restore_z / perturb_normal_rgb (strength == normalmapDepth there).
//
// `p` is the WORLD position here, where D3D11's equivalent parameter (confusingly named `V`) is the VIEW
// position — PS_Diffuse.hlsl passes Input.vViewPosition, not a view *direction*. The space difference is
// harmless: p_world = R*p_view + camPos, so ddx/ddy differ only by the camera rotation R, N is rotated by
// the same R, and cross products commute with a proper rotation — the frame just comes out world-space,
// which is what the callers want. The SIGN is not harmless: D3D11 calls cotangent_frame( N, -V ), and
// negating p negates dp1/dp2 and therefore BOTH T and B, which is a 180-degree spin of the tangent frame
// about N (equivalent to negating nrm.xy). NORMAL_MAP_MODE flips only y and the handedness factor flips
// only T, so no combination of those two can stand in for this — pass -p, exactly like D3D11 does.
float3 PerturbNormal( float3 N, float3 p, Texture2D nrmTex, float2 uv, SamplerState samp, float strength = 1.0 )
{
#if NORMAL_MAP_RESTORE_Z == 1
    float2 nxy = nrmTex.Sample( samp, uv ).xy * 2.0 - 1.0;
  #if NORMAL_MAP_MODE == 2
    nxy.y = -nxy.y;   // flip G channel for DirectX-convention normal maps
  #endif
    nxy *= strength;
    float  nz  = sqrt( saturate( 1.0 - dot( nxy, nxy ) ) );   // reconstruct Z (BC5/BC1)
    float3 nrm = normalize( float3( nxy, nz ) );
#else
    float3 nrm = nrmTex.Sample( samp, uv ).xyz * 2.0 - 1.0;
  #if NORMAL_MAP_MODE == 2
    nrm.y = -nrm.y;
  #endif
    nrm.xy *= strength;
    nrm = normalize( nrm );
#endif
    return normalize( mul( nrm, CotangentFrame( N, -p, uv ) ) );
}

// Builds a TBN from a precomputed (MikkTSpace) vertex tangent. tangent.w is the bitangent handedness sign.
// N and tangent.xyz must be in the same space (world space here). Direct port of Toolbox.h's
// tangent_frame_explicit (D3D11's view-space equivalent) — same Gram-Schmidt re-orthonormalization, same
// single place (the caller's packed handedness sign) that controls handedness.
float3x3 TangentFrameExplicit( float3 N, float3 T, float sign )
{
    T = normalize( T - N * dot( N, T ) );
    float3 B = cross( N, T ) * sign;
    return float3x3( T, B, N );
}

// perturb_normal variant that prefers a precomputed vertex tangent and falls back to the screen-space-
// derivative CotangentFrame when the tangent is absent/degenerate (e.g. quad marks, VOBs, skeletals — none
// of which carry a packed tangent) — mirrors Toolbox.h's tangent-taking perturb_normal overload exactly,
// including its dot(T,T) < 0.25 threshold for "no real tangent here".
float3 PerturbNormal( float3 N, float3 p, float4 vertexTangent, Texture2D nrmTex, float2 uv, SamplerState samp, float strength = 1.0 )
{
    if ( dot( vertexTangent.xyz, vertexTangent.xyz ) < 0.25 )
    {
        return PerturbNormal( N, p, nrmTex, uv, samp, strength );
    }

#if NORMAL_MAP_RESTORE_Z == 1
    float2 nxy = nrmTex.Sample( samp, uv ).xy * 2.0 - 1.0;
  #if NORMAL_MAP_MODE == 2
    nxy.y = -nxy.y;
  #endif
    nxy *= strength;
    float  nz  = sqrt( saturate( 1.0 - dot( nxy, nxy ) ) );
    float3 nrm = normalize( float3( nxy, nz ) );
#else
    float3 nrm = nrmTex.Sample( samp, uv ).xyz * 2.0 - 1.0;
  #if NORMAL_MAP_MODE == 2
    nrm.y = -nrm.y;
  #endif
    nrm.xy *= strength;
    nrm = normalize( nrm );
#endif
    return normalize( mul( nrm, TangentFrameExplicit( normalize( N ), vertexTangent.xyz, vertexTangent.w ) ) );
}

// PBR sun lighting (matches DX11 lighting mix and ground/vertex lighting modulation)
float3 ComputeSunLightingPBR( float3 wpos, float3 N, float3 albedo, float vertLighting, float shadow,
                              float roughness, float metallic, float ao, float ssao )
{
    float3 V = normalize( CamPosWS - wpos );
    float3 L = SunDirWS;                            // dir toward the sun (world space)
    float3 sunCol = SrgbToLinear( SunColor );
    float  sunLum = dot( sunCol, float3( 0.3333, 0.3333, 0.3333 ) );

    // AO factors driven by Gothic's vertex/ground light
    float shadowAO = lerp( 1.0, vertLighting, ShadowAOStrength ) * ao;
    float worldAO  = lerp( 1.0, vertLighting, WorldAOStrength ) * ao;

    // Indirect (ambient) term.
    //
    // The sky-IBL path is the real one: irradiance for diffuse, a prefiltered GGX chain for specular, so metals
    // and roughness finally respond to ambient at all. It deliberately does NOT apply AmbientStrength: its
    // whole scale (ShadowStrength, unhalved at night, x the radiance normalization x the SkyIblIntensity user
    // knob) arrives premultiplied in SkyIblIntensity from UploadSkyIblConstants — see the comment there for
    // why the night halving baked into AmbientStrength must not reach this branch.
    //
    // The `else` branch is the historic flat ambient floor — matched to D3D11's reference formula
    // (PS_DS_AtmosphericScattering.hlsl litPixel), which has no surface-normal/up-direction term at all. It is
    // still the fallback for indoor worlds, a failed/disabled IBL, and SkyIblIntensity == 0. Note that a prior
    // attempt to add directionality HERE — a hemispheric saturate(N.y*0.5+0.5) factor — had to be reverted: it
    // zeroed the term for downward-facing normals and halved it for sideways ones, blacking out cave
    // ceilings/walls and tree-canopy undersides to literal (0,0,0). The IBL above is the correct fix for that,
    // because its below-horizon hemisphere carries a real ground-bounce colour for those normals to catch.
    float3 ambientSun;
    if ( SkySpecularIndex != 0xFFFFFFFFu )
    {
        ambientSun = EvaluateSkyIBL( N, V, albedo, roughness, metallic, shadowAO * ssao );

        // SKY VISIBILITY. The IBL is the OPEN SKY's radiance, so it must not reach anything the sky cannot
        // see. Measured in-game: with this off, a cave at noon is lit to daylight and only goes dark at night
        // (the sky cubes collapse after dusk) — the classic "sun bleeds into caves" report.
        //
        // shadowAO above cannot do this job: it is lerp(1, vertLighting, ShadowAOStrength), which FLOORS at
        // 1-ShadowAOStrength (0.5 by default), so a pitch-black cave still caught half the daytime sky. This
        // is the same baked-vertex-light signal applied at its own full strength instead.
        //
        // The zBSP_MODE_INDOOR overrides in D3D12ShadowMap::UploadSampling / RenderSkyIBL do not help here:
        // that flag is a whole-WORLD dungeon-level marker, and Gothic's caves and portal rooms live inside
        // OUTDOOR-mode worlds, so it never fires for them.
        //
        // Applied ONLY to this branch. The flat fallback below already carries vertLighting through shadowAO
        // and is D3D11's formula verbatim (ForwardPlusLighting.hlsl FP_ComputeSunLighting) — gating it too
        // would apply the baked light twice and push interiors darker than the D3D11 reference.
        ambientSun *= lerp( 1.0, vertLighting, SkyOccStrength );
    }
    else
        ambientSun = albedo * AmbientStrength * sunLum * shadowAO * ssao;

    // Direct Sun term. PBR_DirectLighting saturates dot(N,L) and folds it in ONCE itself (see its final
    // `NdotL * attenuation`) — `attenuation` here must therefore be everything EXCEPT N.L (shadow/AO/
    // intensity only), or the cosine term gets applied twice (NdotL^2). That used to be exactly the bug:
    // `sun = NdotL * shadow` was folded into this attenuation, squaring N.L inside PBR_DirectLighting.
    // NdotL^2 vs NdotL is close to identity at grazing-to-normal incidence (NdotL~1) but crushes moderately
    // sun-facing slopes hard (e.g. NdotL 0.75 -> 0.56, a 25% cut on top of an already-dim baked-light gate) —
    // which is exactly the "sloped roofs go dark under full sun, no shadow, no normal map involved" symptom.
    float sunAtten = shadow * worldAO * SunIntensity;
    float3 directSun = PBR_DirectLighting( albedo, sunCol, N, V, L, roughness, metallic, sunAtten, SunSpecularEnabled );

    return ambientSun + directSun;
}

// Reconstructs THIS pixel's cluster Z slice from its own hardware depth (reversed-Z), analytically — no depth-
// buffer fetch (unlike the old per-tile scheme, which sampled a prepass depth texture in the cull compute pass).
// ProjA/ProjB invert the reversed-Z depth to a linear view-space Z (same formula as LightCull.hlsl's CullCB);
// NearZ/FarZ bound the log-distributed slice range LightCull.hlsl's CSMain used to build the cluster grid — the
// two sides MUST agree on NearZ/FarZ/NUM_Z_SLICES or a pixel picks a different cluster than the one culled for it.
uint ComputeZSlice( float hwDepth )
{
    float d = max( hwDepth, 1e-6 );   // guard: hwDepth==0 (cleared/sky) would divide-by-zero below
    float viewZ = ProjB / ( d - ProjA );
    float t = log2( max( viewZ, NearZ ) / NearZ ) / log2( FarZ / NearZ );
    return (uint)clamp( floor( t * (float)NUM_Z_SLICES ), 0.0, (float)( NUM_Z_SLICES - 1 ) );
}

// Applies one light's contribution (direct BRDF + its shadow, if any) and folds it into `total`/`maxLit`.
void ApplyTiledLight( uint lightIndex, float3 wpos, float3 N, float3 V, float3 albedo, float roughness,
                       float metallic, inout float3 total, inout float3 maxLit )
{
    GPULight L = Lights[lightIndex];
    float3 dir = L.PositionWorld - wpos;
    float dist = length( dir );
    if ( dist >= L.Range ) return;
    dir /= dist;
    float nd  = saturate( 1.0 - dist / L.Range );
    float falloff = nd * ( nd * 0.2 + 0.8 );   // PLS_ComputeRangeFalloff
    // Color.w is the 0/1 "not static" flag BuildFrameLightBuffer packs (D3D12Scene.cpp), fed straight in as the
    // specular scale: an isStatic() zCVobLight is one of the "atmospheric" fill lights Gothic pre-places to
    // brighten a room, not a physical source, so it must contribute diffuse only — a highlight from it reads as a
    // phantom lamp. Same suppression D3D11 does with light.Color.w.
    float3 lit = PBR_DirectLighting( albedo, L.Color.rgb, N, V, dir, roughness, metallic, falloff, L.Color.w );
    if ( L.ShadowCubeIndex >= 0 )
    {
        // ShadowOrigin/ShadowRange, not PositionWorld/Range: a clustered static light samples a cube
        // centred on its cluster, while its own position/range still drive the shading above.
        float sh = SamplePointShadow( L.ShadowCubeIndex, wpos, N, L.ShadowOrigin, L.ShadowRange );
        float camDist = length( L.PositionView );
        float fade    = saturate( ( camDist - L.Range * 6.0 ) / ( L.Range * 3.0 ) );
        lit *= lerp( sh, 1.0, fade );
    }
    total += lit;
    maxLit = max( maxLit, lit );
}

// Accumulate dynamic point lights at a world-space surface point using the CLUSTERED Forward+ grid: derive the
// cluster from SV_Position.xy (screen tile) and SV_Position.z (Z slice, via ComputeZSlice), read its
// MAX_ACTIVE_LIGHTS-bit membership mask, and bit-scan ONLY the lights it flags (indices into the global Lights
// buffer — bit i is light i, see MAX_ACTIVE_LIGHTS). `svpos` is the shader's SV_Position (xy = screen pixel,
// z = hardware depth). `albedo` is LINEAR (sRGB-decoded in the PS). Ports D3D11 feat/pbr
// FP_ComputePointLighting (PLS_ComputePointLightLightingPBR): range cull, falloff = nd*(nd*0.2+0.8), full
// Cook-Torrance BRDF per light, additive. Bit-scanning a fixed-width mask (rather than looping an
// {Offset,Count} index slice) can never spin away on a garbage grid entry — the loop bound is the popcount of
// the mask, not an unclamped Count. Most words are 0 for a typical cluster, so the per-word while() is cheap.
float3 AccumTiledPointLights( float3 svpos, float3 wpos, float3 N, float3 albedo, float roughness, float metallic )
{
    uint2 tile = uint2( svpos.xy ) / TILE_SIZE;
    uint  tileIndex = tile.y * NumTilesX + tile.x;
    uint  slice = ComputeZSlice( svpos.z );
    // Index the fields directly rather than `LightGrid g = LightGridBuf[c]` — binding the whole struct loads all
    // MASK_WORDS+1 words eagerly, which is exactly the cost WordOccupancy exists to avoid.
    const uint cluster = tileIndex * NUM_Z_SLICES + slice;
    float3 V = normalize( CamPosWS - wpos );
    float3 total = 0;
    float3 maxLit = 0;
    // Walk only the mask words WordOccupancy flags as non-empty (see ForwardPlusTypes.hlsl). An empty cluster —
    // the common case — is now one load instead of 32, and since the light list is sorted nearest-first the set
    // bits bunch into the low words, so even a lit cluster usually touches 1-2. The loop bound is still a
    // popcount, so a corrupt entry bounds at MASK_WORDS iterations exactly as the old fixed loop did.
    uint wm = LightGridBuf[cluster].WordOccupancy;
    while ( wm != 0 )
    {
        uint w = firstbitlow( wm );
        wm &= wm - 1;
        uint m = LightGridBuf[cluster].Mask[w];
        while ( m != 0 )
        {
            uint bit = firstbitlow( m );
            m &= m - 1;   // clear the lowest set bit
            ApplyTiledLight( w * 32u + bit, wpos, N, V, albedo, roughness, metallic, total, maxLit );
        }
    }
    // LimitLightIntensity: swap "sum of every light" for "brightest single light" (mirrors D3D11's
    // ForwardPlusLighting.hlsl / CS_TiledShading.hlsl MAX-blend mode) to avoid overexposure where many
    // point lights overlap.
    return LimitLightIntensity != 0 ? maxLit : total;
}
