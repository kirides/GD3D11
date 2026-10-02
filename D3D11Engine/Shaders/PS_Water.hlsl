//--------------------------------------------------------------------------------------
// Water surface pixel shader for G2D3D11 by Degenerated; the shading lives in include/WaterShading.hlsl
//--------------------------------------------------------------------------------------
#include <AtmosphericScattering.h>
#include <FFFog.h>
#include <DS_Defines.h>
#include <DepthReconstruction.h>
#include <include/MathHelpers.hlsl>

cbuffer RefractionInfo : register( b2 )
{
	float4x4 RI_Projection;
	float2 RI_ViewportSize;
	float RI_Time;
	float RI_CameraUnderwater; // 1 while the camera is below the surface: no water body above it

	float3 RI_CameraPosition;
	// Runtime SSR gate (quality itself is the compile-time SSR_QUALITY permutation). 0 while the
	// camera is underwater: from below the surface the "reflection" ray marches up through the
	// water body into geometry that is in front of the camera, so every hit is bogus and the
	// water reads as a mirror of the shoreline instead of the underwater view.
	float RI_SSREnabled;

	float4x4 RI_View; // World->view, for screen-space reflections
};

cbuffer WaterParams : register( b3 )
{
	float3 WP_MoonDir;          // world space, toward the moon
	float WP_MoonGlint;         // 0 by day, below the horizon, in fog and rain
	float3 WP_OceanTint;        // luma-neutral regional ocean tint
	float WP_OceanTintStrength;
	float WP_OceanClimate;      // 0 = coastal Khorinis, 1 = clear Jharkendar
	float WP_IsOcean;           // per texture batch: NW_WATER_LAKE*
	float WP_MoonDisc;          // visibility of the moon disc in the sky
	float WP_SkyReflection;     // 1 = march the reflected sky in screen space
	float WP_OceanTexture;      // 0 = pure water body, 1 = legacy-strength texture blend
	float WP_ShoreFoam;         // 0 = off, 1 = ocean only, 2 = all water
	float WP_ShoreFieldState;   // 0 = no shoreline field, 1 = field, 2 = debug paint
	float WP_Pad;
	float4 WP_ShoreFieldMapping; // xy = field corner (world xz), zw = 1 / its extent
};

//--------------------------------------------------------------------------------------
// Textures and Samplers
//--------------------------------------------------------------------------------------
SamplerState SS_Linear : register( s0 );
SamplerState SS_samMirror : register( s1 );
Texture2D	TX_Diffuse : register( t0 );

Texture2D	TX_Depth : register( t2 );
TextureCube	TX_ReflectionCube : register( t3 );
Texture2D	TX_Distortion : register( t4 );
Texture2D	TX_Scene : register( t5 );
Texture2D	TX_WaterSurfaceDepth : register( t6 ); // live depth after the water prepass, for shore probes
Texture2D	TX_LowClouds : register( t7 );         // premultiplied low cloud layer; unbound = no clouds
Texture2D<float> TX_SkyAverage : register( t8 );   // 4x1 average on-screen sky (rgb, valid); unbound = none
Texture2D	TX_ShoreField : register( t9 );        // ShoreField.cpp: distance, depth, seaward xz

//--------------------------------------------------------------------------------------
// Input / Output structures
//--------------------------------------------------------------------------------------
struct PS_INPUT
{
	float2 vTexcoord		: TEXCOORD0;
	float2 vTexcoord2		: TEXCOORD1;
	float4 vDiffuse			: TEXCOORD2;
	float3 vNormalWS		: TEXCOORD4;
	float3 vWorldPosition	: TEXCOORD5;
	float4 vPosition		: SV_POSITION;
};


