
void ClipDistanceEffect(float viewSpaceDepth, float drawDistance, float noise, float noiseScale)
{
	if(viewSpaceDepth + noise * noiseScale > drawDistance)
		discard;
}

float3x3 cotangent_frame( float3 N, float3 p, float2 uv )
{
    // get edge vectors of the pixel triangle
    float3 dp1 = ddx( p );
    float3 dp2 = ddy( p );
    float2 duv1 = ddx( uv );
    float2 duv2 = ddy( uv );
 
    // solve the linear system
    float3 dp2perp = cross( dp2, N );
    float3 dp1perp = cross( N, dp1 );
    float3 T = dp2perp * duv1.x + dp1perp * duv2.x;
    float3 B = dp2perp * duv1.y + dp1perp * duv2.y;

    // Handedness of the derived (T, B) frame flips per-triangle for mirrored UV islands.
    // Detect it from the signed area of the UV-space triangle so mirrored islands get the
    // opposite correction instead of relying on one hardcoded global flip.
    // NOTE: the comparison direction below is our pipeline's calibrated convention bias
    // (previously expressed as an unconditional X-negate on the sampled normal map).
    // If specular highlights look mirrored after this change, flip "< 0" to "> 0" here.
    float handedness = ( duv1.x * duv2.y - duv1.y * duv2.x ) < 0.0f ? 1.0f : -1.0f;
    T *= handedness;

    // construct a scale-invariant frame
    float invmax = rsqrt( max( dot(T,T), dot(B,B) ) );
    return float3x3( T * invmax, B * invmax, N );
}

#ifndef NORMAL_MAP_MODE
// 1 = OpenGL (Y+)
// 2 = DirectX (Y-)
#define NORMAL_MAP_MODE 1
#endif

#ifndef NORMAL_MAP_RESTORE_Z
// NORMAL_MAP_RESTORE_Z allows to use BC5 compressed normal maps.
#define NORMAL_MAP_RESTORE_Z 1
#endif


/** Magic TBN-Calculation function */
float3 perturb_normal_rgb( float3 N, float3 V, Texture2D normalmap, float2 texcoord, SamplerState samplerState, float normalmapDepth = 1.0f)
{
    // assume N, the interpolated vertex normal and 
    // V, the view vector (vertex to eye)
    float3 nrmmap = normalmap.Sample(samplerState, texcoord).xyz * 2 - 1;
#if NORMAL_MAP_MODE == 2
    // flip G channel in DirectX Normals
	nrmmap.y = -nrmmap.y;
#endif
	nrmmap.xy *= normalmapDepth;
	nrmmap = normalize(nrmmap);
	
    float3x3 TBN = cotangent_frame( N, -V, texcoord );
    return normalize( mul( nrmmap, TBN ) );
}

/** Magic TBN-Calculation function */
float3 perturb_normal_restore_z( float3 N, float3 V, Texture2D normalmap, float2 texcoord, SamplerState samplerState, float normalmapDepth = 1.0f)
{
    // assume N, the interpolated vertex normal and 
    // V, the view vector (vertex to eye)
    float2 nrmmap_xy = normalmap.Sample(samplerState, texcoord).xy * 2 - 1;
#if NORMAL_MAP_MODE == 2
    // flip G channel in DirectX Normals
	nrmmap_xy.y = -nrmmap_xy.y;
#endif
	nrmmap_xy.xy *= normalmapDepth;

    // Reconstruct the Z (Blue) channel using the Pythagorean theorem
    // saturate() prevents a negative square root if length(xy) happens to exceed 1.0
    float nrmmap_z = sqrt(saturate(1.0f - dot(nrmmap_xy, nrmmap_xy)));

    // 4. Combine them back into a full float3 normal vector
    float3 nrmmap = float3(nrmmap_xy, nrmmap_z);

	nrmmap = normalize(nrmmap);
	
    float3x3 TBN = cotangent_frame( N, -V, texcoord );
    return normalize( mul( nrmmap, TBN ) );
}

