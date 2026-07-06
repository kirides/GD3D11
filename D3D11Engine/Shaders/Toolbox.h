
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
 
	// Negate because of left-handedness
	//T *= -1;
	//B *= -1;
 
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
	nrmmap.xy *= -1.0f;
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
