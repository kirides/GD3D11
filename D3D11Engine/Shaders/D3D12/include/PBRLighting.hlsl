#pragma once

// Shared Forward+ lighting math for World.hlsl / Vob.hlsl / Skeletal.hlsl — previously triplicated verbatim
// across all three; now a single source of truth. Ported from the D3D11 feat/pbr branch (Shaders/include/
// PointLightShadows.h): Cook-Torrance GGX PBR, CSM sun shadow, point-light shadow cubes, and the tiled
// point-light accumulator.
//
// #include this AFTER the including shader has already declared (with its own, per-shader register slots):
//   #include "include/ForwardPlusTypes.hlsl"   (GPULight, LightGrid, TILE_SIZE, MAX_LIGHTS_PER_TILE, NUM_CSM_CASCADES)
//   cbuffer FogCB    { ...; float3 CamPosWS; ...; }
//   cbuffer LightCB  { uint LightCount; uint NumTilesX; uint LimitLightIntensity; ...; }
//   cbuffer ShadowCB { float4x4 CascadeViewProj[NUM_CSM_CASCADES]; float3 SunDirWS; float ShadowMapSize;
//                      float3 SunColor; float SunIntensity; float3 CascadeTexelWorld; float AmbientStrength;
//                      float ShadowAOStrength; float WorldAOStrength; ...; }
//   StructuredBuffer<GPULight> Lights; StructuredBuffer<LightGrid> LightGridBuf; StructuredBuffer<uint> LightIndexBuf;
//   Texture2DArray ShadowMap; SamplerComparisonState shadowCmp; TextureCubeArray PointShadowCubes;
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
float SamplePointShadow( int cubeIndex, float3 wpos, float3 N, float3 lightPos, float range )
{
    float3 d  = ( wpos + N * ( range * 0.01 ) ) - lightPos;   // normal-offset bias (world-space, uniform)
    float3 ad = abs( d );
    float  zView = max( ad.x, max( ad.y, ad.z ) );            // dominant cube-axis depth = the face's view-space z
    const float n = 15.0;
    float  f = range * 2.0;
    float  compareDepth = ( f / ( f - n ) ) * ( 1.0 - n / zView ) - 0.001;   // same LH hyperbolic z the caster wrote
    float3 L = normalize( d );

    // P2.10e polish: 4-tap rotated-disk PCF on a basis perpendicular to L (cube sampling follows the offset dir,
    // so a small angular offset lands on neighbouring texels). Softens the previously single-tap hard edges. The
    // offset grows a little with distance so the world-space penumbra stays roughly constant across the range.
    float3 up = abs( L.y ) < 0.99 ? float3( 0, 1, 0 ) : float3( 1, 0, 0 );
    float3 t  = normalize( cross( up, L ) );
    float3 bt = cross( L, t );
    float  r  = 0.006 + 0.010 * saturate( zView / f );
    static const float2 kDisk[4] = { float2( 0.7, 0.7 ), float2( -0.7, 0.7 ), float2( 0.7, -0.7 ), float2( -0.7, -0.7 ) };
    float sh = 0.0;
    [unroll]
    for ( int s = 0; s < 4; ++s )
    {
        float3 o = normalize( L + ( kDisk[s].x * t + kDisk[s].y * bt ) * r );
        sh += PointShadowCubes.SampleCmpLevelZero( shadowCmp, float4( o, (float)cubeIndex ), compareDepth );
    }
    return sh * 0.25;
}

