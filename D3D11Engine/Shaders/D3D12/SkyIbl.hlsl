//--------------------------------------------------------------------------------------
// Compute Shader - Sky image-based lighting (Stage 1 of the environment-probe work).
//
// Builds the two cubemaps the Forward+ lit shaders use for INDIRECT light, replacing the
// flat greyscale ambient floor that ComputeSunLightingPBR used to apply
// (`albedo * AmbientStrength * sunLum`, see include/PBRLighting.hlsl):
//
//   CSSkyRadiance  -> env cube mip 0: the sky's radiance, evaluated ANALYTICALLY per texel.
//   CSPrefilter    -> env cube mips 1..N: GGX-importance-sampled, mip m == roughness m/(N-1).
//   CSIrradiance   -> a small cosine-convolved cube: the diffuse irradiance.
//
// Why analytic and not a scene capture: D3D12 draws Gothic's FIXED-FUNCTION skydome
// (D3D12GraphicsEngine::DrawSky — the atmospheric-scattering path is D3D11-only and not
// ported), so an atmospheric-scattering IBL would not match what is actually on screen.
// Evaluating a gradient built from Gothic's OWN sky state (zCSkyState master colours, fed
// in through SkyRadianceCB by D3D12SkyIbl.cpp) tracks time of day and weather exactly and
// costs one small dispatch instead of six scene re-renders. It also means there is nothing
// to place and nothing to author, which matters for a mod over 2002 game data.
//
// All colours arrive LINEAR (D3D12SkyIbl.cpp linearizes on the CPU — once per frame beats
// once per texel) and the cubes are RGBA16F, so no encode/decode happens here.
//--------------------------------------------------------------------------------------

SamplerState SS_LinearClamp : register( s0 );

// Source for the prefilter/irradiance passes: a mip-0-ONLY view of the env cube. It must be
// mip-0-only — the prefilter writes mips 1..N as a UAV in the same dispatch, and a full-chain
// SRV would put the same resource in two states at once. D3D12SkyIbl.cpp splits the barrier
// per subresource to match (mip 0 readable, the rest UNORDERED_ACCESS).
TextureCube TX_SkySource : register( t0 );

// Destination face slice. A cube UAV is always declared as a Texture2DArray of 6 slices;
// .z of the dispatch thread ID is the face index.
RWTexture2DArray<float4> OutputCube : register( u0 );

cbuffer SkyRadianceCB : register( b0 )
{
    float3 ZenithColor;    float SunSharpness;      // sky colour straight up; sun-lobe exponent
    float3 HorizonColor;   float SkyIntensity;      // colour at the horizon ring; overall scale
    float3 GroundColor;    float GroundBlend;       // below-horizon bounce colour; its falloff exponent
    float3 SunDirWS;       float SunLobeIntensity;  // direction TOWARD the sun; brightness of the disc
    float3 SunColor;       float FaceSize;          // linear sun colour; mip-0 face resolution
};

cbuffer PrefilterCB : register( b1 )
{
    float  P_FaceSize;     // resolution of the face being WRITTEN
    float  P_Roughness;    // GGX roughness this mip represents (0 at mip 0, 1 at the last mip)
    float  P_SourceSize;   // resolution of mip 0 (for the sample-mip heuristic below)
    uint   P_NumSamples;   // importance-sample count for this mip
};

static const float SKY_PI = 3.14159265;

// Cube-face texel -> world direction. Face order is the D3D convention (+X -X +Y -Y +Z -Z),
// which is what a TextureCube SRV over this array will read back.
float3 DirectionFromFaceUV( uint face, float2 uv )
{
    float2 st = uv * 2.0 - 1.0;   // [0,1] texel centre -> [-1,1] face coords
    float3 d;
    switch ( face )
    {
        case 0: d = float3(  1.0, -st.y, -st.x ); break;   // +X
        case 1: d = float3( -1.0, -st.y,  st.x ); break;   // -X
        case 2: d = float3(  st.x,  1.0,  st.y ); break;   // +Y
        case 3: d = float3(  st.x, -1.0, -st.y ); break;   // -Y
        case 4: d = float3(  st.x, -st.y,  1.0 ); break;   // +Z
        default: d = float3( -st.x, -st.y, -1.0 ); break;  // -Z
    }
    return normalize( d );
}