float3 perturb_normal( float3 N, float3 V, Texture2D normalmap, float2 texcoord, SamplerState samplerState, float normalmapDepth = 1.0f) {
#if NORMAL_MAP_RESTORE_Z == 1
    return perturb_normal_restore_z(N, V, normalmap, texcoord, samplerState, normalmapDepth);
#else
    return perturb_normal_rgb(N, V, normalmap, texcoord, samplerState, normalmapDepth);
#endif
}


// Base UV displacement at MI_ParallaxOcclusionStrength == 1.0. Material strength scales this.
static const float PARALLAX_HEIGHT_SCALE = 0.04f;

// Mode 1: simple parallax
// Mode 2: ray marching

#ifndef PARALLAX_MODE
#define PARALLAX_MODE 1
#endif

/** Simple (offset-limiting) parallax mapping: single-sample approximation that shifts the texcoord
    along the view direction by the height at the unshifted texcoord. Cheap, but shows swimming/
    warping at grazing angles and doesn't self-occlude. */
float2 parallax_simple_offset( float3 N, float3 V, Texture2D heightMap, float2 texcoord, SamplerState samplerState, float heightScale)
{
    float3x3 TBN = cotangent_frame( N, -V, texcoord );
    float3 Vts = normalize( mul(TBN, V) );

    float height = heightMap.SampleLevel(samplerState, texcoord, 0).w;
    float2 offset = (Vts.xy / max(abs(Vts.z), 0.2f)) * (height - 1.0f) * heightScale;
    return texcoord + offset;
}

/** Parallax Occlusion Mapping: marches along the view ray in tangent space using the height stored
    in the normalmap's alpha channel, and returns the displaced texcoord where the ray meets the
    surface. Callers should re-sample albedo/fx maps with this texcoord too - otherwise the height
    data only nudges shading and never visibly "pops". */
float2 parallax_raymarch_offset( float3 N, float3 V, Texture2D heightMap, float2 texcoord, SamplerState samplerState, float heightScale)
{
    float3x3 TBN = cotangent_frame( N, -V, texcoord );
    float3 Vts = normalize( mul(TBN, V) );

    const float minLayers = 8.0f;
    const float maxLayers = 32.0f;
    float numLayers = lerp( maxLayers, minLayers, abs(Vts.z) );
    float layerDepth = 1.0f / numLayers;
    float currentLayerDepth = 0.0f;

    // Direction to step the texcoord per layer, scaled by how much the surface should be displaced
    float2 P = (Vts.xy / max(abs(Vts.z), 0.2f)) * heightScale;
    float2 deltaTexcoord = P / numLayers;

    float2 currentTexcoord = texcoord;
    float currentDepth = 1.0f - heightMap.SampleLevel(samplerState, currentTexcoord, 0).w;

    int maxLayerCount = 32;
    for (int i = 0; i < maxLayerCount; i++)
    {
        if (currentLayerDepth >= currentDepth)
            break;

        currentTexcoord -= deltaTexcoord;
        currentDepth = 1.0f - heightMap.SampleLevel(samplerState, currentTexcoord, 0).w;
        currentLayerDepth += layerDepth;
    }

    // Interpolate between the layer that pierced the surface and the previous one for a smooth result
    float2 prevTexcoord = currentTexcoord + deltaTexcoord;
    float afterDepth = currentDepth - currentLayerDepth;
    float beforeDepth = (1.0f - heightMap.SampleLevel(samplerState, prevTexcoord, 0).w) - currentLayerDepth + layerDepth;
    float weight = afterDepth / (afterDepth - beforeDepth);
    return prevTexcoord * weight + currentTexcoord * (1.0f - weight);
}

/** Dispatches to the parallax implementation selected by PARALLAX_MODE (1 = simple, 2 = ray marching). */
float2 parallax_occlusion_offset( float3 N, float3 V, Texture2D heightMap, float2 texcoord, SamplerState samplerState, float heightScale)
{
#if PARALLAX_MODE == 1
    return parallax_simple_offset( N, V, heightMap, texcoord, samplerState, heightScale );
#else
    return parallax_raymarch_offset( N, V, heightMap, texcoord, samplerState, heightScale );
#endif
}