// Returns 1.0 = fully lit, 0.0 = fully occluded. Picks the first cascade whose footprint contains the point
// (0 tightest), applies a per-cascade world-space normal bias, and does a PCF tap. Mirrors the D3D11
// ComputeCascadedShadowValueSoft selection/bounds (GetCascadeUVAndBounds) minus the blue-noise/PCSS machinery.
//
// Bias matches D3D11's scheme (Shaders/ShadowSampling.h ComputeCascadedShadowValueSoft + its PS_Diffuse/
// PS_FP_ShadowMask/PS_DS_AtmosphericScattering call sites): the real bias is the world-space normal offset,
// SLOPE-SCALED so it's ~0 for a surface facing the sun and only grows at grazing angles — a flat offset (as
// this used to be) over-biases front-facing ground/wall contacts and detaches the shadow from its caster
// (peter-panning). The NDC-depth compare bias is a tiny constant (D3D11: 0.000003) only meant to fight
// self-shadow z-fighting at zero slope — NOT a general-purpose bias. This cascade's ortho depth range
// (orthoFar-orthoNear, see ComputeCascadeMatrices) commonly spans several thousand world units, so the old
// 0.0015 constant here was ~500x too large — worth 10+ world units of erroneous depth offset on its own.
float ComputeSunShadow( float3 wpos, float3 N )
{
    const float margin = 1.5 / ShadowMapSize;
    const float texel  = 1.0 / ShadowMapSize;
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
            // Wider, cascade-scaled PCF (P2.9c-3c): far cascades cover more world per texel (sub-texel foliage
            // → temporal "blinking"), so widen the kernel step with the cascade index to spatially average that
            // flicker into a soft, stable penumbra. 5x5 taps; near cascade stays near-1-texel (crisp).
            float pcfStep = texel * ( 1.0 + float( c ) * 1.5 );
            float sh = 0.0;
            [unroll] for ( int y = -2; y <= 2; ++y )
            [unroll] for ( int x = -2; x <= 2; ++x )
                sh += ShadowMap.SampleCmpLevelZero( shadowCmp, float3( uv + float2( x, y ) * pcfStep, c ), sp.z - constantDepthBias );
            return sh / 25.0;
        }
    }
    return 1.0;   // outside all cascades → treat as lit
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
float3 PBR_DirectLighting( float3 baseColor, float3 lightColor, float3 N, float3 V, float3 L,
                           float roughness, float metallic, float attenuation )
{
    float NdotL = saturate( dot( N, L ) );
    float NdotV = saturate( dot( N, V ) );
    if ( NdotL <= 0.0 || NdotV <= 0.0 || attenuation <= 0.0 ) return 0.0;
    float3 H = normalize( V + L );
    float NdotH = saturate( dot( N, H ) );
    float VdotH = saturate( dot( V, H ) );
    float  cr = PBR_SafeRoughness( roughness * roughness );   // perceptual->physical (the branch squares here)
    float  cm = saturate( metallic );
    float3 F0 = lerp( float3( 0.04, 0.04, 0.04 ), baseColor, cm );
    float  D = PBR_DistributionGGX( NdotH, cr );
    float  G = PBR_GeometrySmith( NdotV, NdotL, cr );
    float3 F = PBR_FresnelSchlick( VdotH, F0 );
    float3 specular = ( D * G * F ) / max( 4.0 * NdotV * NdotL, 1e-4 );
    float3 kD = ( 1.0 - F ) * ( 1.0 - cm );
    float3 diffuse = kD * baseColor / PBR_PI;
    return ( diffuse + specular ) * lightColor * ( NdotL * attenuation );
}

// Tangent-space normal-map support (ported from feat/pbr Toolbox.h). Z is ALWAYS reconstructed from XY, so BC5
// (2-channel) and BC1 (we ignore B, recompute it) both decode with one path. `p` = world position for the
// derivative-based TBN basis. If normal-mapped specular looks mirrored, flip the handedness comparison sign.
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
float3 PerturbNormal( float3 N, float3 p, Texture2D nrmTex, float2 uv, SamplerState samp )
{
    float2 nxy = nrmTex.Sample( samp, uv ).xy * 2.0 - 1.0;
    nxy.y = -nxy.y;
    float  nz  = sqrt( saturate( 1.0 - dot( nxy, nxy ) ) );   // reconstruct Z (BC5/BC1)
    float3 nrm = normalize( float3( nxy, nz ) );
    return normalize( mul( nrm, CotangentFrame( N, p, uv ) ) );
}

