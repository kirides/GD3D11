// D3D12 rain/snow billboard draw — DXC/SM6 port of VS_ParticlePointShaded.hlsl + PS_Rain.hlsl.
// No input-assembler vertex/instance buffers at all: both particle buffers are read as plain
// StructuredBuffers via root SRV (t0 dynamic {position,velocity}, t1 immutable {seed,brightness,
// drawMode}), indexed by SV_InstanceID — DrawInstanced(4, numParticles) with an empty input layout
// expands each instance into a camera-facing quad from SV_VertexID, mirroring the D3D11 VS's
// right/up-from-velocity construction. This is a D3D12-side simplification (D3D11 needs a real vertex
// buffer + step-rate-1 input layout because the FL10 stream-out fallback shares the same VB); D3D12 has
// no such fallback, so the extra machinery isn't needed.
//
// The VS also samples the rain shadowmap (IsWet(), a real port of D3D11's VS_ParticlePointShaded.hlsl
// function of the same name) to zero out indoor/roofed raindrops — a single-slice, normal-Z depth map of
// the world mesh plus the instanced VOBs (so tree canopies and wagons shelter what is under them too),
// rendered by D3D12GraphicsEngine::RecordRainShadowmap and read bindlessly here
// (ResourceDescriptorHeap[RainShadowSrvIndex]) with a static comparison sampler.
//
// The PS ports D3D11's rainResponse() unchanged (same g_rainfactors table + view/light-relative texture
// index math) but reads the rain/snow Texture2DArray bindlessly (ResourceDescriptorHeap[TexArrayIndex])
// instead of a bound t0 — the array choice (rain vs. snow) is a per-draw root const, not a shader
// permutation, since the two only differ by which array is bound and a tint tweak.

cbuffer ViewProjCB : register( b0 )
{
    float4x4 ViewProj;
};

cbuffer RainInfoCB : register( b1 )
{
    float3 CameraPosition;
    float  RainFxWeight;
    float  RainHeight;
    float3 RainDirection;   // normalized global-velocity direction (the raindrop/snowflake "fall axis" — D3D11's dropDir/AR_GlobalVelocity), used by the PS's view/light angle math
    float2 RainScale;
};

cbuffer RainTexCB : register( b2 )
{
    uint TexArrayIndex;   // SRV heap slot of the active (rain or snow) Texture2DArray
    uint IsSnow;
};

cbuffer RainShadowCB : register( b3 )
{
    float4x4 RainShadowViewProj;
    uint     RainShadowSrvIndex;
};

struct RainParticleDynamic
{
    float3 position;
    float3 velocity;
};

struct RainParticleStatic
{
    float3 seed;
    float  randomBrightness;
    int    drawMode;
};

StructuredBuffer<RainParticleDynamic> DynamicData : register( t0 );
StructuredBuffer<RainParticleStatic>  StaticData  : register( t1 );

SamplerState SS_Wrap : register( s0 );
SamplerComparisonState SS_ShadowComp : register( s1 );

// Verbatim from D3D11's VS_ParticlePointShaded.hlsl IsWet() — projects the world position into the rain
// shadowmap's clip space and compares against the stored (world-mesh) occluder depth.
float IsWet( float3 wsPosition )
{
    Texture2D shadowTex = ResourceDescriptorHeap[RainShadowSrvIndex];

    float4 clip = mul( float4( wsPosition, 1.0f ), RainShadowViewProj );
    clip.xyz /= clip.w;

    float2 uv;
    uv.x = clip.x / 2.0f + 0.5f;
    uv.y = clip.y / -2.0f + 0.5f;

    const float bias = -0.001f;
    return shadowTex.SampleCmpLevelZero( SS_ShadowComp, uv, clip.z - bias );
}

struct PS_INPUT
{
    float4 vDiffuse       : DIFFUSE;
    nointerpolation uint  vType : TYPE;
    float2 vTexcoord      : TEXCOORD0;
    float3 vNormal        : NORMAL;
    float3 vWorldPosition : WORLDPOS;
    float4 vPosition      : SV_POSITION;
};

