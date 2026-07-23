cbuffer WorldCB : register(b0) { float4x4 ViewProj; };   // default column-major packing (see world shader)
cbuffer FogCB   : register(b1) { float3 FogColor; float FogNear; float3 CamPosWS; float FogFar; };
cbuffer LightCB : register(b2) { uint LightCount; uint NumTilesX; uint LimitLightIntensity; uint _lpad; };

// Forward+ tiled point lights (root-descriptor SRVs + per-tile grid)
struct GPULight { float3 PositionView; float Range; float4 Color; float3 PositionWorld; int ShadowCubeIndex; };
struct LightGrid { uint Offset; uint Count; };
StructuredBuffer<GPULight>  Lights        : register(t1);
StructuredBuffer<LightGrid> LightGridBuf  : register(t2);
StructuredBuffer<uint>      LightIndexBuf : register(t3);
#define TILE_SIZE 16u
#define MAX_LIGHTS_PER_TILE 32u

Texture2D    tx  : register(t0);
SamplerState smp : register(s0);

#define NUM_CSM_CASCADES 3
cbuffer ShadowCB : register(b3)
{
    float4x4 CascadeViewProj[NUM_CSM_CASCADES];
    float3   SunDirWS;          float ShadowMapSize;
    float3   SunColor;          float SunIntensity;
    float3   CascadeTexelWorld; float AmbientStrength;
    float    ShadowAOStrength;  float WorldAOStrength;  float2 _shpad;
};
Texture2DArray          ShadowMap : register(t4);
SamplerComparisonState  shadowCmp : register(s2);
cbuffer MaterialCB : register(b6) { uint MatNormalIndex; uint MatOrmIndex; };
TextureCubeArray        PointShadowCubes : register(t5);

float3 DelightDiffuse( float3 linearAlbedo )
{
    float luminance = dot( linearAlbedo, float3( 0.2126, 0.7152, 0.0722 ) );
    float delightFactor = 1.0 / max( sqrt( luminance + 1e-4 ), 0.2 );
    return saturate( linearAlbedo * lerp( 1.0, delightFactor, 0.5 ) );
}

float SamplePointShadow( int cubeIndex, float3 wpos, float3 N, float3 lightPos, float range )
{
    float3 d  = ( wpos + N * ( range * 0.01 ) ) - lightPos;
    float3 ad = abs( d );
    float  zView = max( ad.x, max( ad.y, ad.z ) );
    const float n = 15.0;
    float  f = range * 2.0;
    float  compareDepth = ( f / ( f - n ) ) * ( 1.0 - n / zView ) - 0.001;
    float3 L = normalize( d );

    float3 up = abs( L.y ) < 0.99 ? float3( 0, 1, 0 ) : float3( 1, 0, 0 );
    float3 t  = normalize( cross( up, L ) );
    float3 bt = cross( L, t );
    float  r  = 0.006 + 0.010 * saturate( zView / f );
    static const float2 kDisk[4] = { float2( 0.7, 0.7 ), float2( -0.7, 0.7 ), float2( 0.7, -0.7 ), float2( -0.7, -0.7 ) };
    float sh = 0.0;
    [unroll]
    for ( int s = 0; s < 4; ++s )
    {
        float3 o = normalize( L + ( kDisk[s].x * t + kDisk[s].y * bt ) * r );
        sh += PointShadowCubes.SampleCmpLevelZero( shadowCmp, float4( o, (float)cubeIndex ), compareDepth );
    }
    return sh * 0.25;
}

float ComputeSunShadow( float3 wpos, float3 N )
{
    const float margin = 1.5 / ShadowMapSize;
    const float texel  = 1.0 / ShadowMapSize;
    [unroll]
    for ( int c = 0; c < NUM_CSM_CASCADES; ++c )
    {
        float3 biased = wpos + N * ( CascadeTexelWorld[c] * 1.5 );
        float4 sp = mul( float4( biased, 1.0 ), CascadeViewProj[c] );
        float2 uv = sp.xy * float2( 0.5, -0.5 ) + 0.5;
        if ( uv.x > margin && uv.x < 1.0 - margin && uv.y > margin && uv.y < 1.0 - margin &&
             sp.z >= 0.0 && sp.z <= 1.0 )
        {
            float pcfStep = texel * ( 1.0 + float( c ) * 1.5 );
            float sh = 0.0;
            [unroll] for ( int y = -2; y <= 2; ++y )
            [unroll] for ( int x = -2; x <= 2; ++x )
                sh += ShadowMap.SampleCmpLevelZero( shadowCmp, float3( uv + float2( x, y ) * pcfStep, c ), sp.z - 0.0015 );
            return sh / 25.0;
        }
    }
    return 1.0;
}

static const float PBR_PI = 3.14159265;

float3 SrgbToLinear( float3 c )
{
    return select( c <= 0.04045, c / 12.92, pow( ( c + 0.055 ) / 1.055, 2.4 ) );
}

