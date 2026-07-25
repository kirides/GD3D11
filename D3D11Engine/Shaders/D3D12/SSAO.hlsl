// Forward+ has no GBuffer normals before lighting (see PBRLighting.hlsl), so this reconstructs a view-space
// normal from neighbouring depth samples (D3D11's CS_PFX_SAO SAO_RECONSTRUCT_NORMALS fallback) — same
// Alchemy-AO/SAO spiral-sample formula as D3D11's CS_PFX_SAO.hlsl, self-contained (no shared D3D11 headers;
// this file lives in the D3D12-only shader root). CSMain produces the raw AO estimate; CSBlur is a
// depth-aware separable blur, run twice (horizontal then vertical) to denoise it — mirrors
// D3D11's CS_PFX_SAO_Blur.hlsl pass-for-pass.

cbuffer SSAOCB : register( b0 )
{
    float2 ProjScale;     // (Proj._11, Proj._22): view->clip x/y scale (diagonal terms, layout-invariant)
    float2 ProjZ;         // (Proj._33, Proj._43): reversed-Z depth -> viewZ (viewZ = ProjZ.y / (depth - ProjZ.x))
    float  Radius;        // world-space AO sample radius
    float  Bias;          // normal bias to reduce self-occlusion
    float  Intensity;     // darkening strength
    int    NumSamples;    // spiral sample count
    float2 InvResolution; // 1/width, 1/height
    float2 _pad;
};

SamplerState    SS_Point : register( s0 );
Texture2D<float> DepthTex : register( t0 );   // prepass depth (reversed-Z)
RWTexture2D<float> OutputAO : register( u0 );

float3 ViewPosFromDepth( float2 uv, float depth )
{
    float2 ndc = uv * float2( 2.0, -2.0 ) + float2( -1.0, 1.0 );
    float viewZ = ProjZ.y / ( depth - ProjZ.x );
    return float3( ndc.x / ProjScale.x * viewZ, ndc.y / ProjScale.y * viewZ, viewZ );
}

float3 ViewPosAtOffset( float2 uv, float2 texelOffset )
{
    float2 sampleUV = uv + texelOffset * InvResolution;
    float d = DepthTex.SampleLevel( SS_Point, sampleUV, 0 );
    return ViewPosFromDepth( sampleUV, d );
}

// Picks the closer neighbour per axis to keep silhouettes crisp (matches D3D11's ReconstructViewNormal).
float3 ReconstructViewNormal( float3 centerPos, float2 uv )
{
    float3 l = ViewPosAtOffset( uv, float2( -1,  0 ) );
    float3 r = ViewPosAtOffset( uv, float2(  1,  0 ) );
    float3 d = ViewPosAtOffset( uv, float2(  0, -1 ) );
    float3 u = ViewPosAtOffset( uv, float2(  0,  1 ) );

    float3 dpdx = ( abs( r.z - centerPos.z ) < abs( centerPos.z - l.z ) ) ? ( r - centerPos ) : ( centerPos - l );
    float3 dpdy = ( abs( u.z - centerPos.z ) < abs( centerPos.z - d.z ) ) ? ( u - centerPos ) : ( centerPos - d );

    float3 n = normalize( cross( dpdx, dpdy ) );
    if ( dot( n, -normalize( centerPos ) ) < 0.0 ) n = -n;
    return n;
}

float2 GetSpiralSample( int index, int count )
{
    float r = sqrt( ( float( index ) + 0.5 ) / float( count ) );
    float theta = float( index ) * 2.39996323;   // golden angle
    return float2( r * cos( theta ), r * sin( theta ) );
}

float InterleavedGradientNoise( float2 pos )
{
    float3 magic = float3( 0.06711056, 0.00583715, 52.9829189 );
    return frac( magic.z * frac( dot( pos, magic.xy ) ) );
}