static const float tu[4] = { 0.0, 1.0, 0.0, 1.0 };
static const float tv[4] = { 1.0, 1.0, 0.0, 0.0 };
static const float vr[4] = { -1.0,  1.0, -1.0, 1.0 };
static const float vu[4] = { -1.0, -1.0,  1.0, 1.0 };

PS_INPUT VSMain( uint vertexID : SV_VertexID, uint instanceID : SV_InstanceID )
{
    RainParticleDynamic dyn = DynamicData[instanceID];
    RainParticleStatic  stat = StaticData[instanceID];

    // Check if we even have to render this raindrop (matches D3D11's IsWet() call site exactly — sampled
    // against the RAW particle position, before the billboard right/up offset below).
    const float wet = IsWet( dyn.position );

    float3 planeNormal = normalize( CameraPosition - dyn.position );
    float3 upVector = normalize( dyn.velocity );
    float3 rightVector = normalize( cross( planeNormal, upVector ) );
    upVector = normalize( cross( planeNormal, rightVector ) );

    rightVector *= RainScale.x;
    upVector *= RainScale.y;

    const float brightness = stat.randomBrightness * wet;

    float3 position = dyn.position;
    position += rightVector * vr[vertexID];
    position += upVector * vu[vertexID];

    PS_INPUT o;
    o.vPosition = mul( float4( position, 1.0f ), ViewProj );
    o.vTexcoord = float2( tu[vertexID], tv[vertexID] );
    o.vDiffuse = float4( 0.0f, 0.0f, 0.0f, brightness );
    o.vType = (uint)stat.drawMode;
    o.vNormal = planeNormal;
    o.vWorldPosition = position;

    // Fades particle count in/out with weather strength (matches D3D11's drawMode-upper-bits threshold):
    // a degenerate clip-space position (w = -1) drops the particle entirely instead of drawing it dim.
    const uint rand = (uint)(stat.drawMode) >> 16 & 0xFFFF;
    if ( (float)rand > pow( RainFxWeight, 3.0f ) * 0xFFFF )
        o.vPosition = float4( 0.0f, 0.0f, 0.0f, -1.0f );

    return o;
}

