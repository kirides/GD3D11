// D3D12 water surfaces — port of the D3D11 spec pair VS_ExWater.hlsl + PS_Water.hlsl.
//
// The MVP version of this shader was a flat alpha-blended texture lookup: no refraction, no reflection,
// and translucency faked with a constant `WaterAlpha` on an SRC_ALPHA/INV_SRC_ALPHA blend. That reads as
// "too solid" because real water here is NOT a blended surface at all — D3D11 draws water OPAQUE
// (GothicBlendStateInfo::SetDefault leaves BlendEnabled = false) and does the see-through part itself, by
// sampling a *copy* of the finished scene through a distorted UV. That is what makes shallow water show
// the ground beneath it, deep water go dark, and the surface pick up sky/geometry reflections.
//
// Ported wholesale from PS_Water.hlsl:
//   * refraction        — the scene copy sampled through the two-octave distortion normal
//   * shallow/deep      — depth copy vs. the surface's own view-space Z drives darkening + border cleanup
//   * sky reflection    — the static reflection cube (reflect_cube.dds) as a TextureCube
//   * SSR               — TraceWaterSSR marches the reflection ray against the depth copy
//   * sun spot + atmospheric scattering + the day/night darkness factor
//
// Two deliberate deviations from D3D11:
//   1. SSR quality is a RUNTIME uniform (SsrMaxSteps/SsrRefineSteps from the CB), not the SSR_QUALITY
//      compile-time permutation. D3D12 shaders are baked at Init() and this backend has no live shader
//      reload yet (see D3D12GraphicsEngine::ReloadShaders), so a macro permutation would need a restart
//      to change; the loop bounds are uniform across the draw, so the branch is free.
//   2. Every screen-space input (scene copy, depth copy, distortion, reflection cube) is fetched
//      BINDLESSLY via SM6.6 ResourceDescriptorHeap instead of fixed t2..t5 slots. Only the per-material
//      diffuse still rides a descriptor table, because the color loop rebinds it per texture batch.
//
// Not ported: the SHD_WATERANI Gerstner wave displacement in VS_ExWater (vertex-level wave offset). The
// surface is still geometrically flat; all the wave *shading* below comes from the distortion texture.
//
// D3D12-only addition (no D3D11 equivalent — D3D11 has no DXR path): when an SSR ray misses, TraceWater
// ReflectionRT below fires one DXR 1.1 inline ray query (RayQuery/TraceRayInline) against a BLAS/TLAS built
// from the world mesh, so off-screen geometry can still tint the reflection instead of always falling back
// to the flat static sky cube. See that function's comment for the PoC-level scope of its shading.

#include "include/AtmosphericScattering.hlsl"   // ApplyAtmosphericScatteringGround + the Atmosphere cbuffer (b1)
#include "../include/MathHelpers.hlsl"

cbuffer WorldCB : register(b0) { float4x4 ViewProj; };   // root constants, VS only

cbuffer WaterCB : register(b2)
{
    float4x4 RI_Projection;      // Gothic's projection matrix, uploaded verbatim (mul(v, M) == M*v, see below)
    float4x4 RI_View;            // world->view, likewise verbatim — same as D3D11's RefractionInfo.RI_View

    float2 RI_ViewportSize;
    float  RI_Time;              // seconds, drives the distortion scroll (D3D11: GetTimeSeconds)
    float  RI_TotalTime;         // milliseconds, drives the per-material UV scroll (D3D11: M_TotalTime)

    float3 RI_CameraPosition;
    float  RI_ProjA;             // == HLSL RI_Projection._33 — see the depth note below
    float  RI_ProjB;             // == HLSL RI_Projection._43
    uint   DepthIndex;           // R32_FLOAT SRV of the pre-water depth copy
    uint   SceneIndex;           // HDR SRV of the pre-water scene-color copy
    uint   DistortionIndex;      // distortion2.dds

    uint   ReflectionCubeIndex;  // reflect_cube.dds as a TextureCube (0xFFFFFFFF = unavailable)
    uint   SsrMaxSteps;          // 0 disables SSR entirely (WATER_SSR_DISABLED)
    uint   SsrRefineSteps;
    uint   UseAtmosphere;        // 0 when GSky had no atmosphere data this frame (skip scattering)

    // --- Raytraced reflection PoC (D3D12GraphicsEngine::EnsureWaterReflectionAS) ---
    uint   TlasIndex;            // RAYTRACING_ACCELERATION_STRUCTURE SRV over the world BLAS; 0xFFFFFFFF = RT path off
    uint   WorldVbIndex;         // raw ByteAddressBuffer SRV, same world VB the BLAS was built from (ExVertexStructGPU, 36 B stride)
    uint   WorldIbIndex;         // raw ByteAddressBuffer SRV, same world IB (R32_UINT)
    uint   _Pad0;
};

