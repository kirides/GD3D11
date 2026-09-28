//--------------------------------------------------------------------------------------
// World/VOB-Pixelshader for G2D3D11 by Degenerated
//--------------------------------------------------------------------------------------
#include <AtmosphericScattering.h>
#include <FFFog.h>
#include <DS_Defines.h>
#include <DepthReconstruction.h>
#include <include/MathHelpers.hlsl>

static const float DIST_SMALL_SPEED = -0.01f;
static const float DIST_SMALL_AMOUNT = 0.01f;
static const float DIST_SMALL_SCALE = 0.3f;
static const float DIST_BIG_SCALE = 0.1f;
static const float DIST_BIG_SPEED = -0.005f;


// Cleans the refraction borders
#define CleanRefraction(uv, screen_uv, depthRef) (lerp(uv, screen_uv, saturate(Input.vTexcoord2.x-depthRef)))

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

cbuffer WaterParams : register( b3 )
{
	float3 WP_MoonDir;          // world space, toward the moon
	float WP_MoonGlint;         // 0 by day, below the horizon, in fog and rain
	float3 WP_OceanTint;        // luma-neutral regional ocean tint
	float WP_OceanTintStrength;
	float WP_OceanClimate;      // 0 = coastal Khorinis, 1 = clear Jharkendar
	float WP_IsOcean;           // per texture batch: NW_WATER_LAKE*
	float2 WP_Pad;
};

float WaterSceneRawDepth( float2 uv ) { return TX_Depth.SampleLevel( SS_Linear, uv, 0 ).r; }
float WaterSurfaceRawDepth( float2 uv ) { return TX_WaterSurfaceDepth.SampleLevel( SS_Linear, uv, 0 ).r; }
float WaterLinearDepth( float raw ) { return RI_Projection._43 / ( raw - RI_Projection._33 ); }
float3 WaterWorldToView( float3 p ) { return mul( float4( p, 1.0f ), RI_View ).xyz; }
bool WaterViewToUV( float3 posVS, out float2 uv )
{
	float4 clip = mul( float4( posVS, 1.0f ), RI_Projection );
	uv = ( clip.xy / max( clip.w, 0.0001f ) ) * float2( 0.5f, -0.5f ) + 0.5f;
	return clip.w > 0.0f;
}

#include <include/WaterShading.hlsl>

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
// confidence is 0 so the caller falls back to the static reflection cube.
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

