// D3D12 water surfaces — port of the D3D11 spec pair VS_ExWater.hlsl + PS_Water.hlsl.
//
// The MVP version of this shader was a flat alpha-blended texture lookup: no refraction, no reflection,
// and translucency faked with a constant `WaterAlpha` on an SRC_ALPHA/INV_SRC_ALPHA blend. That reads as
// "too solid" because real water here is NOT a blended surface at all — D3D11 draws water OPAQUE
// (GothicBlendStateInfo::SetDefault leaves BlendEnabled = false) and does the see-through part itself, by
// sampling a *copy* of the finished scene through a distorted UV. That is what makes shallow water show
// the ground beneath it, deep water go dark, and the surface pick up sky/geometry reflections.
//
// The shading itself is include/WaterShading.hlsl, shared with PS_Water.hlsl. It runs in D3D11's gamma
// space: the hooks below convert the linear scene copy in and the result back out, so both backends match.
//
// Two deliberate deviations from D3D11:
//   1. SSR quality is a RUNTIME uniform (SsrMaxSteps/SsrRefineSteps from the CB), not the SSR_QUALITY
//      compile-time permutation. D3D12 shaders are baked at Init() and this backend has no live shader
//      reload yet (see D3D12GraphicsEngine::ReloadShaders), so a macro permutation would need a restart
//      to change; the loop bounds are uniform across the draw, so the branch is free.
//   2. Every screen-space input (scene copy, depth copy, distortion, reflection cube) is fetched
//      BINDLESSLY via SM6.6 ResourceDescriptorHeap instead of fixed t2..t5 slots. Only the per-material
//      diffuse still rides a descriptor table, because the color loop rebinds it per texture batch.
//   3. The vertex waves (D3D11's SHD_WATERANI permutation) are the runtime WaveAnimation mode, for the
//      same reason as 1.

#include "include/AtmosphericScattering.hlsl"   // ApplyAtmosphericScatteringGround + the Atmosphere cbuffer (b1)
#include "../include/MathHelpers.hlsl"
#include "../include/WaterVertexWaves.hlsl"

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

    uint   CameraUnderwater;     // 1 while the camera is below the surface: no water body above it
    uint   SurfaceDepthIndex;    // depth after the water prepass, for shore probes (0xFFFFFFFF = unavailable)
    float  OceanClimate;         // 0 = coastal Khorinis, 1 = clear Jharkendar
    float  OceanTintStrength;

    float3 MoonDir;              // world space, toward the moon
    float  MoonGlint;            // 0 by day, below the horizon, in fog and rain
    float3 OceanTint;            // luma-neutral regional ocean tint
    float  MoonDisc;             // visibility of the moon disc in the sky

    uint   LowCloudIndex;        // premultiplied low cloud layer (0xFFFFFFFF = none)
    float  SkyReflection;        // 1 = march the reflected sky in screen space
    uint   SkyAverageIndex;      // 4x1 average on-screen sky, linear (0xFFFFFFFF = none)
    float  OceanTexture;         // 0 = pure water body, 1 = legacy-strength texture blend

    uint   RtColorIndex;         // WaterRT.hlsl result, premultiplied (0xFFFFFFFF = screen-space reflections)
    uint   RtDistanceIndex;      // its hit distance, premultiplied
    uint   WaveAnimation;        // WATER_WAVES_*, for water materials with a wave mode
    float  ShoreFoam;            // 0 = off, 1 = ocean only, 2 = all water

    float4 ShoreFieldMapping;    // xy = field corner (world xz), zw = 1 / its extent
    uint   ShoreFieldIndex;      // ShoreField.cpp: distance, depth, seaward xz (0xFFFFFFFF = none)
    float  ShoreFieldState;      // 0 = no shoreline field, 1 = field, 2 = debug paint
    float2 ShoreFieldPad;
};

cbuffer WaterBatchCB : register(b3) { uint IsOcean; };   // root constant, per texture batch: NW_WATER_LAKE*

Texture2D    tx  : register(t0);   // per-material diffuse (descriptor table — rebound per texture batch)
SamplerState smp : register(s0);   // linear WRAP  — diffuse + the world-space distortion lookups
SamplerState smpClamp : register(s1); // linear CLAMP — screen-space fetches (scene copy, depth copy)