[numthreads(8, 8, 1)]
void CSMain( uint3 DTid : SV_DispatchThreadID )
{
    uint2 outSize;
    OutputAO.GetDimensions( outSize.x, outSize.y );
    if ( DTid.x >= outSize.x || DTid.y >= outSize.y ) return;

    float2 uv = ( float2( DTid.xy ) + 0.5 ) * InvResolution;
    float rawDepth = DepthTex.SampleLevel( SS_Point, uv, 0 );
    if ( rawDepth <= 0.0 )   // reversed-Z: 0 == cleared (sky / no opaque geometry)
    {
        OutputAO[DTid.xy] = 1.0;
        return;
    }

    float3 viewPos = ViewPosFromDepth( uv, rawDepth );
    float3 viewNormal = ReconstructViewNormal( viewPos, uv );

    float projScale = float( outSize.y ) / ( 2.0 * ProjScale.y );
    float screenRadius = max( Radius * projScale / abs( viewPos.z ), 3.0 );

    float rotAngle = InterleavedGradientNoise( float2( DTid.xy ) ) * 6.28318530718;
    float sinR, cosR;
    sincos( rotAngle, sinR, cosR );
    float2x2 rotMatrix = float2x2( cosR, -sinR, sinR, cosR );

    float occlusion = 0.0;
    int numSamples = max( NumSamples, 1 );
    for ( int i = 0; i < numSamples; i++ )
    {
        float2 spiralOffset = mul( GetSpiralSample( i, numSamples ), rotMatrix );
        float2 sampleUV = uv + spiralOffset * screenRadius * InvResolution;
        if ( sampleUV.x < 0.0 || sampleUV.x > 1.0 || sampleUV.y < 0.0 || sampleUV.y > 1.0 ) continue;

        float sampleDepth = DepthTex.SampleLevel( SS_Point, sampleUV, 0 );
        float3 sampleViewPos = ViewPosFromDepth( sampleUV, sampleDepth );

        float3 delta = sampleViewPos - viewPos;
        float dist2 = dot( delta, delta );
        float radiusSq = Radius * Radius;
        if ( dist2 < 0.0001 || dist2 > radiusSq ) continue;

        float invDist = rsqrt( dist2 );
        float NdotD = max( dot( viewNormal, delta * invDist ) - Bias, 0.0 );
        // Squared falloff (vs. linear) concentrates weight on close-range samples, so open/flat surfaces
        // (where occluders sit near the radius edge) stay near-unoccluded instead of picking up a uniform
        // wash — only genuine contact/crease geometry (close occluders) contributes strongly.
        float falloffLin = saturate( 1.0 - dist2 / radiusSq );
        float falloff = falloffLin * falloffLin;
        occlusion += NdotD * falloff;
    }

    occlusion = saturate( occlusion / numSamples );
    // Quadratic contact boost: adds occlusion^2 on top of the raw average. This is monotonically
    // non-decreasing (never darkens a pixel below the plain-average result the old formula produced),
    // but the added term grows fastest where raw occlusion is already high — real contact/crease
    // geometry — while barely nudging the small values typical of open/flat surfaces. (A smoothstep
    // curve was tried here first but it also *suppresses* every value below the 0.5 midpoint, i.e.
    // nearly every pixel pre-Intensity-scaling, which read as "SAO does almost nothing" — this additive
    // form avoids that failure mode by construction.)
    occlusion = occlusion + occlusion * occlusion;
    OutputAO[DTid.xy] = saturate( 1.0 - occlusion * Intensity );
}

cbuffer BlurCB : register( b0 )
{
    float2 Blur_InvResolution;
    float2 Blur_Direction;    // (1,0) horizontal, (0,1) vertical
    float2 Blur_ProjZ;        // same reversed-Z terms as SSAOCB.ProjZ, for depth-aware weighting
    float  Blur_Sharpness;
    float  _blurPad;
};

Texture2D<float> BlurAOTex    : register( t0 );   // this pass's AO input
Texture2D<float> BlurDepthTex : register( t1 );

static const int   kBlurRadius = 4;
static const float kGaussianWeights[5] = { 0.2270270270, 0.1945945946, 0.1216216216, 0.0540540541, 0.0162162162 };

float BlurLinearDepth( float2 uv )
{
    float d = BlurDepthTex.SampleLevel( SS_Point, uv, 0 );
    return Blur_ProjZ.y / ( d - Blur_ProjZ.x );
}

[numthreads(8, 8, 1)]
void CSBlur( uint3 DTid : SV_DispatchThreadID )
{
    uint2 outSize;
    OutputAO.GetDimensions( outSize.x, outSize.y );
    if ( DTid.x >= outSize.x || DTid.y >= outSize.y ) return;

    float2 uv = ( float2( DTid.xy ) + 0.5 ) * Blur_InvResolution;

    float centerAO = BlurAOTex.SampleLevel( SS_Point, uv, 0 );
    float centerDepth = BlurLinearDepth( uv );

    float totalAO = centerAO * kGaussianWeights[0];
    float totalWeight = kGaussianWeights[0];

    [unroll]
    for ( int i = 1; i <= kBlurRadius; i++ )
    {
        float2 offset = Blur_Direction * Blur_InvResolution * float( i );

        float2 uvP = uv + offset;
        float aoP = BlurAOTex.SampleLevel( SS_Point, uvP, 0 );
        float depthDiffP = abs( BlurLinearDepth( uvP ) - centerDepth );
        float weightP = kGaussianWeights[i] * exp( -depthDiffP * Blur_Sharpness );
        totalAO += aoP * weightP;
        totalWeight += weightP;

        float2 uvN = uv - offset;
        float aoN = BlurAOTex.SampleLevel( SS_Point, uvN, 0 );
        float depthDiffN = abs( BlurLinearDepth( uvN ) - centerDepth );
        float weightN = kGaussianWeights[i] * exp( -depthDiffN * Blur_Sharpness );
        totalAO += aoN * weightN;
        totalWeight += weightN;
    }

    OutputAO[DTid.xy] = totalAO / max( totalWeight, 0.001 );
}