// PBR sun lighting (matches DX11 lighting mix and ground/vertex lighting modulation)
float3 ComputeSunLightingPBR( float3 wpos, float3 N, float3 albedo, float vertLighting, float shadow,
                              float roughness, float metallic, float ao )
{
    float3 V = normalize( CamPosWS - wpos );
    float3 L = SunDirWS;                            // dir toward the sun (world space)
    float3 sunCol = SrgbToLinear( SunColor );
    float  sunLum = dot( sunCol, float3( 0.3333, 0.3333, 0.3333 ) );
    const float ssao = 1.0;

    // Direct sun term N.L
    float NdotL = saturate( dot( N, L ) );
    float sun = NdotL * shadow;

    // AO factors driven by Gothic's vertex/ground light
    float shadowAO = lerp( 1.0, vertLighting, ShadowAOStrength ) * ao;
    float worldAO  = lerp( 1.0, vertLighting, WorldAOStrength ) * ao;

    // Directional sky/ambient term: top-facing gets full sky ambient, vertical/undersides darken
    float skyAmbientDir = saturate( N.y * 0.5 + 0.5 ); // Hemispheric directional ambient factor
    float3 ambientSun = albedo * AmbientStrength * sunLum * shadowAO * skyAmbientDir * ssao;

    // Direct Sun term
    float sunAtten = sun * worldAO * SunIntensity;
    float3 directSun = PBR_DirectLighting( albedo, sunCol, N, V, L, roughness, metallic, sunAtten );

    return ambientSun + directSun;
}

// Accumulate dynamic point lights at a world-space surface point using the Forward+ tile grid: derive the
// tile from SV_Position.xy, read its {Offset,Count} slice, and loop ONLY the lights culled into that tile
// (indices into the global Lights buffer). `albedo` is LINEAR (sRGB-decoded in the PS). Ports D3D11 feat/pbr
// FP_ComputePointLighting (PLS_ComputePointLightLightingPBR): range cull, falloff = nd*(nd*0.2+0.8), full
// Cook-Torrance BRDF per light, additive. The count is clamped to the per-tile capacity so a garbage grid
// entry can never spin the loop away (that reads as a GPU timeout).
float3 AccumTiledPointLights( float2 svpos, float3 wpos, float3 N, float3 albedo, float roughness, float metallic )
{
    uint2 tile = uint2( svpos ) / TILE_SIZE;
    uint  tileIndex = tile.y * NumTilesX + tile.x;
    LightGrid g = LightGridBuf[tileIndex];
    uint n = min( g.Count, MAX_LIGHTS_PER_TILE );
    float3 V = normalize( CamPosWS - wpos );
    float3 total = 0;
    float3 maxLit = 0;
    for ( uint k = 0; k < n; k++ )
    {
        GPULight L = Lights[ LightIndexBuf[g.Offset + k] ];
        float3 dir = L.PositionWorld - wpos;
        float dist = length( dir );
        if ( dist >= L.Range ) continue;
        dir /= dist;
        float nd  = saturate( 1.0 - dist / L.Range );
        float falloff = nd * ( nd * 0.2 + 0.8 );   // PLS_ComputeRangeFalloff
        float3 lit = PBR_DirectLighting( albedo, L.Color.rgb, N, V, dir, roughness, metallic, falloff );
        if ( L.ShadowCubeIndex >= 0 )
        {
            float sh = SamplePointShadow( L.ShadowCubeIndex, wpos, N, L.PositionWorld, L.Range );
            float camDist = length( L.PositionView );
            float fade    = saturate( ( camDist - L.Range * 6.0 ) / ( L.Range * 3.0 ) );
            lit *= lerp( sh, 1.0, fade );
        }
        total += lit;
        maxLit = max( maxLit, lit );
    }
    // LimitLightIntensity: swap "sum of every light" for "brightest single light" (mirrors D3D11's
    // ForwardPlusLighting.hlsl / CS_TiledShading.hlsl MAX-blend mode) to avoid overexposure where many
    // point lights overlap.
    return LimitLightIntensity != 0 ? maxLit : total;
}