// Octahedral normal decode — matches Shaders/VertexPacking.h DecodeOctNormal (the packed 36-byte world
// vertex stores its world-space normal as an R16G16_SNORM octahedral pair).
float3 DecodeOctNormal( float2 e )
{
    float3 n = float3( e.xy, 1.0 - abs( e.x ) - abs( e.y ) );
    float t = saturate( -n.z );
    n.xy += select( n.xy >= 0.0, -t, t );
    return normalize( n );
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
    float3 pos = i.pos;
    [branch] if ( WaveAnimation != WATER_WAVES_OFF ) pos += WaterWaveOffset( pos, i.col, RI_TotalTime, WaveAnimation );
    o.clip = mul( float4( pos, 1.0 ), ViewProj );
    float2 ani = i.scroll * RI_TotalTime;   // scroll delta (TexCoord2) * total time (ms), like VS_ExWater
    ani -= floor( ani );                    // wrap to [0,1) so the float stays precise over long sessions
    o.uv = i.uv + ani;
    o.col = i.col;
    o.wnrm = DecodeOctNormal( i.nrm );      // already world-space
    o.wpos = pos;
    // mul(v, M) with a verbatim-uploaded Gothic matrix evaluates to M*v (see CLAUDE.md's column-major
    // note) — the same convention D3D11's PS_Water/VS_ExWater rely on.
    o.vz.x = mul( float4( pos, 1.0 ), RI_View ).z;
    o.vz.y = length( mul( float4( pos, 1.0 ), RI_View ) );
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

float3 TraceWaterSSR( float3 worldPos, float3 reflectDirWS, out float confidence, out float hitDistance )
{
    confidence = 0.0f;
    hitDistance = 0.0f;

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
                hitDistance = travelled;
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
// Hooks for include/WaterShading.hlsl, which shades in gamma space like D3D11
//--------------------------------------------------------------------------------------
float3 WaterToGamma( float3 c ) { return pow( max( c, 0.0f ), 1.0f / 2.2f ); }
float3 WaterToLinear( float3 c ) { return pow( max( c, 0.0f ), 2.2f ); }

float WaterSceneRawDepth( float2 uv )
{
    Texture2D<float> depthTex = ResourceDescriptorHeap[DepthIndex];
    return depthTex.SampleLevel( smpClamp, uv, 0 ).r;
}

float WaterSceneRawDepthTexel( float2 uv )
{
    Texture2D<float> depthTex = ResourceDescriptorHeap[DepthIndex];
    int2 px = clamp( int2( uv * RI_ViewportSize ), int2( 0, 0 ), int2( RI_ViewportSize ) - 1 );
    return depthTex.Load( int3( px, 0 ) );
}

float WaterSurfaceRawDepth( float2 uv )
{
    if ( SurfaceDepthIndex == 0xFFFFFFFFu ) return 0.0f;
    Texture2D<float> depthTex = ResourceDescriptorHeap[SurfaceDepthIndex];
    return depthTex.SampleLevel( smpClamp, uv, 0 ).r;
}

float WaterLinearDepth( float raw ) { return LinearizeWaterDepth( raw ); }
float3 WaterWorldToView( float3 p ) { return mul( float4( p, 1.0f ), RI_View ).xyz; }

bool WaterViewToUV( float3 posVS, out float2 uv )
{
    float4 clip = mul( float4( posVS, 1.0f ), RI_Projection );
    uv = ( clip.xy / max( clip.w, 0.0001f ) ) * float2( 0.5f, -0.5f ) + 0.5f;
    return clip.w > 0.0f;
}

float3 WaterSceneColor( float2 uv )
{
    Texture2D sceneTex = ResourceDescriptorHeap[SceneIndex];
    return WaterToGamma( sceneTex.SampleLevel( smpClamp, uv, 0 ).rgb );
}

float3 WaterDistortion( float2 uv )
{
    Texture2D distortionTex = ResourceDescriptorHeap[DistortionIndex];
    return distortionTex.Sample( smp, uv ).xyz;
}

float3 WaterDiffuse( float2 uv ) { return tx.Sample( smp, uv ).rgb; }   // gamma-encoded texels, used as-is

float3 WaterCube( float3 dir )
{
    if ( ReflectionCubeIndex == 0xFFFFFFFFu ) return float3( 0.0f, 0.0f, 0.0f );
    TextureCube reflectionCube = ResourceDescriptorHeap[ReflectionCubeIndex];
    return reflectionCube.Sample( smp, dir ).xyz;
}

float3 WaterScatterGround( float3 worldPos, float3 color )
{
    return UseAtmosphere != 0 ? ApplyAtmosphericScatteringGround( worldPos, color ) : color;
}

bool WaterSSREnabled() { return SsrMaxSteps > 0 || RtColorIndex != 0xFFFFFFFFu; }

// Away from the field: as far from any shore as WATER_SHORE_FAR / WATER_SHORE_NO_FLOOR say
float4 WaterShoreField( float2 xz )
{
    float2 uv = ( xz - ShoreFieldMapping.xy ) * ShoreFieldMapping.zw;
    if ( ShoreFieldIndex == 0xFFFFFFFFu || any( uv < 0.0f ) || any( uv > 1.0f ) ) return float4( 30000.0f, 5000.0f, 0.0f, 0.0f );
    Texture2D field = ResourceDescriptorHeap[ShoreFieldIndex];
    return field.SampleLevel( smpClamp, uv, 0 );
}

float4 WaterLowClouds( float2 uv )   // stored in gamma space like the shading here
{
    if ( LowCloudIndex == 0xFFFFFFFFu ) return float4( 0.0f, 0.0f, 0.0f, 0.0f );
    Texture2D clouds = ResourceDescriptorHeap[LowCloudIndex];
    return clouds.SampleLevel( smpClamp, uv, 0 );
}

float4 WaterSkyAverage()
{
    if ( SkyAverageIndex == 0xFFFFFFFFu ) return float4( 0.0f, 0.0f, 0.0f, 0.0f );
    Texture2D<float> avg = ResourceDescriptorHeap[SkyAverageIndex];
    float3 c = float3( avg.Load( int3( 0, 0, 0 ) ), avg.Load( int3( 1, 0, 0 ) ), avg.Load( int3( 2, 0, 0 ) ) );
    return float4( WaterToGamma( c ), avg.Load( int3( 3, 0, 0 ) ) );
}

static float2 g_WaterScreenUV;   // set first thing in PSMain

float3 WaterTraceSSR( float3 worldPos, float3 dir, out float confidence, out float hitDistance )
{
    [branch] if ( RtColorIndex != 0xFFFFFFFFu )
    {
        // Premultiplied by coverage, so the bilinear upsample does not bleed misses into hits
        Texture2D rtColor = ResourceDescriptorHeap[RtColorIndex];
        Texture2D<float> rtDistance = ResourceDescriptorHeap[RtDistanceIndex];
        float4 c = rtColor.SampleLevel( smpClamp, g_WaterScreenUV, 0 );
        float inv = c.a > 1e-4 ? 1.0f / c.a : 0.0f;
        confidence = saturate( c.a );
        hitDistance = rtDistance.SampleLevel( smpClamp, g_WaterScreenUV, 0 ) * inv;
        return WaterToGamma( c.rgb * inv );
    }
    return WaterToGamma( TraceWaterSSR( worldPos, dir, confidence, hitDistance ) );
}

#include "../include/WaterShading.hlsl"

//--------------------------------------------------------------------------------------
// Pixel Shader
//--------------------------------------------------------------------------------------
float4 PSMain( VS_OUT Input ) : SV_TARGET
{
    g_WaterScreenUV = Input.clip.xy / RI_ViewportSize;
    WaterPixel px;
    px.screenUV = g_WaterScreenUV;
    px.texcoord = Input.uv;
    px.surfaceViewZ = Input.vz.x;
    px.surfaceViewDistance = Input.vz.y;
    px.worldPos = Input.wpos;
    px.geometricNormal = Input.wnrm;

    WaterFrame fr;
    fr.cameraPos = RI_CameraPosition;
    fr.time = RI_Time;
    fr.viewportSize = RI_ViewportSize;
    fr.cameraBelow = CameraUnderwater != 0 ? 1.0f : 0.0f;
    fr.isOcean = IsOcean != 0 ? 1.0f : 0.0f;
    fr.oceanClimate = OceanClimate;
    fr.oceanTint = OceanTint;
    fr.oceanTintStrength = OceanTintStrength;
    fr.oceanTexture = OceanTexture;
    fr.moonDir = MoonDir;
    fr.moonGlint = MoonGlint;
    fr.moonDisc = MoonDisc;
    fr.skyReflection = SkyReflection;
    fr.shoreFoam = ShoreFoam;
    fr.shoreFieldState = ShoreFieldIndex != 0xFFFFFFFFu ? ShoreFieldState : 0.0f;

    // Opaque write: the see-through look is composited from the scene copy, exactly like D3D11
    return float4( WaterToLinear( ShadeWater( px, fr ) ), 1.0f );
}