Texture2D    tx  : register(t0);   // per-material diffuse (descriptor table — rebound per texture batch)
SamplerState smp : register(s0);   // linear WRAP  — diffuse + the world-space distortion lookups
SamplerState smpClamp : register(s1); // linear CLAMP — screen-space fetches (scene copy, depth copy)

static const float DIST_SMALL_SPEED  = -0.01f;
static const float DIST_SMALL_AMOUNT = 0.01f;
static const float DIST_SMALL_SCALE  = 0.3f;
static const float DIST_BIG_SCALE    = 0.1f;
static const float DIST_BIG_SPEED    = -0.005f;

// Cleans the refraction borders (verbatim from PS_Water.hlsl's CleanRefraction macro, expanded to a
// function because the D3D11 version reaches into `Input` from macro scope).
float2 CleanRefraction( float2 uv, float2 screenUV, float depthRef, float surfaceViewZ )
{
    return lerp( uv, screenUV, saturate( surfaceViewZ - depthRef ) );
}

// Octahedral normal decode — matches Shaders/VertexPacking.h DecodeOctNormal (the packed 36-byte world
// vertex stores its world-space normal as an R16G16_SNORM octahedral pair).
float3 DecodeOctNormal( float2 e )
{
    float3 n = float3( e.xy, 1.0 - abs( e.x ) - abs( e.y ) );
    float t = saturate( -n.z );
    n.xy += select( n.xy >= 0.0, -t, t );
    return normalize( n );
}

float3 SrgbToLinear( float3 c )   // accurate sRGB EOTF — the diffuse texture is gamma-encoded, the RT is linear
{
    return select( c <= 0.04045, c / 12.92, pow( ( c + 0.055 ) / 1.055, 2.4 ) );
}

struct VS_IN  { float3 pos : POSITION; float2 nrm : NORMAL; float2 uv : TEXCOORD0; float2 scroll : TEXCOORD1; float4 col : DIFFUSE; };
struct VS_OUT
{
    float4 clip    : SV_POSITION;
    float2 uv      : TEXCOORD0;
    float2 vz      : TEXCOORD1;   // .x = view-space Z, .y = view-space distance (D3D11's vTexcoord2)
    float4 col     : TEXCOORD2;
    float3 wnrm    : TEXCOORD3;   // geometric world normal — waterfall detection only
    float3 wpos    : TEXCOORD4;
};

VS_OUT VSMain( VS_IN i )
{
    VS_OUT o;
    o.clip = mul( float4( i.pos, 1.0 ), ViewProj );
    float2 ani = i.scroll * RI_TotalTime;   // scroll delta (TexCoord2) * total time (ms), like VS_ExWater
    ani -= floor( ani );                    // wrap to [0,1) so the float stays precise over long sessions
    o.uv = i.uv + ani;
    o.col = i.col;
    o.wnrm = DecodeOctNormal( i.nrm );      // already world-space
    o.wpos = i.pos;
    // mul(v, M) with a verbatim-uploaded Gothic matrix evaluates to M*v (see CLAUDE.md's column-major
    // note) — the same convention D3D11's PS_Water/VS_ExWater rely on.
    o.vz.x = mul( float4( i.pos, 1.0 ), RI_View ).z;
    o.vz.y = length( mul( float4( i.pos, 1.0 ), RI_View ) );
    return o;
}