// Verbatim from D3D11's PS_Rain.hlsl (Shaders/PS_Rain.hlsl) — per-texture normalization factors for the
// 370 raindrop lookup textures (index 0..369; the snow array reuses the same table by construction,
// since it's indexed by the same verticalIndex*90+horizontalIndex*10+type scheme, capped at 256 below).
static const float g_rainfactors[370] =
    {
        0.004535 , 0.014777 , 0.012512 , 0.130630 , 0.013893 , 0.125165 , 0.011809 , 0.244907 , 0.010722 , 0.218252,
        0.011450 , 0.016406 , 0.015855 , 0.055476 , 0.015024 , 0.067772 , 0.021120 , 0.118653 , 0.018705 , 0.142495,
        0.004249 , 0.017267 , 0.042737 , 0.036384 , 0.043433 , 0.039413 , 0.058746 , 0.038396 , 0.065664 , 0.054761,
        0.002484 , 0.003707 , 0.004456 , 0.006006 , 0.004805 , 0.006021 , 0.004263 , 0.007299 , 0.004665 , 0.007037,
        0.002403 , 0.004809 , 0.004978 , 0.005211 , 0.004855 , 0.004936 , 0.006266 , 0.007787 , 0.006973 , 0.007911,
        0.004843 , 0.007565 , 0.007675 , 0.011109 , 0.007726 , 0.012165 , 0.013179 , 0.021546 , 0.013247 , 0.012964,
        0.105644 , 0.126661 , 0.128746 , 0.101296 , 0.123779 , 0.106198 , 0.123470 , 0.129170 , 0.116610 , 0.137528,
        0.302834 , 0.379777 , 0.392745 , 0.339152 , 0.395508 , 0.334227 , 0.374641 , 0.503066 , 0.387906 , 0.519618,
        0.414521 , 0.521799 , 0.521648 , 0.498219 , 0.511921 , 0.490866 , 0.523137 , 0.713744 , 0.516829 , 0.743649,
        0.009892 , 0.013868 , 0.034567 , 0.025788 , 0.034729 , 0.036399 , 0.030606 , 0.017303 , 0.051809 , 0.030852,
        0.018874 , 0.027152 , 0.031625 , 0.023033 , 0.038150 , 0.024483 , 0.029034 , 0.021801 , 0.037730 , 0.016639,
        0.002868 , 0.004127 , 0.133022 , 0.013847 , 0.123368 , 0.012993 , 0.122183 , 0.015031 , 0.126043 , 0.015916,
        0.002030 , 0.002807 , 0.065443 , 0.002752 , 0.069440 , 0.002810 , 0.081357 , 0.002721 , 0.076409 , 0.002990,
        0.002425 , 0.003250 , 0.003180 , 0.011331 , 0.002957 , 0.011551 , 0.003387 , 0.006086 , 0.002928 , 0.005548,
        0.003664 , 0.004258 , 0.004269 , 0.009404 , 0.003925 , 0.009233 , 0.004224 , 0.009405 , 0.004014 , 0.008435,
        0.038058 , 0.040362 , 0.035946 , 0.072104 , 0.038315 , 0.078789 , 0.037069 , 0.077795 , 0.042554 , 0.073945,
        0.124160 , 0.122589 , 0.121798 , 0.201886 , 0.122283 , 0.214549 , 0.118196 , 0.192104 , 0.122268 , 0.209397,
        0.185212 , 0.181729 , 0.194527 , 0.420721 , 0.191558 , 0.437096 , 0.199995 , 0.373842 , 0.192217 , 0.386263,
        0.003520 , 0.053502 , 0.060764 , 0.035197 , 0.055078 , 0.036764 , 0.048231 , 0.052671 , 0.050826 , 0.044863,
        0.002254 , 0.023290 , 0.082858 , 0.043008 , 0.073780 , 0.035838 , 0.080650 , 0.071433 , 0.073493 , 0.026725,
        0.002181 , 0.002203 , 0.112864 , 0.060140 , 0.115635 , 0.065531 , 0.093277 , 0.094123 , 0.093125 , 0.144290,
        0.002397 , 0.002369 , 0.043241 , 0.002518 , 0.040455 , 0.002656 , 0.002540 , 0.090915 , 0.002443 , 0.101604,
        0.002598 , 0.002547 , 0.002748 , 0.002939 , 0.002599 , 0.003395 , 0.002733 , 0.003774 , 0.002659 , 0.004583,
        0.003277 , 0.003176 , 0.003265 , 0.004301 , 0.003160 , 0.004517 , 0.003833 , 0.008354 , 0.003140 , 0.009214,
        0.008558 , 0.007646 , 0.007622 , 0.026437 , 0.007633 , 0.021560 , 0.007622 , 0.017570 , 0.007632 , 0.018037,
        0.031062 , 0.028428 , 0.028428 , 0.108300 , 0.028751 , 0.111013 , 0.028428 , 0.048661 , 0.028699 , 0.061490,
        0.051063 , 0.047597 , 0.048824 , 0.129541 , 0.045247 , 0.124975 , 0.047804 , 0.128904 , 0.045053 , 0.119087,
        0.002197 , 0.002552 , 0.002098 , 0.200688 , 0.002073 , 0.102060 , 0.002111 , 0.163116 , 0.002125 , 0.165419,
        0.002060 , 0.002504 , 0.002105 , 0.166820 , 0.002117 , 0.144274 , 0.005074 , 0.143881 , 0.004875 , 0.205333,
        0.001852 , 0.002184 , 0.002167 , 0.163804 , 0.002132 , 0.212644 , 0.003431 , 0.244546 , 0.004205 , 0.315848,
        0.002450 , 0.002360 , 0.002243 , 0.154635 , 0.002246 , 0.148259 , 0.002239 , 0.348694 , 0.002265 , 0.368426,
        0.002321 , 0.002393 , 0.002376 , 0.074124 , 0.002439 , 0.126918 , 0.002453 , 0.439270 , 0.002416 , 0.489812,
        0.002484 , 0.002629 , 0.002559 , 0.150246 , 0.002579 , 0.140103 , 0.002548 , 0.493103 , 0.002637 , 0.509481,
        0.002960 , 0.002952 , 0.002880 , 0.294884 , 0.002758 , 0.332805 , 0.002727 , 0.455842 , 0.002816 , 0.431807,
        0.003099 , 0.003028 , 0.002927 , 0.387154 , 0.002899 , 0.397946 , 0.002957 , 0.261333 , 0.002909 , 0.148548,
        0.004887 , 0.004884 , 0.006581 , 0.414647 , 0.003735 , 0.431317 , 0.006426 , 0.148997 , 0.003736 , 0.080715,
        0.001969 , 0.002159 , 0.002325 , 0.200211 , 0.002288 , 0.202137 , 0.002289 , 0.595331 , 0.002311 , 0.636097
    };