float  PBR_SafeRoughness( float r ) { return max( saturate( r ), 0.045 ); }
float  PBR_DistributionGGX( float NdotH, float roughness )
{
    float a = roughness * roughness;
    float a2 = a * a;
    float denom = NdotH * NdotH * ( a2 - 1.0 ) + 1.0;
    return a2 / max( PBR_PI * denom * denom, 1e-4 );
}
float  PBR_GeometrySchlickGGX( float NdotX, float roughness )
{
    float r = roughness + 1.0;
    float k = ( r * r ) / 8.0;
    return NdotX / max( NdotX * ( 1.0 - k ) + k, 1e-4 );
}
float  PBR_GeometrySmith( float NdotV, float NdotL, float roughness )
{
    return PBR_GeometrySchlickGGX( NdotV, roughness ) * PBR_GeometrySchlickGGX( NdotL, roughness );
}
float  PBR_Pow5( float x ) { float x2 = x * x; return x2 * x2 * x; }
float3 PBR_FresnelSchlick( float cosTheta, float3 F0 ) { return F0 + ( 1.0 - F0 ) * PBR_Pow5( saturate( 1.0 - cosTheta ) ); }

float3 PBR_DirectLighting( float3 baseColor, float3 lightColor, float3 N, float3 V, float3 L,
                           float roughness, float metallic, float attenuation )
{
    float NdotL = saturate( dot( N, L ) );
    float NdotV = saturate( dot( N, V ) );
    if ( NdotL <= 0.0 || NdotV <= 0.0 || attenuation <= 0.0 ) return 0.0;
    float3 H = normalize( V + L );
    float NdotH = saturate( dot( N, H ) );
    float VdotH = saturate( dot( V, H ) );
    float  cr = PBR_SafeRoughness( roughness * roughness );
    float  cm = saturate( metallic );
    float3 F0 = lerp( float3( 0.04, 0.04, 0.04 ), baseColor, cm );
    float  D = PBR_DistributionGGX( NdotH, cr );
    float  G = PBR_GeometrySmith( NdotV, NdotL, cr );
    float3 F = PBR_FresnelSchlick( VdotH, F0 );
    float3 specular = ( D * G * F ) / max( 4.0 * NdotV * NdotL, 1e-4 );
    float3 kD = ( 1.0 - F ) * ( 1.0 - cm );
    float3 diffuse = kD * baseColor / PBR_PI;
    return ( diffuse + specular ) * lightColor * ( NdotL * attenuation );
}

float3x3 CotangentFrame( float3 N, float3 p, float2 uv )
{
    float3 dp1 = ddx( p ), dp2 = ddy( p );
    float2 duv1 = ddx( uv ), duv2 = ddy( uv );
    float3 dp2perp = cross( dp2, N ), dp1perp = cross( N, dp1 );
    float3 T = dp2perp * duv1.x + dp1perp * duv2.x;
    float3 B = dp2perp * duv1.y + dp1perp * duv2.y;
    float handedness = ( duv1.x * duv2.y - duv1.y * duv2.x ) < 0.0 ? 1.0 : -1.0;
    T *= handedness;
    float invmax = rsqrt( max( dot( T, T ), dot( B, B ) ) );
    return float3x3( T * invmax, B * invmax, N );
}
float3 PerturbNormal( float3 N, float3 p, Texture2D nrmTex, float2 uv, SamplerState samp )
{
    float2 nxy = nrmTex.Sample( samp, uv ).xy * 2.0 - 1.0;
    nxy.y = -nxy.y;
    float  nz  = sqrt( saturate( 1.0 - dot( nxy, nxy ) ) );
    float3 nrm = normalize( float3( nxy, nz ) );
    return normalize( mul( nrm, CotangentFrame( N, p, uv ) ) );
}

// PBR sun lighting (stage 2 — matching DX11 lighting mix and ground/vertex lighting modulation)
float3 ComputeSunLightingPBR( float3 wpos, float3 N, float3 albedo, float vertLighting, float shadow,
                              float roughness, float metallic, float ao )
{
    float3 V = normalize( CamPosWS - wpos );
    float3 L = SunDirWS;                            // dir toward the sun (world space)
    float3 sunCol = SrgbToLinear( SunColor );
    float  sunLum = dot( sunCol, float3( 0.3333, 0.3333, 0.3333 ) );
    const float ssao = 1.0;

    // Direct sun term N.L
    float NdotL = saturate( dot( N, L ) );
    float sun = NdotL * shadow;

    // AO factors driven by Gothic's vertex/ground light
    float shadowAO = lerp( 1.0, vertLighting, ShadowAOStrength ) * ao;
    float worldAO  = lerp( 1.0, vertLighting, WorldAOStrength ) * ao;

    // Directional sky/ambient term: top-facing gets full sky ambient, vertical/undersides darken
    float skyAmbientDir = saturate( N.y * 0.5 + 0.5 ); // Hemispheric directional ambient factor
    float3 ambientSun = albedo * AmbientStrength * sunLum * shadowAO * skyAmbientDir * ssao;

    // Direct Sun term
    float sunAtten = sun * worldAO * SunIntensity;
    float3 directSun = PBR_DirectLighting( albedo, sunCol, N, V, L, roughness, metallic, sunAtten );

    return ambientSun + directSun;
}