// --- Depth-only prepass (mirrors D3D11's "DrawWaterSurfaces::ZPrepass": same VS transform, null PS,
// color writes masked off, depth-write ON). Water is drawn depth-read-only in the color pass, so without
// this the main depth buffer keeps the OPAQUE geometry behind the water surface (or the far plane over
// open ocean) — and every later pass that reconstructs world position from depth (height fog, god rays)
// then fogs the sea floor / sky instead of the water surface. Also makes overlapping water surfaces blend
// once instead of stacking, since the color pass's GREATER_EQUAL test now only passes on the nearest layer.
// No alpha clip here (unlike the world/VOB prepass): water opacity is decided by the refraction math in
// the PS, not by the diffuse texture's alpha, so clipping on it would punch holes into the depth.
//
// The prepass PSO reuses **VSMain verbatim** (like D3D11 reuses VS_ExWater for its Z-prepass) rather than a
// leaner position-only VS. That is deliberate and load-bearing: a separate VS is only *algebraically* equal,
// and DXC/the driver may emit a different instruction sequence for the same matrix multiply, so the two
// passes can disagree by an ULP. Gothic's water is full of coplanar/near-coplanar overlapping surfaces (two
// water materials meeting, double-sided quads), and a 1-ULP disagreement there makes the color pass'
// GREATER_EQUAL test reject the layer the prepass accepted on some pixels but not others — i.e. exactly the
// z-fighting/shimmering patchwork of stacked water textures. Same VS => bit-identical depth => coplanar
// layers all compare EQUAL and blend, stable. Only the PS below is swapped out.
void PSDepth( float4 clip : SV_POSITION ) {}   // subset of VS_OUT's signature; writes nothing

//--------------------------------------------------------------------------------------
// Depth helpers
//
// The main camera writes reversed-Z with an infinite far plane, so linear view Z is
// RI_ProjB / (raw - RI_ProjA). Gothic pins those to 1.0 / 0.0 (i.e. plain rcp(raw)), but the two values
// are uploaded rather than baked so this stays exactly D3D11's `RI_Projection._43 / (d - _33)` formula.
//--------------------------------------------------------------------------------------
float LinearizeWaterDepth( float raw ) { return RI_ProjB / ( raw - RI_ProjA ); }

float SampleSceneDepth( float2 uv )
{
    Texture2D<float> depthTex = ResourceDescriptorHeap[DepthIndex];
    return LinearizeWaterDepth( depthTex.SampleLevel( smpClamp, uv, 0 ).r );
}

// Point-sample (Load), never bilinear: at silhouette edges of thin geometry (masts, poles) bilinear
// filtering blends foreground and far-background raw depth into a phantom Z that matches no real surface.
// The ray "hits" that phantom depth and then samples the bright sky behind the edge -> sparse blue/white
// speckles. Nearest-texel depth removes those false intersections. (Verbatim reasoning from PS_Water.)
float SSR_SceneZ( float2 uv )
{
    Texture2D<float> depthTex = ResourceDescriptorHeap[DepthIndex];
    int2 px = clamp( int2( uv * RI_ViewportSize ), int2( 0, 0 ), int2( RI_ViewportSize ) - 1 );
    return LinearizeWaterDepth( depthTex.Load( int3( px, 0 ) ).r );
}

//--------------------------------------------------------------------------------------
// Screen-space reflections — a direct port of PS_Water.hlsl's TraceWaterSSR. Marches the (wave-perturbed)
// reflection ray in view space against the copied scene depth and returns the scene color at the hit. On
// a miss the confidence is 0 so the caller falls back to the static reflection cube.
//--------------------------------------------------------------------------------------
#define SSR_MAX_DISTANCE    30000.0f  // view-space units the ray may travel
#define SSR_THICKNESS       350.0f    // max depth gap that still counts as a hit
#define SSR_START_BIAS      2.0f      // push off the surface to avoid self-intersection