// Verbatim from D3D11's rainResponse() (Shaders/PS_Rain.hlsl) — samples the lookup-texture array with a
// view/light-relative index to approximate anisotropic raindrop/snowflake glinting.
float4 RainResponse( Texture2DArray texArray, uint type, float2 vTexcoord,
    float3 lightVector, float lightIntensity, float3 lightColor, float3 eyeVector, float3 dropDir )
{
    float opacity = 0.0f;
    float fallOff = 1.0f;

    if ( fallOff > 0.01 && lightIntensity > 0.01 )
    {
        float3 L = normalize( lightVector );
        float3 E = normalize( eyeVector );
        float3 N = normalize( dropDir );

        bool is_EpLp_angle_ccw = true;
        float hangle = 0.0f;
        float vangle = abs( (acos( dot( L, N ) ) * 180.0f / 3.14159265f) - 90.0f );

        {
            float3 Lp = normalize( L - dot( L, N ) * N );
            float3 Ep = normalize( E - dot( E, N ) * N );
            hangle = acos( dot( Ep, Lp ) ) * 180.0f / 3.14159265f;
            hangle = (hangle - 10.0f) / 20.0f;
            is_EpLp_angle_ccw = dot( N, cross( Ep, Lp ) ) > 0.0f;
        }

        if ( vangle >= 88.0f )
        {
            hangle = 0.0f;
            is_EpLp_angle_ccw = true;
        }

        vangle = (vangle - 10.0f) / 20.0f;
        hangle = clamp( hangle, -0.5f, 8.5f );

        const int MAX_VIDX = 4;
        const int MAX_HIDX = 8;

        int verticalLightIndex1 = (int)floor( vangle );
        int verticalLightIndex2 = min( MAX_VIDX, verticalLightIndex1 + 1 );
        verticalLightIndex1 = max( 0, verticalLightIndex1 );
        float t = frac( vangle );

        float textureCoordsH1 = vTexcoord.x;
        float textureCoordsH2 = vTexcoord.x;

        int horizontalLightIndex1 = 0;
        int horizontalLightIndex2 = 0;
        float s = frac( hangle );
        horizontalLightIndex1 = (int)floor( hangle );
        horizontalLightIndex2 = horizontalLightIndex1 + 1;
        if ( horizontalLightIndex1 < 0 )
        {
            horizontalLightIndex1 = 0;
            horizontalLightIndex2 = 0;
        }

        if ( is_EpLp_angle_ccw )
        {
            if ( horizontalLightIndex2 > MAX_HIDX )
            {
                horizontalLightIndex2 = MAX_HIDX;
                textureCoordsH2 = 1.0f - textureCoordsH2;
            }
        }
        else
        {
            textureCoordsH1 = 1.0f - textureCoordsH1;
            if ( horizontalLightIndex2 > MAX_HIDX )
            {
                horizontalLightIndex2 = MAX_HIDX;
            }
            else
            {
                textureCoordsH2 = 1.0f - textureCoordsH2;
            }
        }

        if ( verticalLightIndex1 >= MAX_VIDX )
        {
            textureCoordsH2 = vTexcoord.x;
            horizontalLightIndex1 = 0;
            horizontalLightIndex2 = 0;
            s = 0.0f;
        }

        uint2 texIndicesV1 = uint2( verticalLightIndex1 * 90 + horizontalLightIndex1 * 10 + type,
                                     verticalLightIndex1 * 90 + horizontalLightIndex2 * 10 + type );
        float3 tex1 = float3( textureCoordsH1, vTexcoord.y, texIndicesV1.x );
        float3 tex2 = float3( textureCoordsH2, vTexcoord.y, texIndicesV1.y );
        if ( (verticalLightIndex1 < 4) && (verticalLightIndex2 >= 4) )
        {
            s = 0.0f;
            horizontalLightIndex1 = 0;
            horizontalLightIndex2 = 0;
            textureCoordsH1 = vTexcoord.x;
            textureCoordsH2 = vTexcoord.x;
        }

        uint2 texIndicesV2 = uint2( verticalLightIndex2 * 90 + horizontalLightIndex1 * 10 + type,
                                     verticalLightIndex2 * 90 + horizontalLightIndex2 * 10 + type );
        float3 tex3 = float3( textureCoordsH1, vTexcoord.y, texIndicesV2.x );
        float3 tex4 = float3( textureCoordsH2, vTexcoord.y, texIndicesV2.y );

        const float v2MaxFactor = 0.010f;

        float col1 = texArray.SampleLevel( SS_Wrap, tex1, 0 ).r * min( g_rainfactors[texIndicesV1.x], v2MaxFactor );
        float col2 = texArray.SampleLevel( SS_Wrap, tex2, 0 ).r * min( g_rainfactors[texIndicesV1.y], v2MaxFactor );
        float col3 = texArray.SampleLevel( SS_Wrap, tex3, 0 ).r * min( g_rainfactors[texIndicesV2.x], v2MaxFactor );
        float col4 = texArray.SampleLevel( SS_Wrap, tex4, 0 ).r * min( g_rainfactors[texIndicesV2.y], v2MaxFactor );

        float hOpacity1 = min( 1.0f, lerp( col1, col2, s ) );
        float hOpacity2 = min( 1.0f, lerp( col3, col4, s ) );

        opacity = lerp( hOpacity1, hOpacity2, t );
        opacity = pow( opacity, 0.7f );
        opacity = 4.0f * lightIntensity * opacity * fallOff;
    }

    return float4( lightColor, opacity );
}

float4 PSMain( PS_INPUT i ) : SV_TARGET0
{
    Texture2DArray texArray = ResourceDescriptorHeap[TexArrayIndex];

    const float globalLighting = 1.0f;
    const float3 lightPos = normalize( float3( 0.333f, 0.433f, 0.333f ) ) * 10000.0f;
    const float3 eyeVector = CameraPosition - i.vWorldPosition;   // matches D3D11's PSMain call site exactly

    float4 response = RainResponse( texArray, i.vType & 0xFFFF, i.vTexcoord,
        lightPos, globalLighting * i.vDiffuse.a, float3( 1.0f, 1.0f, 1.0f ), eyeVector, RainDirection );

    if ( IsSnow != 0 )
    {
        // Slight white-ish tint, matches D3D11's SNOW_FEATURE branch in PS_Rain.hlsl.
        response = float4( 1.0f - (1.0f / 255.0f), 1.0f - (1.0f / 250.0f), 1.0f - (1.0f / 250.0f), response.w );
    }

    return response;
}