// The analytic sky. Gothic is Y-up (see include/Wetness.hlsl, which uses dot(N, (0,1,0)) for
// rain exposure), so d.y is the horizon parameter.
float3 SkyRadiance( float3 d )
{
    float t = d.y;
    float3 col;
    if ( t >= 0.0 )
    {
        // Above the horizon: horizon -> zenith. The 0.45 exponent keeps the bright horizon band
        // wide, which is what the sky actually looks like and what makes the reflection in a
        // polished surface read as "outdoors" rather than as a flat blue.
        col = lerp( HorizonColor, ZenithColor, pow( saturate( t ), 0.45 ) );
    }
    else
    {
        // Below the horizon: bounce off the ground. This hemisphere is the entire reason the
        // earlier hemispheric ambient experiment had to be reverted (see the note in
        // PBRLighting.hlsl's ComputeSunLightingPBR) — a saturate(N.y*0.5+0.5) factor drove
        // downward-facing normals to literal black because there was no ground term to catch
        // them. Cave ceilings and canopy undersides get their indirect light from here.
        col = lerp( HorizonColor, GroundColor, pow( saturate( -t ), GroundBlend ) );
    }

    // Soft sun disc, so a low-roughness surface catches an actual sun reflection in the
    // prefiltered chain instead of only the sky gradient.
    float sunCos = saturate( dot( d, SunDirWS ) );
    col += SunColor * SunLobeIntensity * pow( sunCos, SunSharpness );

    return col * SkyIntensity;
}

[numthreads( 8, 8, 1 )]
void CSSkyRadiance( uint3 tid : SV_DispatchThreadID )
{
    uint size = (uint)FaceSize;
    if ( tid.x >= size || tid.y >= size ) return;

    float2 uv = ( float2( tid.xy ) + 0.5 ) / FaceSize;
    float3 dir = DirectionFromFaceUV( tid.z, uv );
    OutputCube[tid] = float4( SkyRadiance( dir ), 1.0 );
}

// --- GGX prefilter (split-sum, Karis) ---------------------------------------------------

float RadicalInverse_VdC( uint bits )
{
    bits = ( bits << 16u ) | ( bits >> 16u );
    bits = ( ( bits & 0x55555555u ) << 1u ) | ( ( bits & 0xAAAAAAAAu ) >> 1u );
    bits = ( ( bits & 0x33333333u ) << 2u ) | ( ( bits & 0xCCCCCCCCu ) >> 2u );
    bits = ( ( bits & 0x0F0F0F0Fu ) << 4u ) | ( ( bits & 0xF0F0F0F0u ) >> 4u );
    bits = ( ( bits & 0x00FF00FFu ) << 8u ) | ( ( bits & 0xFF00FF00u ) >> 8u );
    return float( bits ) * 2.3283064365386963e-10;
}

float2 Hammersley( uint i, uint n )
{
    return float2( float( i ) / float( n ), RadicalInverse_VdC( i ) );
}

float3 ImportanceSampleGGX( float2 xi, float3 N, float roughness )
{
    float a = roughness * roughness;
    float phi = 2.0 * SKY_PI * xi.x;
    float cosTheta = sqrt( ( 1.0 - xi.y ) / max( 1.0 + ( a * a - 1.0 ) * xi.y, 1e-6 ) );
    float sinTheta = sqrt( saturate( 1.0 - cosTheta * cosTheta ) );

    float3 h = float3( sinTheta * cos( phi ), sinTheta * sin( phi ), cosTheta );

    float3 up = abs( N.z ) < 0.999 ? float3( 0, 0, 1 ) : float3( 1, 0, 0 );
    float3 tx = normalize( cross( up, N ) );
    float3 ty = cross( N, tx );
    return normalize( tx * h.x + ty * h.y + N * h.z );
}