bool SSR_ProjectToUV( float3 posVS, out float2 uv )
{
    float4 clip = mul( float4( posVS, 1.0f ), RI_Projection );
    if ( clip.w <= 0.0f ) { uv = float2( 0.0f, 0.0f ); return false; }
    uv = ( clip.xy / clip.w ) * float2( 0.5f, -0.5f ) + 0.5f;
    return true;
}

float3 TraceWaterSSR( float3 worldPos, float3 reflectDirWS, out float confidence )
{
    confidence = 0.0f;

    float3 originVS = mul( float4( worldPos, 1.0f ), RI_View ).xyz;
    float3 dirVS = normalize( mul( float4( reflectDirWS, 0.0f ), RI_View ).xyz );

    // Uniform march; binary search recovers precision at the hit.
    const float stepLen = SSR_MAX_DISTANCE / (float)SsrMaxSteps;
    float startBias = max( SSR_START_BIAS, originVS.z * 0.002f );

    float3 prevPos = originVS + dirVS * startBias;
    float2 prevUV;
    if ( !SSR_ProjectToUV( prevPos, prevUV ) )
        return float3( 0.0f, 0.0f, 0.0f );
    // delta < 0 => ray is in front of the scene surface at this pixel. Sky/far pixels have a huge sceneZ,
    // so delta stays very negative there.
    float prevDelta = prevPos.z - SSR_SceneZ( prevUV );
    float travelled = startBias;

    [loop]
    for ( uint i = 0; i < SsrMaxSteps; ++i )
    {
        float3 curPos = prevPos + dirVS * stepLen;
        travelled += stepLen;

        float2 uv;
        if ( !SSR_ProjectToUV( curPos, uv ) )
            return float3( 0.0f, 0.0f, 0.0f ); // behind camera -> fall back to cube
        if ( any( uv < 0.0f ) || any( uv > 1.0f ) )
            return float3( 0.0f, 0.0f, 0.0f ); // left the screen -> fall back to cube

        float sceneZ = SSR_SceneZ( uv );
        float curDelta = curPos.z - sceneZ;

        // Front -> behind crossing between prevPos and curPos: we hit a surface.
        //
        // ...but only if the surface sits at or behind where the ray was already in front (prevPos.z). A
        // genuine continuous surface satisfies sceneZ >= prevPos.z. If curUV's sceneZ is much NEARER than
        // prevPos.z, the screen-space ray merely swept BEHIND a foreground silhouette (e.g. the player
        // standing between the water and the far shore): sceneZ teleports from far-background to
        // near-player, firing a false crossing. Rejecting these (and continuing the march) stops the
        // player's dark silhouette from smearing into the water. This must gate the crossing itself, not
        // the post-refine gap, which binary search always shrinks.
        if ( prevDelta < 0.0f && curDelta >= 0.0f && sceneZ >= prevPos.z - SSR_THICKNESS )
        {
            // Binary-search refine between prevPos (in front) and curPos (behind).
            float3 lo = prevPos;
            float3 hi = curPos;
            float2 hitUV = uv;
            float hitGap = curDelta;
            [loop]
            for ( uint j = 0; j < SsrRefineSteps; ++j )
            {
                float3 mid = ( lo + hi ) * 0.5f;
                float2 midUV;
                if ( !SSR_ProjectToUV( mid, midUV ) )
                    break;
                float midGap = mid.z - SSR_SceneZ( midUV );
                if ( midGap >= 0.0f ) { hi = mid; hitUV = midUV; hitGap = midGap; }
                else                  { lo = mid; }
            }

            // After refinement a real surface converges to a small residual gap. A large residual means
            // the ray passed behind a thin object into empty space (its far side); reject so we don't
            // smear background over water.
            if ( hitGap < SSR_THICKNESS )
            {
                // Fade only in the outermost sliver near the screen borders (where the reflected data
                // genuinely runs out), plus at the end of the ray.
                float2 edge = smoothstep( 0.0f, 0.001f, hitUV ) * smoothstep( 0.0f, 0.001f, 1.0f - hitUV );
                float edgeFade = edge.x * edge.y;
                float distFade = saturate( 1.0f - travelled / SSR_MAX_DISTANCE );

                confidence = edgeFade * distFade;
                Texture2D sceneTex = ResourceDescriptorHeap[SceneIndex];
                return sceneTex.SampleLevel( smpClamp, hitUV, 0 ).rgb;
            }
        }

        prevPos = curPos;
        prevDelta = curDelta;
    }

    return float3( 0.0f, 0.0f, 0.0f ); // nothing hit -> fall back to cube
}