//--------------------------------------------------------------------------------------
// Screen-space reflections
// Marches the (wave-perturbed) reflection ray in view space against the copied scene
// depth (TX_Depth) and returns the scene color (TX_Scene) at the hit. On a miss the
// confidence is 0 so the caller falls back to the sky and the static reflection cube.
//--------------------------------------------------------------------------------------
// SSR_QUALITY is a compile-time permutation macro: 0=Disabled, 1=Low, 2=Medium, 3=High.
// Default to Medium if the macro isn't supplied (e.g. standalone compile).
#ifndef SSR_QUALITY
#define SSR_QUALITY 2
#endif

#if SSR_QUALITY > 0

#if SSR_QUALITY == 1        // Low
	#define SSR_MAX_STEPS    12
	#define SSR_REFINE_STEPS 4
#elif SSR_QUALITY == 2      // Medium (balanced quality)
	#define SSR_MAX_STEPS    24
	#define SSR_REFINE_STEPS 5
#else                       // High
	#define SSR_MAX_STEPS    48
	#define SSR_REFINE_STEPS 6
#endif

#define SSR_MAX_DISTANCE    30000.0f  // view-space units the ray may travel
#define SSR_THICKNESS       350.0f    // max depth gap that still counts as a hit
#define SSR_START_BIAS      2.0f     // push off the surface to avoid self-intersection

// Project a view-space position to screen UV. Returns false if behind the camera.
bool SSR_ProjectToUV( float3 posVS, out float2 uv )
{
	float4 clip = mul( float4(posVS, 1.0f), RI_Projection );
	if ( clip.w <= 0.0f )
	{
		uv = float2(0.0f, 0.0f);
		return false;
	}
	uv = (clip.xy / clip.w) * float2(0.5f, -0.5f) + 0.5f;
	return true;
}

// Linear view-space Z of the scene at a screen UV. The main camera writes reversed-Z
// with an infinite far plane (depth == 1/viewZ), so linear Z is the reciprocal. Using
// the shared helper avoids relying on projection-matrix element/packing conventions.
//
// Point-sample (Load), never bilinear: at silhouette edges of thin geometry (masts,
// poles) bilinear filtering blends foreground and far-background raw depth into a
// phantom Z that matches no real surface. The ray "hits" that phantom depth and then
// samples the bright sky behind the edge -> sparse blue/white speckles. Nearest-texel
// depth removes those false intersections.
float SSR_SceneZ( float2 uv )
{
	int2 px = clamp( int2( uv * RI_ViewportSize ), int2(0, 0), int2( RI_ViewportSize ) - 1 );
	float raw = TX_Depth.Load( int3( px, 0 ) ).r;
	return LinearizeDepthReverseZInfinite( raw );
}