[numthreads( 8, 8, 1 )]
void CSPrefilter( uint3 tid : SV_DispatchThreadID )
{
    uint size = (uint)P_FaceSize;
    if ( tid.x >= size || tid.y >= size ) return;

    float2 uv = ( float2( tid.xy ) + 0.5 ) / P_FaceSize;
    float3 N = DirectionFromFaceUV( tid.z, uv );
    // Split-sum approximation: assume N == V == R. Loses the stretched grazing-angle lobe, which
    // the analytic DFG term in PBRLighting.hlsl's EnvBRDFApprox partly compensates for.
    float3 V = N;

    float roughness = P_Roughness;
    float3 sum = 0.0;
    float  weight = 0.0;

    // Filtered-importance sampling: pick the source mip from the solid angle each sample covers
    // vs. the solid angle of one source texel. Without this a rough mip aliases badly at low
    // sample counts (the classic sparkling-sky artefact).
    float saTexel = 4.0 * SKY_PI / ( 6.0 * P_SourceSize * P_SourceSize );

    for ( uint i = 0; i < P_NumSamples; ++i )
    {
        float2 xi = Hammersley( i, P_NumSamples );
        float3 H = ImportanceSampleGGX( xi, N, roughness );
        float3 L = normalize( 2.0 * dot( V, H ) * H - V );

        float NdotL = dot( N, L );
        if ( NdotL <= 0.0 ) continue;

        float NdotH = saturate( dot( N, H ) );
        float VdotH = saturate( dot( V, H ) );

        float a = roughness * roughness;
        float a2 = a * a;
        float denom = NdotH * NdotH * ( a2 - 1.0 ) + 1.0;
        float D = a2 / max( SKY_PI * denom * denom, 1e-6 );
        float pdf = ( D * NdotH / max( 4.0 * VdotH, 1e-6 ) ) + 1e-4;

        float saSample = 1.0 / ( float( P_NumSamples ) * pdf );
        float mip = roughness <= 0.0 ? 0.0 : 0.5 * log2( saSample / saTexel );

        sum += TX_SkySource.SampleLevel( SS_LinearClamp, L, max( mip, 0.0 ) ).rgb * NdotL;
        weight += NdotL;
    }

    OutputCube[tid] = float4( sum / max( weight, 1e-4 ), 1.0 );
}

// --- Diffuse irradiance -----------------------------------------------------------------
// Cosine convolution by uniform hemisphere march. The destination is tiny (16^2 x 6 = 1536
// texels total), so the O(n^2) angular loop here is cheaper than setting up an SH projection
// and needs no cross-thread reduction. Runs only when the sky state actually changed.
[numthreads( 8, 8, 1 )]
void CSIrradiance( uint3 tid : SV_DispatchThreadID )
{
    uint size = (uint)P_FaceSize;
    if ( tid.x >= size || tid.y >= size ) return;

    float2 uv = ( float2( tid.xy ) + 0.5 ) / P_FaceSize;
    float3 N = DirectionFromFaceUV( tid.z, uv );

    float3 up = abs( N.z ) < 0.999 ? float3( 0, 0, 1 ) : float3( 1, 0, 0 );
    float3 tx = normalize( cross( up, N ) );
    float3 ty = cross( N, tx );

    const float kDelta = 0.05;   // ~1963 samples over the hemisphere
    float3 irradiance = 0.0;
    float  samples = 0.0;

    for ( float phi = 0.0; phi < 2.0 * SKY_PI; phi += kDelta )
    {
        float sinPhi, cosPhi;
        sincos( phi, sinPhi, cosPhi );
        for ( float theta = 0.0; theta < 0.5 * SKY_PI; theta += kDelta )
        {
            float sinTheta, cosTheta;
            sincos( theta, sinTheta, cosTheta );
            float3 tangentDir = float3( sinTheta * cosPhi, sinTheta * sinPhi, cosTheta );
            float3 worldDir = tangentDir.x * tx + tangentDir.y * ty + tangentDir.z * N;
            // cos(theta) is the Lambert weight, sin(theta) the solid-angle Jacobian.
            irradiance += TX_SkySource.SampleLevel( SS_LinearClamp, worldDir, 0 ).rgb * cosTheta * sinTheta;
            samples += 1.0;
        }
    }

    // PI * mean(L * cos * sin) is the standard normalization for this march; the result is the
    // irradiance E, which PBRLighting.hlsl multiplies by albedo/PI-folded-in kD.
    OutputCube[tid] = float4( SKY_PI * irradiance / max( samples, 1.0 ), 1.0 );
}