//--------------------------------------------------------------------------------------
// Raytraced reflection — proof of concept for DXR 1.1 inline ray tracing (RayQuery/TraceRayInline).
//
// TraceWaterSSR above only ever sees what already made it onto the screen this frame; a reflection ray
// that leaves the viewport, or one that never crosses the copied depth buffer at all (nothing between the
// water and the horizon), has no on-screen data to find and falls straight back to the flat static sky
// cube. This adds a third tier: fire ONE inline ray query against the BLAS/TLAS built from the wrapped
// world mesh (D3D12GraphicsEngine::EnsureWaterReflectionAS) and, on a hit, shade it well enough to read as
// an actual off-screen reflection rather than sky. Because it goes through RayQuery instead of the
// DispatchRays pipeline there is no hit-group/shader-table setup at all — this function is the entire
// "raytracing pipeline".
//
// Deliberately crude shading, consistent with this being a PoC: no shadows, no textures, one bounce, and
// the hit color is the interpolated vertex-color tinted by the current sun/night factor and faded by hit
// distance — not a real BRDF evaluation. Good enough to distinguish "cliff face" from "open sky" at the
// water's edge, which is the whole point: SSR's miss case goes from "always the cube" to "usually right".
//--------------------------------------------------------------------------------------
float3 TraceWaterReflectionRT( float3 worldPos, float3 reflectDirWS, out float confidence )
{
    confidence = 0.0f;
    if ( TlasIndex == 0xFFFFFFFFu )
        return float3( 0.0f, 0.0f, 0.0f );

    RaytracingAccelerationStructure tlas = ResourceDescriptorHeap[TlasIndex];

    RayDesc ray;
    ray.Origin = worldPos + reflectDirWS * 4.0f;   // push off the surface, same idea as SSR_START_BIAS
    ray.Direction = reflectDirWS;
    ray.TMin = 0.0f;
    ray.TMax = 100000.0f;

    // Every triangle in the BLAS is flagged OPAQUE at build time (EnsureWaterReflectionAS), so the query
    // never needs an any-hit shader; SKIP_PROCEDURAL_PRIMITIVES is free since the BLAS holds only triangles.
    // (There is no "RAY_FLAG_CULL_NONE" — no culling is just the absence of a cull flag, i.e. RAY_FLAG_NONE.)
    RayQuery<RAY_FLAG_NONE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    q.TraceRayInline( tlas, RAY_FLAG_NONE, 0xFF, ray );
    q.Proceed();

    if ( q.CommittedStatus() != COMMITTED_TRIANGLE_HIT )
        return float3( 0.0f, 0.0f, 0.0f );

    // Fetch the hit triangle's 3 vertices (position + vertex color) straight out of the raw world VB/IB
    // (bound bindlessly alongside the TLAS) and interpolate by the query's barycentrics — the cheapest
    // possible stand-in for a real material fetch, since there is no hit-group/closest-hit shader to do it
    // for us. The geometric (flat, non-interpolated) face normal then gives cheap Lambert shading instead
    // of a flat unlit vertex-color wash.
    ByteAddressBuffer worldIb = ResourceDescriptorHeap[WorldIbIndex];
    ByteAddressBuffer worldVb = ResourceDescriptorHeap[WorldVbIndex];

    const uint primBase = q.CommittedPrimitiveIndex() * 3u * 4u;   // R32_UINT indices, 3 per triangle
    const uint3 idx = worldIb.Load3( primBase );

    const uint kVertexStride   = 36u;   // ExVertexStructGPU (VertexTypes.h)
    const uint kPositionOffset = 0u;    // float3
    const uint kColorOffset    = 32u;   // DWORD diffuse, packed BGRA — same layout the DIFFUSE input element reads

    float3 pos[3];
    float3 col[3];
    [unroll]
    for ( uint i = 0; i < 3; ++i )
    {
        const uint base = idx[i] * kVertexStride;
        pos[i] = asfloat( worldVb.Load3( base + kPositionOffset ) );
        uint packed = worldVb.Load( base + kColorOffset );
        col[i] = float3( ( packed >> 16 ) & 0xFF, ( packed >> 8 ) & 0xFF, packed & 0xFF ) / 255.0f;   // BGRA -> RGB
    }

    float3 bary;
    bary.yz = q.CommittedTriangleBarycentrics();
    bary.x = 1.0f - bary.y - bary.z;
    float3 vcolor = col[0] * bary.x + col[1] * bary.y + col[2] * bary.z;

    // Flip the face normal to face the incoming ray if the BLAS winding puts it the other way — the world
    // mesh has no guaranteed consistent "outside" winding, and a backwards normal here would light every
    // hit as though seen from behind it (flat black).
    float3 faceNormal = normalize( cross( pos[1] - pos[0], pos[2] - pos[0] ) );
    if ( dot( faceNormal, reflectDirWS ) > 0.0f )
        faceNormal = -faceNormal;

    // Cheap directional + ambient term using the same sun direction / day-night signal PSMain's own
    // sun-spot term reads below (AC_LightPos), so RT hit shading brightens/dims in step with the rest of
    // the scene instead of reading flat-lit at night and in full sun alike.
    float3 sunDir = normalize( AC_LightPos.xyz );
    float NdotL = saturate( dot( faceNormal, sunDir ) );
    float3 ambient = lerp( float3( 0.05, 0.05, 0.07 ), float3( 0.16, 0.18, 0.22 ), saturate( AC_LightPos.y ) );
    float3 sunColor = lerp( float3( 0.6, 0.3, 0.1 ), float3( 1.0, 0.95, 0.85 ), saturate( AC_LightPos.y ) );
    float3 lighting = ambient + sunColor * NdotL;

    // Confidence alone carries the distance fade: PSMain's lerp( reflection, rtColor, confidence ) already
    // blends a far hit back towards the cube, so the color returned here must NOT also fade itself — doing
    // both would darken a moderate-range hit twice (once here, once by the caller's own blend weight).
    confidence = saturate( 1.0f - q.CommittedRayT() / 60000.0f );
    return vcolor * lighting;
}