float3 TraceWaterSSR( float3 worldPos, float3 reflectDirWS, out float confidence )
{
	confidence = 0.0f;

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
// Pixel Shader
//--------------------------------------------------------------------------------------
float4 PSMain( PS_INPUT Input ) : SV_TARGET
{
	float2 screenUV = Input.vPosition.xy / RI_ViewportSize;
	float surfaceViewZ = Input.vTexcoord2.x;
	float pxDistance = Input.vTexcoord2.y;
	bool isOcean = WP_IsOcean > 0.5f;
	float cameraBelow = RI_CameraUnderwater;
	float rain = saturate( AC_RainFXWeight );
	float night = saturate( ( -AC_LightPos.y + 0.12f ) * 2.2f );

	// Linear depth
	float rawCenterDepth = TX_Depth.Sample(SS_Linear, screenUV).r;
	float depth = WaterLinearDepth(rawCenterDepth);
	float shallowDepth = saturate((depth - surfaceViewZ) * 0.01f);

	// Camera direction
	float3 viewDirection = normalize(Input.vWorldPosition - RI_CameraPosition);

	// Calculate distortion vectors
	float2 worldTexCoord = Input.vWorldPosition.xz / 1000.0f;
	float3 distortionSmall = TX_Distortion.Sample(SS_Linear, worldTexCoord * DIST_SMALL_SCALE + RI_Time * DIST_SMALL_SPEED).xyz * 2 - 1;
	distortionSmall += TX_Distortion.Sample(SS_Linear, worldTexCoord * float2(-1,0.7) * DIST_SMALL_SCALE + RI_Time * DIST_SMALL_SPEED * 2).xyz * 2 - 1;
	distortionSmall *= 0.5f;

	float3 distortionBig = TX_Distortion.Sample(SS_Linear, worldTexCoord * DIST_BIG_SCALE + RI_Time * DIST_BIG_SPEED).xyz * 2 - 1;
	distortionBig += TX_Distortion.Sample(SS_Linear, worldTexCoord * float2(-1,0.7) * DIST_BIG_SCALE + RI_Time * DIST_BIG_SPEED * 1.2).xyz * 2 - 1;
	distortionBig *= 0.5f;

	float2 distUV = screenUV + distortionSmall.xy * DIST_SMALL_AMOUNT + distortionBig.xy * DIST_SMALL_AMOUNT;

	// Distorted diffuse
	float3 diffuse = TX_Diffuse.Sample(SS_Linear, Input.vTexcoord + distortionSmall.xy * DIST_SMALL_AMOUNT * 0.5f).rgb;

	// Refracted depth
	float depthRefracted = WaterLinearDepth(TX_Depth.Sample(SS_Linear, distUV).r);

	distUV = CleanRefraction(distUV, screenUV, depthRefracted);
	distUV = saturate(distUV);

	// Re-fetch at the cleaned UV so the water column matches the scene texel actually refracted
	float rawDepthRefracted = TX_Depth.Sample(SS_Linear, distUV).r;
	depthRefracted = WaterLinearDepth(rawDepthRefracted);
	float refractedValid = step(0.000001f, rawDepthRefracted);

	// Wave vectors
	float3 wavesFres = normalize(distortionBig.xzy * float3(1,10,1));
	float3 wavesSmall = normalize(distortionSmall.xzy * float3(1,10,1));

	// Scene color
	float3 scene = TX_Scene.Sample(SS_Linear, distUV).rgb;
	float3 sceneClean = TX_Scene.Sample(SS_Linear, lerp(distUV, screenUV, pow(1-shallowDepth, 20.0f))).rgb;

	// Fresnel from waves
	float fresnel = min(0.5f, saturate(pow(1.0f - saturate(dot(-viewDirection, wavesFres)), 10.0f)));

	// Reflection: static cube, replaced by screen-space hits where the trace finds on-screen geometry
	float3 reflect_vec = reflect(-viewDirection, wavesFres);
	float3 cube = TX_ReflectionCube.Sample(SS_Linear, reflect_vec).xyz;
	float ssrConfidence = 0.0f;
	float3 ssrColor = float3(0.0f, 0.0f, 0.0f);
#if SSR_QUALITY > 0
	[branch] if ( RI_SSREnabled > 0.0f )
	{
		// The eye reflection marching UP into the scene is reflect(viewDirection, N); a half-flattened
		// wave normal keeps the rays coherent instead of scattering into off-screen misses.
		float3 ssrNormal = normalize(lerp(float3(0.0f, 1.0f, 0.0f), wavesFres, 0.5f));
		float3 ssrDir = reflect(viewDirection, ssrNormal);
		ssrColor = TraceWaterSSR(Input.vWorldPosition, ssrDir, ssrConfidence);
	}
#endif
	ssrConfidence = saturate(ssrConfidence);

	// Fresnel picks how much reflection shows; ssrConfidence only picks the source and adds a modest boost.
	float NdotV = saturate(dot(-viewDirection, wavesFres));
	float reflectFresnel = kPow3(1.0f - NdotV);

	// Waterfalls: near-vertical sheets get neither reflections nor a shoreline
	float flatness = WaterFlatness(Input.vNormalWS);
	float waterfallMask = 1.0f - flatness;
	float reflectAmount = saturate(mad(reflectFresnel, 1.0f - 0.35f, 0.35f) * mad(ssrConfidence, 1.0f - 0.5f, 0.5f) * reflectFresnel) * lerp(0.12f, 1.0f, flatness);

	// Shoreline: water thickness along the view ray, and the vertical depth below this pixel
	float column = WaterColumnLength(depthRefracted, surfaceViewZ, pxDistance);
	float colorColumn = min(column, WaterColumnLength(depth, surfaceViewZ, pxDistance));
	float columnDeriv = fwidth(column);
	float colorColumnDeriv = fwidth(colorColumn);
	float shoreException = step(WATER_DEEP_WATER_DEPTH,
		WaterDepthBelowSurface(Input.vWorldPosition, surfaceViewZ, rawCenterDepth, RI_CameraPosition));
	[branch] if ( shoreException < 0.5f && cameraBelow < 0.5f )
		shoreException = WaterShoreProbeException(Input.vWorldPosition, RI_CameraPosition);
	float2 shore = isOcean ? OceanShore(column, columnDeriv) : LegacyShore(column, columnDeriv, colorColumn, colorColumnDeriv);
	shore = lerp(shore, 1.0f, max(max(shoreException, waterfallMask), cameraBelow));

	float3 reflect_vecSmall = reflect(-viewDirection, wavesSmall);
	float3 color;
	[branch] if ( !isOcean )
	{
		// Water body; sky behind the surface (depth 0) has no floor to absorb against
		if ( refractedValid > 0.5f && cameraBelow < 0.5f )
			scene = ApplyWaterVolume(scene, column, WATER_VOLUME_ABSORPTION, WATER_VOLUME_SCATTER);

		scene = lerp(scene, diffuse, 0.73f * max(pow(fresnel,8.0f), 0.5f));
		color = lerp(scene, sceneClean, kPow4(saturate(pxDistance / 35000.0f)));
		color = lerp(color, WaterSceneHue(color, sceneClean), refractedValid * flatness * 0.42f);
		color = lerp(sceneClean, color, shore.y);

		float3 reflection = lerp(cube, ssrColor, ssrConfidence);
		color = lerp(color, reflection * lerp(1.0f, diffuse, 0.6f), reflectAmount * shore.x);
		color = lerp(color, WaterSceneHue(color, sceneClean), refractedValid * waterfallMask * 0.14f);

		color.rgb = ApplyAtmosphericScatteringGround(Input.vWorldPosition, color.rgb);

		// Do spec lighting
		float3 sunOrange = float3(0.6,0.3,0.1) * 2.0f;
		float3 sunColor = lerp(sunOrange, 1.0f, AC_LightPos.y) * 5.0f;
		float cos_spec = saturate(dot(reflect_vecSmall, -AC_LightPos.xyz));
		float sun_spot = pow(cos_spec, 500.0f) * 0.5f * shore.x;
		color.rgb += lerp(sunColor * sun_spot, float3(0.0f, 0.0f, 0.0f), step(step(0.0f, AC_LightPos.y) * Input.vDiffuse.y, 0.5f));

		//darken / lighten water based on the day / night cycle
		color /= 2.0f - AC_LightPos.y;
	}
	else
	{
		// Ocean: the water body replaces the texture; night and rain use their own scatter colors
		float underThick = clamp(abs(depthRefracted - surfaceViewZ) * 0.35f, 0.0f, 1400.0f);
		float3 absorption, scatter;
		GetOceanOptics(WP_OceanClimate, rain, night, cameraBelow, dot(diffuse, WATER_LUMA), absorption, scatter);
		float3 transmittance = exp(-absorption * lerp(column, underThick, cameraBelow));
		float3 volume = scene * transmittance + scatter * (1.0f - transmittance);
		volume = lerp(scatter, volume, saturate(refractedValid + cameraBelow));
		volume = lerp(sceneClean, volume, shore.y);
		// From below: sky through the surface stays clear, geometry above water only partly tinted
		volume = lerp(volume, scene, cameraBelow * (1.0f - refractedValid));
		volume = lerp(volume, lerp(scene, volume, 0.32f), cameraBelow * refractedValid);

		float3 reflection = lerp(LimitOceanCube(cube, volume, night, WP_OceanClimate), ssrColor, ssrConfidence);
		float amount = reflectAmount * shore.x * (1.0f - cameraBelow) * lerp(0.82f, 1.0f, WP_OceanClimate);
		color = lerp(volume, reflection, amount);

		float sunSpot = pow(saturate(dot(reflect_vecSmall, -AC_LightPos.xyz)), 500.0f) * 0.5f
			* smoothstep(-0.04f, 0.08f, AC_LightPos.y) * (1.0f - rain) * (1.0f - cameraBelow);
		color += lerp(float3(1.2f, 0.6f, 0.2f), float3(5.0f, 5.0f, 5.0f), saturate(AC_LightPos.y))
			* sunSpot * shore.x * lerp(0.82f, 1.0f, WP_OceanClimate);

		// Regional tint, luma-neutral; rain and night bring their own colors
		float tintStrength = saturate(WP_OceanTintStrength) * lerp(1.0f, 0.35f, rain) * lerp(1.0f, 0.20f, night) * shore.y;
		color = lerp(color, color * WP_OceanTint, tintStrength);

		color.rgb = ApplyAtmosphericScatteringGround(Input.vWorldPosition, color.rgb);
	}

	// Moon on the water, after the night darkening so it keeps its brightness
	color += WaterMoonGlint(viewDirection, wavesSmall, wavesFres, WP_MoonDir)
		* WP_MoonGlint * shore.x * flatness * (1.0f - cameraBelow);

	return float4(color, 1);
}