float3 TraceWaterSSR( float3 worldPos, float3 reflectDirWS, out float confidence, out float hitDistance )
{
	confidence = 0.0f;
	hitDistance = 0.0f;

	float3 originVS = mul( float4(worldPos, 1.0f), RI_View ).xyz;
	float3 dirVS = normalize( mul( float4(reflectDirWS, 0.0f), RI_View ).xyz );

	// Uniform march; binary search recovers precision at the hit.
	const float stepLen = SSR_MAX_DISTANCE / (float)SSR_MAX_STEPS;
	float startBias = max( SSR_START_BIAS, originVS.z * 0.002f );

	float3 prevPos = originVS + dirVS * startBias;
	float2 prevUV;
	if ( !SSR_ProjectToUV( prevPos, prevUV ) )
		return float3(0.0f, 0.0f, 0.0f);
	// delta < 0 => ray is in front of the scene surface at this pixel.
	// Sky/far pixels have a huge sceneZ, so delta stays very negative there.
	float prevDelta = prevPos.z - SSR_SceneZ( prevUV );
	float travelled = startBias;

	[loop]
	for ( int i = 0; i < SSR_MAX_STEPS; ++i )
	{
		float3 curPos = prevPos + dirVS * stepLen;
		travelled += stepLen;

		float2 uv;
		if ( !SSR_ProjectToUV( curPos, uv ) )
			return float3(0.0f, 0.0f, 0.0f); // behind camera -> fall back to cube
		if ( any(uv < 0.0f) || any(uv > 1.0f) )
			return float3(0.0f, 0.0f, 0.0f); // left the screen -> fall back to cube

		float sceneZ = SSR_SceneZ( uv );
		float curDelta = curPos.z - sceneZ;

		// Front -> behind crossing between prevPos and curPos: we hit a surface.
		//
		// ...but only if the surface sits at or behind where the ray was already in
		// front (prevPos.z). A genuine continuous surface satisfies sceneZ >= prevPos.z.
		// If curUV's sceneZ is much NEARER than prevPos.z, the screen-space ray merely
		// swept BEHIND a foreground silhouette (e.g. the player standing between the
		// water and the far shore): sceneZ teleports from far-background to near-player,
		// firing a false crossing. Rejecting these (and continuing the march) stops the
		// player's dark silhouette from smearing into the water. This must gate the
		// crossing itself, not the post-refine gap, which binary search always shrinks.
		if ( prevDelta < 0.0f && curDelta >= 0.0f &&
		     sceneZ >= prevPos.z - SSR_THICKNESS )
		{
			// Binary-search refine between prevPos (in front) and curPos (behind).
			float3 lo = prevPos;
			float3 hi = curPos;
			float2 hitUV = uv;
			float hitGap = curDelta;
			[unroll]
			for ( int j = 0; j < SSR_REFINE_STEPS; ++j )
			{
				float3 mid = (lo + hi) * 0.5f;
				float2 midUV;
				if ( !SSR_ProjectToUV( mid, midUV ) )
					break;
				float midGap = mid.z - SSR_SceneZ( midUV );
				if ( midGap >= 0.0f )
				{
					hi = mid;
					hitUV = midUV;
					hitGap = midGap;
				}
				else
				{
					lo = mid;
				}
			}

			// After refinement a real surface converges to a small residual gap.
			// A large residual means the ray passed behind a thin object into empty
			// space (its far side); reject so we don't smear background over water.
			if ( hitGap < SSR_THICKNESS )
			{
				// Fade only in the outermost sliver near the screen borders (where the
				// reflected data genuinely runs out), plus at the end of the ray.
				float2 edge = smoothstep( 0.0f, 0.001f, hitUV ) * smoothstep( 0.0f, 0.001f, 1.0f - hitUV );
				float edgeFade = edge.x * edge.y;
				float distFade = saturate( 1.0f - travelled / SSR_MAX_DISTANCE );

				confidence = edgeFade * distFade;
				hitDistance = travelled;
				return TX_Scene.SampleLevel( SS_Linear, hitUV, 0 ).rgb;
			}
		}

		prevPos = curPos;
		prevDelta = curDelta;
	}

	return float3(0.0f, 0.0f, 0.0f); // nothing hit -> fall back to cube
}

#endif // SSR_QUALITY > 0