//--------------------------------------------------------------------------------------
// Pixel Shader — line-for-line port of PS_Water.hlsl's PSMain.
//--------------------------------------------------------------------------------------
float4 PSMain( VS_OUT Input ) : SV_TARGET
{
    Texture2D sceneTex = ResourceDescriptorHeap[SceneIndex];
    Texture2D distortionTex = ResourceDescriptorHeap[DistortionIndex];

    float2 screenUV = Input.clip.xy / RI_ViewportSize;

    // Linear depth of whatever the opaque scene put behind this water pixel.
    float depth = SampleSceneDepth( screenUV );
    float shallowDepth = saturate( ( depth - Input.vz.x ) * 0.01f );

    // Camera direction
    float3 viewDirection = normalize( Input.wpos - RI_CameraPosition );

    // Calculate distortion vectors
    float2 worldTexCoord = Input.wpos.xz / 1000.0f;
    float3 distortionSmall = distortionTex.Sample( smp, worldTexCoord * DIST_SMALL_SCALE + RI_Time * DIST_SMALL_SPEED ).xyz * 2 - 1;
    distortionSmall += distortionTex.Sample( smp, worldTexCoord * float2( -1, 0.7 ) * DIST_SMALL_SCALE + RI_Time * DIST_SMALL_SPEED * 2 ).xyz * 2 - 1;
    distortionSmall *= 0.5f;

    float3 distortionBig = distortionTex.Sample( smp, worldTexCoord * DIST_BIG_SCALE + RI_Time * DIST_BIG_SPEED ).xyz * 2 - 1;
    distortionBig += distortionTex.Sample( smp, worldTexCoord * float2( -1, 0.7 ) * DIST_BIG_SCALE + RI_Time * DIST_BIG_SPEED * 1.2 ).xyz * 2 - 1;
    distortionBig *= 0.5f;

    float2 distUV = screenUV + distortionSmall.xy * DIST_SMALL_AMOUNT + distortionBig.xy * DIST_SMALL_AMOUNT;

    // Distorted diffuse (linearized — the HDR scene target is linear, unlike D3D11's gamma-space one)
    float3 diffuse = SrgbToLinear( tx.Sample( smp, Input.uv + distortionSmall.xy * DIST_SMALL_AMOUNT * 0.5f ).rgb );

    // Refracted depth
    float depthRefracted = SampleSceneDepth( distUV );

    distUV = CleanRefraction( distUV, screenUV, depthRefracted, Input.vz.x );
    distUV = saturate( distUV );

    // Wave vectors
    float3 wavesDist = normalize( distortionSmall.xzy * float3( 1, 100, 1 ) );
    float3 wavesFres = normalize( distortionBig.xzy * float3( 1, 10, 1 ) );

    // Scene color (this is what makes the water see-through — D3D11 does the same and writes opaque)
    float3 scene = sceneTex.SampleLevel( smpClamp, distUV, 0 ).rgb;
    float3 sceneClean = sceneTex.SampleLevel( smpClamp, lerp( distUV, screenUV, pow( 1 - shallowDepth, 20.0f ) ), 0 ).rgb;

    // Fresnel from waves
    float fresnel = min( 0.5f, saturate( pow( 1.0f - saturate( dot( -viewDirection, wavesFres ) ), 10.0f ) ) );

    // Reflection: the static sky/environment cube is the fallback for off-screen / missed SSR rays.
    float3 reflect_vec = reflect( -viewDirection, wavesFres );
    float3 reflection = float3( 0.0f, 0.0f, 0.0f );
    if ( ReflectionCubeIndex != 0xFFFFFFFFu )
    {
        TextureCube reflectionCube = ResourceDescriptorHeap[ReflectionCubeIndex];
        reflection = reflectionCube.Sample( smp, reflect_vec ).xyz;
    }

    // reflect_vec above is negated (reflect(-viewDirection,N)) for the cube lookup. The true
    // eye-reflection direction, which marches UP into the scene, is reflect(viewDirection, N).
    // Flatten the wave normal so reflection rays stay coherent (mirror-like) instead of scattering
    // into many off-screen misses.
    float3 ssrNormal = normalize( lerp( float3( 0.0f, 1.0f, 0.0f ), wavesFres, 0.5f ) );
    float3 ssrDir = reflect( viewDirection, ssrNormal );

    float ssrConfidence = 0.0f;
    if ( SsrMaxSteps > 0 )
    {
        float3 ssrColor = TraceWaterSSR( Input.wpos, ssrDir, ssrConfidence );
        reflection = lerp( reflection, ssrColor, saturate( ssrConfidence ) );
    }

    // Raytraced reflection PoC. SCREENSPACE and RAYTRACED modes are mutually exclusive by construction,
    // never blended per-pixel: D3D12Water.cpp's CB fill zeroes SsrMaxSteps whenever RAYTRACED is active
    // (and TlasIndex whenever it isn't), so ssrConfidence is unconditionally 0 in RT mode and this branch
    // is the ONLY reflection source. That is deliberate — a per-pixel blend still let SSR's own
    // grazing-angle smearing through on every pixel it did resolve, since the screen-space march's
    // silhouette/thickness heuristics get worse exactly at the shallow view angles reflections are most
    // visible at. The `ssrConfidence <= 0.0f` check below is therefore just "is RT mode active", not a
    // per-pixel miss fallback.
    if ( ssrConfidence <= 0.0f && TlasIndex != 0xFFFFFFFFu )
    {
        float rtConfidence = 0.0f;
        float3 rtColor = TraceWaterReflectionRT( Input.wpos, ssrDir, rtConfidence );
        reflection = lerp( reflection, rtColor, saturate( rtConfidence ) );
        ssrConfidence = max( ssrConfidence, rtConfidence );   // feeds reflectAmount's confidence boost below
    }

    // Darken the scene, to make a wet surface
    float f = 1 - saturate( pow( 1 - shallowDepth, 8.0f ) + clamp( kPow2(distortionSmall.y), 0.5f, 1.0f ) );

    float3 sceneWet = lerp( sceneClean, sceneClean * 0.01f, f );                       // Darken border-scene
    scene = lerp( scene, scene * float3( 4, 0.2f, 0.1f ) * 0.05f, f );                 // Darken distorted scene

    float pxDistance = Input.vz.y;
    scene = lerp( scene, diffuse, 0.73f * max( pow( fresnel, 8.0f ), 0.5f ) );
    float3 color = lerp( scene, sceneClean, kPow4(saturate( pxDistance / 35000.0f )) );
    color = lerp( color, sceneWet, ( 1 - shallowDepth ) );

    // Reflection compositing.
    // Fresnel (view angle) is the primary driver of how much reflection shows, same as real water:
    // looking straight down mostly shows the water body's own color, looking at a grazing angle mostly
    // shows the reflection. ssrConfidence only picks *which* reflection source to use (real on-screen
    // geometry vs the static cube, chosen above) and gives it a modest boost — it must not override the
    // angle-based blend entirely, or the water reads as a flat mirror regardless of the viewing angle.
    float NdotV = saturate( dot( -viewDirection, wavesFres ) );
    float reflectFresnel = kPow3(1.0f - NdotV);

    // Waterfalls (surface normal pointing mostly sideways rather than up) get a strong, distracting
    // reflection because the geometry is nearly vertical while the shader still treats it like flat,
    // horizontal water. Use the true geometric normal (not the wave-perturbed one) to detect this and
    // fade the reflection out.
    float waterfallFactor = 1.0f - saturate( abs( normalize( Input.wnrm ).y ) );
    float reflectSuppress = mad(waterfallFactor, 0.12f - 1.0f, 1.0f);

    float reflectAmount = saturate( mad(reflectFresnel, 1.0f - 0.35f, 0.35f) * mad(saturate( ssrConfidence ), 1.0f - 0.5f, 0.5f) * reflectFresnel ) * reflectSuppress;
    color = lerp( color, reflection * lerp( 1.0f, diffuse, 0.6f ), reflectAmount );

    if ( UseAtmosphere != 0 )
        color.rgb = ApplyAtmosphericScatteringGround( Input.wpos, color.rgb );

    // Do spec lighting
    float3 sunOrange = float3( 0.6, 0.3, 0.1 ) * 2.0f;
    float3 sunColor = lerp( sunOrange, 1.0f, AC_LightPos.y ) * 5.0f;

    float3 reflect_vecSmall = reflect( -viewDirection, normalize( distortionSmall.xzy * float3( 1, 10, 1 ) ) );

    float cos_spec = saturate(dot( reflect_vecSmall, -AC_LightPos.xyz ));
    float sun_spot = pow( cos_spec, 500.0f ) * 0.5f;
    // Input.col is the packed R8G8B8A8 vertex DWORD; D3D11's vDiffuse.y is the GREEN channel, which the
    // BGRA-ordered DWORD puts in .g here as well — no swizzle needed for this one component.
    color.rgb += lerp( sunColor * sun_spot, float3( 0.0f, 0.0f, 0.0f ), step( step( 0.0f, AC_LightPos.y ) * Input.col.g, 0.5f ) );

    // darken / lighten water based on the day / night cycle
    float darknessFactor = 2.0f;
    darknessFactor -= AC_LightPos.y;

    // Opaque write: the see-through look is already composited above from the scene copy, exactly like
    // D3D11 (whose water PSO has BlendEnabled = false and also returns alpha 1).
    return float4( color / darknessFactor, 1 );
}