float3 AccumTiledPointLights( float2 svpos, float3 wpos, float3 N, float3 albedo, float roughness, float metallic )
{
    uint2 tile = uint2( svpos ) / TILE_SIZE;
    uint  tileIndex = tile.y * NumTilesX + tile.x;
    LightGrid g = LightGridBuf[tileIndex];
    uint n = min( g.Count, MAX_LIGHTS_PER_TILE );
    float3 V = normalize( CamPosWS - wpos );
    float3 total = 0;
    float3 maxLit = 0;
    for ( uint k = 0; k < n; k++ )
    {
        GPULight L = Lights[ LightIndexBuf[g.Offset + k] ];
        float3 dir = L.PositionWorld - wpos;
        float dist = length( dir );
        if ( dist >= L.Range ) continue;
        dir /= dist;
        float nd  = saturate( 1.0 - dist / L.Range );
        float falloff = nd * ( nd * 0.2 + 0.8 );
        float3 lit = PBR_DirectLighting( albedo, L.Color.rgb, N, V, dir, roughness, metallic, falloff );
        if ( L.ShadowCubeIndex >= 0 )
        {
            float sh = SamplePointShadow( L.ShadowCubeIndex, wpos, N, L.PositionWorld, L.Range );
            float camDist = length( L.PositionView );
            float fade    = saturate( ( camDist - L.Range * 6.0 ) / ( L.Range * 3.0 ) );
            lit *= lerp( sh, 1.0, fade );
        }
        total += lit;
        maxLit = max( maxLit, lit );
    }
    return LimitLightIntensity != 0 ? maxLit : total;
}

struct VS_IN
{
    float3   pos     : POSITION;
    float3   nrm     : NORMAL;                  // ExVertexStruct object-space float3 normal (@12)
    float2   uv      : TEXCOORD0;
    float4x4 iworld  : INSTANCE_WORLD_MATRIX;
    float4   icolor  : INSTANCE_COLOR;
};
struct VS_OUT { float4 clip : SV_POSITION; float2 uv : TEXCOORD0; float4 col : TEXCOORD1; float fogDist : TEXCOORD2; float3 wpos : TEXCOORD3; float3 wnrm : TEXCOORD4; };

VS_OUT VSMain( VS_IN i )
{
    VS_OUT o;
    float3 worldPos = mul( float4( i.pos, 1.0 ), i.iworld ).xyz;
    o.clip = mul( float4( worldPos, 1.0 ), ViewProj );
    o.uv  = i.uv;
    o.col = i.icolor;
    o.wpos = worldPos;
    o.wnrm = mul( i.nrm, (float3x3)i.iworld );
    o.fogDist = length( worldPos - CamPosWS );
    return o;
}

float4 PSMain( VS_OUT i ) : SV_TARGET
{
    float4 t = tx.Sample( smp, i.uv );
    clip( t.a - 0.5 );
    float3 N = normalize( i.wnrm );
    if ( MatNormalIndex != 0xffffffff )
    {
        Texture2D nrmTex = ResourceDescriptorHeap[MatNormalIndex];
        N = PerturbNormal( N, i.wpos, nrmTex, i.uv, smp );
    }
    Texture2D ormTex = ResourceDescriptorHeap[MatOrmIndex];
    float3 orm = ormTex.Sample( smp, i.uv ).rgb;
    float3 albedo = SrgbToLinear( t.rgb );
    albedo = DelightDiffuse( albedo );
    float vertLighting = i.col.g;
    float shadow = ComputeSunShadow( i.wpos, N );
    float3 rgb = ComputeSunLightingPBR( i.wpos, N, albedo, vertLighting, shadow, orm.g, orm.b, orm.r );
    rgb += AccumTiledPointLights( i.clip.xy, i.wpos, N, albedo, orm.g, orm.b );
    float f = saturate( ( i.fogDist - FogNear ) / max( 1.0, FogFar - FogNear ) );
    return float4( lerp( rgb, SrgbToLinear( FogColor ), f ), 1.0 );
}

struct VS_DEPTH_IN  { float3 pos : POSITION; float2 uv : TEXCOORD0; float4x4 iworld : INSTANCE_WORLD_MATRIX; };
struct VS_DEPTH_OUT { float4 clip : SV_POSITION; float2 uv : TEXCOORD0; };
VS_DEPTH_OUT VSDepth( VS_DEPTH_IN i )
{
    VS_DEPTH_OUT o;
    float3 worldPos = mul( float4( i.pos, 1.0 ), i.iworld ).xyz;
    o.clip = mul( float4( worldPos, 1.0 ), ViewProj );
    o.uv = i.uv;
    return o;
}
float4 PSDepthClip( VS_DEPTH_OUT i ) : SV_TARGET
{
    float4 t = tx.Sample( smp, i.uv );
    clip( t.a - 0.5 );
    return float4( 0, 0, 0, 1 );
}
void PSShadowClip( VS_DEPTH_OUT i )
{
    clip( tx.Sample( smp, i.uv ).a - 0.5 );
}