//--------------------------------------------------------------------------------------
// Hooks for include/WaterShading.hlsl (D3D11 renders in gamma space, so no conversion)
//--------------------------------------------------------------------------------------
float WaterSceneRawDepth( float2 uv ) { return TX_Depth.SampleLevel( SS_Linear, uv, 0 ).r; }
float WaterSceneRawDepthTexel( float2 uv )
{
	int2 px = clamp( int2( uv * RI_ViewportSize ), int2( 0, 0 ), int2( RI_ViewportSize ) - 1 );
	return TX_Depth.Load( int3( px, 0 ) ).r;
}
float WaterSurfaceRawDepth( float2 uv ) { return TX_WaterSurfaceDepth.SampleLevel( SS_Linear, uv, 0 ).r; }
float WaterLinearDepth( float raw ) { return RI_Projection._43 / ( raw - RI_Projection._33 ); }
float3 WaterWorldToView( float3 p ) { return mul( float4( p, 1.0f ), RI_View ).xyz; }
bool WaterViewToUV( float3 posVS, out float2 uv )
{
	float4 clip = mul( float4( posVS, 1.0f ), RI_Projection );
	uv = ( clip.xy / max( clip.w, 0.0001f ) ) * float2( 0.5f, -0.5f ) + 0.5f;
	return clip.w > 0.0f;
}
float3 WaterSceneColor( float2 uv ) { return TX_Scene.SampleLevel( SS_Linear, uv, 0 ).rgb; }
float3 WaterDistortion( float2 uv ) { return TX_Distortion.Sample( SS_Linear, uv ).xyz; }
float3 WaterDiffuse( float2 uv ) { return TX_Diffuse.Sample( SS_Linear, uv ).rgb; }
float3 WaterCube( float3 dir ) { return TX_ReflectionCube.Sample( SS_Linear, dir ).xyz; }
float3 WaterScatterGround( float3 worldPos, float3 color ) { return ApplyAtmosphericScatteringGround( worldPos, color ); }
float4 WaterLowClouds( float2 uv ) { return TX_LowClouds.SampleLevel( SS_Linear, uv, 0 ); }
float4 WaterSkyAverage()
{
	return float4( TX_SkyAverage.Load( int3( 0, 0, 0 ) ), TX_SkyAverage.Load( int3( 1, 0, 0 ) ),
	               TX_SkyAverage.Load( int3( 2, 0, 0 ) ), TX_SkyAverage.Load( int3( 3, 0, 0 ) ) );
}

// Away from the field: as far from any shore as WATER_SHORE_FAR / WATER_SHORE_NO_FLOOR say
float4 WaterShoreField( float2 xz )
{
	float2 uv = ( xz - WP_ShoreFieldMapping.xy ) * WP_ShoreFieldMapping.zw;
	if ( any( uv < 0.0f ) || any( uv > 1.0f ) ) return float4( 30000.0f, 5000.0f, 0.0f, 0.0f );
	return TX_ShoreField.SampleLevel( SS_Linear, uv, 0 );
}

bool WaterSSREnabled()
{
#if SSR_QUALITY > 0
	return RI_SSREnabled > 0.0f;
#else
	return false;
#endif
}

float3 WaterTraceSSR( float3 worldPos, float3 dir, out float confidence, out float hitDistance )
{
#if SSR_QUALITY > 0
	return TraceWaterSSR( worldPos, dir, confidence, hitDistance );
#else
	confidence = 0.0f;
	hitDistance = 0.0f;
	return float3( 0.0f, 0.0f, 0.0f );
#endif
}

#include <include/WaterShading.hlsl>

//--------------------------------------------------------------------------------------
// Pixel Shader
//--------------------------------------------------------------------------------------
float4 PSMain( PS_INPUT Input ) : SV_TARGET
{
	WaterPixel px;
	px.screenUV = Input.vPosition.xy / RI_ViewportSize;
	px.texcoord = Input.vTexcoord;
	px.surfaceViewZ = Input.vTexcoord2.x;
	px.surfaceViewDistance = Input.vTexcoord2.y;
	px.worldPos = Input.vWorldPosition;
	px.geometricNormal = Input.vNormalWS;

	WaterFrame fr;
	fr.cameraPos = RI_CameraPosition;
	fr.time = RI_Time;
	fr.viewportSize = RI_ViewportSize;
	fr.cameraBelow = RI_CameraUnderwater;
	fr.isOcean = WP_IsOcean;
	fr.oceanClimate = WP_OceanClimate;
	fr.oceanTint = WP_OceanTint;
	fr.oceanTintStrength = WP_OceanTintStrength;
	fr.oceanTexture = WP_OceanTexture;
	fr.moonDir = WP_MoonDir;
	fr.moonGlint = WP_MoonGlint;
	fr.moonDisc = WP_MoonDisc;
	fr.skyReflection = WP_SkyReflection;
	fr.shoreFoam = WP_ShoreFoam;
	fr.shoreFieldState = WP_ShoreFieldState;

	return float4( ShadeWater( px, fr ), 1.0f );
}
