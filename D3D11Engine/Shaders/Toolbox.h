
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

// Samples the normal map into tangent space, honoring the NORMAL_MAP_MODE / NORMAL_MAP_RESTORE_Z
// conventions used above.
float3 sample_normal_ts( Texture2D normalmap, float2 texcoord, SamplerState samplerState, float normalmapDepth )
{
#if NORMAL_MAP_RESTORE_Z == 1
    float2 nrmmap_xy = normalmap.Sample( samplerState, texcoord ).xy * 2 - 1;
  #if NORMAL_MAP_MODE == 2
    nrmmap_xy.y = -nrmmap_xy.y;
  #endif
    nrmmap_xy *= normalmapDepth;
    float nrmmap_z = sqrt( saturate( 1.0f - dot( nrmmap_xy, nrmmap_xy ) ) );
    return normalize( float3( nrmmap_xy, nrmmap_z ) );
#else
    float3 nrmmap = normalmap.Sample( samplerState, texcoord ).xyz * 2 - 1;
  #if NORMAL_MAP_MODE == 2
    nrmmap.y = -nrmmap.y;
  #endif
    nrmmap.xy *= normalmapDepth;
    return normalize( nrmmap );
#endif
}

// Builds a TBN from a precomputed (MikkTSpace) vertex tangent. tangent.w = bitangent handedness.
// N and tangent.xyz must be in the same space (view space here).
float3x3 tangent_frame_explicit( float3 N, float3 T, float sign )
{
    // Gram-Schmidt: re-orthonormalize the interpolated tangent against the interpolated normal.
    T = normalize( T - N * dot( N, T ) );
    // Handedness is controlled in ONE place: the packer stores -tangent.w (OpenGL Y+ convention).
    // If normal maps still look inverted vs the ddx/ddy path, flip the sign in VertexPacking.h
    // (EncodeTangent call) rather than adding a second negation here.
    float3 B = cross( N, T ) * sign;
    return float3x3( T, B, N );
}

// perturb_normal variant that prefers a precomputed vertex tangent and falls back to the
// screen-space-derivative frame when the tangent is absent (degenerate / zero) — this lets
// geometry that doesn't yet supply a tangent (e.g. VOBs) keep the old behavior.
float3 perturb_normal( float3 N, float3 V, float4 vertexTangent, Texture2D normalmap, float2 texcoord, SamplerState samplerState, float normalmapDepth = 1.0f )
{
    [branch] if ( dot( vertexTangent.xyz, vertexTangent.xyz ) < 0.25f )
    {
        return perturb_normal( N, V, normalmap, texcoord, samplerState, normalmapDepth );
    }

    float3 nrmmap = sample_normal_ts( normalmap, texcoord, samplerState, normalmapDepth );
    float3x3 TBN = tangent_frame_explicit( normalize( N ), vertexTangent.xyz, vertexTangent.w );
    return normalize( mul( nrmmap, TBN ) );
}
