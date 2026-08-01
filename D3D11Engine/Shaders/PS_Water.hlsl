//--------------------------------------------------------------------------------------
// World/VOB-Pixelshader for G2D3D11 by Degenerated
//--------------------------------------------------------------------------------------
#include <AtmosphericScattering.h>
#include <FFFog.h>
#include <DS_Defines.h>
#include <DepthReconstruction.h>

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
	float RI_Pad1;

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
				return TX_Scene.Sample( SS_Linear, hitUV ).rgb;
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
	
	// Linear depth
	float depth = TX_Depth.Sample(SS_Linear, screenUV).r;
	depth = RI_Projection._43 / (depth - RI_Projection._33);
	
	// Clip here so we don't have to bind the depthbuffer
	//if(depth < Input.vTexcoord2.x) // vTexcoord2-x stores viewspace.z
	//	discard;
		
	float shallowDepth = saturate((depth - Input.vTexcoord2.x) * 0.01f);
		
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
	float depthRefracted = TX_Depth.Sample(SS_Linear, distUV).r;
	depthRefracted = RI_Projection._43 / (depthRefracted - RI_Projection._33);
	
	distUV = CleanRefraction(distUV, screenUV, depthRefracted);
	distUV = saturate(distUV);
	
	// Wave vector
	float3 wavesDist = normalize(distortionSmall.xzy * float3(1,100,1));
	float3 wavesFres = normalize(distortionBig.xzy * float3(1,10,1));
	
	// Scene color
	float3 scene = TX_Scene.Sample(SS_Linear, distUV).rgb;
	float3 sceneClean = TX_Scene.Sample(SS_Linear, lerp(distUV, screenUV, pow(1-shallowDepth, 20.0f))).rgb;
	
	// Fresnel from waves
	float fresnel = min(0.5f, saturate(pow(1.0f - saturate(dot(-viewDirection, wavesFres)), 10.0f)));
	
	// Reflection
	float3 reflect_vec = reflect(-viewDirection, wavesFres);

	// sample reflection cube (fallback for off-screen / missed rays)
	float3 reflection = TX_ReflectionCube.Sample(SS_Linear, reflect_vec).xyz;
	float ssrConfidence = 0.0f;
	float3 ssrColor = float3(0.0f, 0.0f, 0.0f);
#if SSR_QUALITY > 0
	[branch] if ( RI_SSREnabled > 0.0f )
	{
		// reflect_vec above is negated (reflect(-viewDirection,N)) for the cube lookup.
		// The true eye-reflection direction, which marches UP into the scene, is
		// reflect(viewDirection, N). Flatten the wave normal so reflection rays stay
		// coherent (mirror-like) instead of scattering into many off-screen misses.
		float3 ssrNormal = normalize(lerp(float3(0.0f, 1.0f, 0.0f), wavesFres, 0.5f));
		float3 ssrDir = reflect(viewDirection, ssrNormal);
		// Screen-space reflection of nearby on-screen geometry; falls back to the cube on a miss
		ssrColor = TraceWaterSSR(Input.vWorldPosition, ssrDir, ssrConfidence);
		reflection = lerp(reflection, ssrColor, saturate(ssrConfidence));
	}
#endif
	
	// Darken the scene, to make a wet surface
	float f = 1-saturate(pow(1-shallowDepth, 8.0f) + clamp(pow(distortionSmall.y, 2), 0.5f, 1.0f));

	float3 sceneWet = lerp(sceneClean, sceneClean * 0.01f, f); // Darken border-scene
	scene = lerp(scene, scene * float3(4, 0.2f, 0.1f) * 0.05f, f); // Darken distorted scene
	
	float pxDistance = Input.vTexcoord2.y;
	scene = lerp(scene, diffuse, 0.73f * max(pow(fresnel,8.0f), 0.5f));
	float3 color = lerp(scene, sceneClean, pow(saturate(pxDistance / 35000.0f), 4.0f));
	color = lerp(color, sceneWet, (1-shallowDepth));

	// Reflection compositing.
	// Fresnel (view angle) is the primary driver of how much reflection shows, same as
	// real water: looking straight down mostly shows the water body's own color, looking
	// at a grazing angle mostly shows the reflection. ssrConfidence only picks *which*
	// reflection source to use (real on-screen geometry vs the static cube, chosen above
	// at line 284) and gives it a modest boost - it must not override the angle-based
	// blend entirely, or the water reads as a flat mirror regardless of how you're
	// looking at it.
	float NdotV = saturate(dot(-viewDirection, wavesFres));
	float reflectFresnel = pow(1.0f - NdotV, 3.0f);

	// Waterfalls (surface normal pointing mostly sideways rather than up) get a
	// strong, distracting reflection because the geometry is nearly vertical while
	// the shader still treats it like flat, horizontal water. Use the true geometric
	// normal (not the wave-perturbed one) to detect this and fade the reflection out.
	float waterfallFactor = 1.0f - saturate(abs(normalize(Input.vNormalWS).y));
	float reflectSuppress = lerp(1.0f, 0.12f, waterfallFactor);

	float reflectAmount = saturate(lerp(0.35f, 1.0f, reflectFresnel) * lerp(0.5f, 1.0f, saturate(ssrConfidence)) * reflectFresnel) * reflectSuppress;
	color = lerp(color, reflection * lerp(1.0f, diffuse, 0.6f), reflectAmount);
	
	color.rgb = ApplyAtmosphericScatteringGround(Input.vWorldPosition, color.rgb);
	
	// Do spec lighting
	float3 sunOrange = float3(0.6,0.3,0.1) * 2.0f;
	float3 sunColor = lerp(sunOrange, 1.0f, AC_LightPos.y) * 5.0f;
	
	float3 reflect_vecSmall = reflect(-viewDirection, normalize(distortionSmall.xzy * float3(1,10,1)));
	
	float cos_spec = clamp(dot(reflect_vecSmall, -AC_LightPos.xyz * float3(1,1,1)), 0, 1);
	float sun_spot = pow(cos_spec, 500.0f) * 0.5f;
	color.rgb += lerp(sunColor * sun_spot, float3(0.0f, 0.0f, 0.0f), step(step(0.0f, AC_LightPos.y) * Input.vDiffuse.y, 0.5f));

	//darken / lighten water based on the day / night cycle
	float darknessFactor = 2.0f;
	darknessFactor -= AC_LightPos.y;

	return float4(color / darknessFactor, 1);
}
