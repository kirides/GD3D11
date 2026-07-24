// Local-space wind sway ported verbatim (math-for-math) from the D3D11 SHD_WIND/SHD_INFLUENCE blocks in
// Shaders/VS_ExInstancedObj.hlsl. Pure functions taking explicit parameters (no globals) so any D3D12 shader
// can reuse them without having to declare an identical constant buffer — the caller owns its own WindCB.

static const float kWindTrunkStiffness = 0.12f;   // bottom 12% of height stays put, tree-trunk-like
static const float kWindPhaseVariation = 0.40f;
static const float kWindStrengthMult   = 16.0f;   // internal scale-up of the CPU windStrength value
static const float kWindPI2            = 6.283185f;

float WindInstancePhaseOffset( float4x4 instMatrix, float maxHeightValue )
{
    // Deterministic per-instance pseudo-random phase, seeded from the instance's world matrix + max height.
    float seed = dot( float3( instMatrix._11, instMatrix._22, instMatrix._33 ), float3( 12.9898, 78.233, 53.539 ) ) + maxHeightValue;
    return frac( sin( seed ) * 43758.5453 ) * kWindPhaseVariation;
}

// Local-space offset to add to a vertex swaying in the wind (flags, foliage, leaves).
float3 ApplyTreeWind( float3 localPos, float3 direction, float heightNorm, float timeSec, float4x4 instMatrix, float maxHeightValue, float windStrength )
{
    float shouldAffect   = saturate( sign( heightNorm - kWindTrunkStiffness + 0.0001f ) );
    float instancePhase  = WindInstancePhaseOffset( instMatrix, maxHeightValue ) * kWindPI2;
    float adjustedHeight = saturate( ( heightNorm - kWindTrunkStiffness ) / ( 1.0 - kWindTrunkStiffness ) ) * shouldAffect;
    float heightFactor   = pow( adjustedHeight, 2.6f );

    float mainWave       = sin( timeSec * 1.0 + heightNorm * 3.0 + instancePhase ) * 0.8;
    float secondaryWave  = cos( timeSec * 0.7 + heightNorm * 5.0 + instancePhase * 1.5 ) * 0.80;
    float inertiaEffect  = sin( timeSec * 0.3 + heightNorm * 8.0 ) * 0.1;
    float topSmoothing   = smoothstep( 0.7, 0.9, adjustedHeight );
    float combinedWave   = ( mainWave + secondaryWave * 0.5 ) * ( 1.0 - topSmoothing * 0.3 ) + inertiaEffect * topSmoothing;
    float leafTurbulence = ( sin( timeSec * 4.0 + localPos.x * 15.0 ) + cos( timeSec * 3.7 + localPos.z * 12.0 ) ) * 0.05 * topSmoothing;

    return direction * windStrength * kWindStrengthMult * ( combinedWave + leafTurbulence ) * heightFactor;
}

static const float kHeroAffectRange    = 100.0f;
static const float kHeroAffectStrength = 38.0f;

// Local-space displacement pushing a vertex away from the player ("hero moves the bushes").
float3 ApplyHeroInfluence( float3 playerPosWS, float3 vertexLocalPos, float minHeightValue, float maxHeightValue, float4x4 instWorldMatrix )
{
    float heightRange      = max( maxHeightValue - minHeightValue, 0.001 );
    float vertexHeightNorm = saturate( ( vertexLocalPos.y - minHeightValue ) / heightRange );
    float heightMask       = smoothstep( 0.14, 0.16, vertexHeightNorm );   // only the top ~85% of height reacts

    float3 vertexWorldPos = mul( float4( vertexLocalPos, 1.0 ), instWorldMatrix ).xyz;
    float3 toVertex = vertexWorldPos - playerPosWS;

    // Branch instead of normalize(): normalize(~0) is Inf/NaN when the vertex sits on top of the player.
    float toVertexLen = length( toVertex );
    float3 displaceDirWorld = toVertexLen >= 0.001 ? ( toVertex / toVertexLen ) : float3( 0, 1, 0 );

    float distanceXZ = length( toVertex.xz );
    float distanceFactor = exp( -(distanceXZ * distanceXZ) / (1.8 * kHeroAffectRange * kHeroAffectRange) );

    float influence = distanceFactor * vertexHeightNorm * heightMask;
    float randomOffset = frac( sin( dot( vertexLocalPos.xz, float2( 12.9898, 78.233 ) ) ) * 43758.5453 );
    influence *= 0.9 + 0.1 * randomOffset;

    float3 displaceDirLocal = normalize( mul( displaceDirWorld, (float3x3)instWorldMatrix ) );
    return displaceDirLocal * kHeroAffectStrength * influence;
}
