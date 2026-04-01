#define TILE_SIZE 16

struct TiledPointLight {
    float3 PositionView;
    float Range;
    float4 Color;
    float3 PositionWorld;
    int ShadowCubeIndex; // -1 = no shadow, else index into TextureCubeArray
};

struct LightGrid {
    uint Offset;
    uint Count;
};

cbuffer TiledShadingConstantBuffer : register( b0 ) {
    float2 ViewportSize;
    float2 Pad0;
    float4 ProjParams; // x = 1/P._11, y = 1/P._22, z = P._43, w = P._33
    uint LimitLightIntensity;
    uint NumTilesX;
    float2 Pad1;
    matrix InvView; // For world-space reconstruction (shadow sampling)
};

SamplerState SS_Linear : register( s0 );
SamplerComparisonState SS_Comp : register( s2 );
Texture2D TX_Diffuse : register( t0 );
Texture2D TX_Nrm : register( t1 );
Texture2D TX_Depth : register( t2 );
Texture2D TX_SI_SP : register( t7 );

StructuredBuffer<TiledPointLight> SB_Lights : register( t8 );
StructuredBuffer<LightGrid> SB_LightGrid : register( t9 );
StructuredBuffer<uint> SB_LightIndexList : register( t10 );

TextureCubeArray TX_ShadowCubeArray : register( t11 );

RWTexture2D<float4> RW_HDR : register( u0 );

float3 VSPositionFromDepth( float depth, float2 texCoord ) {
    float2 ndc = texCoord * float2( 2.0f, -2.0f ) + float2( -1.0f, 1.0f );
    float linearZ = ProjParams.z / (depth - ProjParams.w);
    return float3( ndc * ProjParams.xy * linearZ, linearZ );
}

float CalcBlinnPhongLighting( float3 N, float3 H ) {
    return saturate( dot( N, H ) );
}

// 8-tap PCF shadow sampling matching PS_DS_PointLightDynShadow.hlsl
static const int SHADOW_BLUR_COUNT = 8;
static const float3 SHADOW_BLUR_OFFSETS[SHADOW_BLUR_COUNT] = {
    float3( 0.054426466605825*2-1, 0.057144871008184*2-1, 0.57025665350736*2-1 ),
    float3( 0.32904030165125*2-1, 0.22406590786952*2-1, 0.76940122329136*2-1 ),
    float3( 0.90462177475198*2-1, 0.091382070021416*2-1, 0.0065345494107038*2-1 ),
    float3( 0.93540243382352*2-1, 0.61764284391778*2-1, 0.103979589466*2-1 ),
    float3( 0.44626536287659*2-1, 0.19266830440269*2-1, 0.73062449308607*2-1 ),
    float3( 0.0084832706528172*2-1, 0.83200742948428*2-1, 0.43927977813374*2-1 ),
    float3( 0.28579624476181*2-1, 0.57096250149001*2-1, 0.0095401159532089*2-1 ),
    float3( 0.55814247604373*2-1, 0.59385285228205*2-1, 0.44374119743879*2-1 )
};

float SampleShadowCube( float3 wsPosition, float3 lightPosWorld, float lightRange, int cubeIndex ) {
    float3 dir = normalize( wsPosition - lightPosWorld );
    float distance = length( wsPosition - lightPosWorld );
    float zFar = lightRange * 2.0f;
    distance = distance / zFar;

    float fixedBias = 0.005f;
    float fixedBlurScale = 0.010f;

    float shd = 0;
    [unroll] for ( int i = 0; i < SHADOW_BLUR_COUNT; i++ ) {
        float4 sampleCoord = float4( dir + SHADOW_BLUR_OFFSETS[i] * fixedBlurScale, (float)cubeIndex );
        shd += TX_ShadowCubeArray.SampleCmpLevelZero( SS_Comp, sampleCoord, distance - fixedBias );
    }
    shd /= SHADOW_BLUR_COUNT;
    return shd;
}

[numthreads( TILE_SIZE, TILE_SIZE, 1 )]
void CSMain( uint3 groupID : SV_GroupID, uint3 threadID : SV_GroupThreadID, uint3 dispatchThreadID : SV_DispatchThreadID ) {
    uint2 pixelCoord = dispatchThreadID.xy;

    if ( pixelCoord.x >= (uint)ViewportSize.x || pixelCoord.y >= (uint)ViewportSize.y )
        return;

    float2 uv = (float2( pixelCoord ) + 0.5f) / ViewportSize;

    // Read GBuffer
    float4 diffuse = TX_Diffuse.SampleLevel( SS_Linear, uv, 0 );
    float4 gb2 = TX_Nrm.SampleLevel( SS_Linear, uv, 0 );
    float3 normal = normalize( gb2.xyz );
    float4 gb3 = TX_SI_SP.SampleLevel( SS_Linear, uv, 0 );
    float specIntensity = gb3.x;
    float specPower = gb3.y;

    float expDepth = TX_Depth.SampleLevel( SS_Linear, uv, 0 ).r;
    float3 vsPosition = VSPositionFromDepth( expDepth, uv );

    // World-space position for shadow sampling (computed once, shared by all shadowed lights)
    float3 wsPosition = mul( float4( vsPosition, 1 ), InvView ).xyz;

    // Compute tile index
    uint tileX = pixelCoord.x / TILE_SIZE;
    uint tileY = pixelCoord.y / TILE_SIZE;
    uint tileIndex = tileY * NumTilesX + tileX;

    LightGrid grid = SB_LightGrid[tileIndex];

    float3 totalLighting = float3( 0, 0, 0 );
    float3 maxLighting = float3( 0, 0, 0 );

    for ( uint i = 0; i < grid.Count; i++ ) {
        uint lightIdx = SB_LightIndexList[grid.Offset + i];
        TiledPointLight light = SB_Lights[lightIdx];

        float3 lightDir = light.PositionView - vsPosition;
        float distance = length( lightDir );

        if ( distance >= light.Range )
            continue;

        lightDir /= distance;

        float ndl = max( 0, dot( lightDir, normal ) );
        float falloff = pow( saturate( 1.0f - (distance / light.Range) ), 1.2f );

        // Match legacy PS_DS_PointLight: V is computed from the light's view position,
        // not the pixel's. This is per-light, matching the legacy per-draw-call behavior.
        float3 V = normalize( -light.PositionView );
        float3 H = normalize( lightDir + V );
        float spec = CalcBlinnPhongLighting( normal, H );
        float specMod = pow( dot( float3( 0.333f, 0.333f, 0.333f ), diffuse.rgb ), 2 );
        float3 specBare = pow( spec, specPower ) * specIntensity * light.Color.rgb * falloff;
        float3 specColored = lerp( specBare, specBare * diffuse.rgb, specMod );

        float3 color = saturate( falloff * ndl * light.Color.rgb );
        float3 lighting = color * diffuse.rgb + specColored;

        // Apply shadow if this light has a shadow cubemap
        if ( light.ShadowCubeIndex >= 0 ) {
            float shadow = SampleShadowCube( wsPosition, light.PositionWorld, light.Range, light.ShadowCubeIndex );
            lighting *= shadow;
        }

        lighting = saturate( lighting );

        totalLighting += lighting;
        maxLighting = max( maxLighting, lighting );
    }

    float3 activeLighting = LimitLightIntensity ? maxLighting : totalLighting;
    if ( any( activeLighting > 0 ) ) {
        float4 existing = RW_HDR[pixelCoord];
        if ( LimitLightIntensity ) {
            // Match legacy MAX blend: each light uses max(light, existing) individually.
            // Since we see all lights at once, take the per-light max.
            RW_HDR[pixelCoord] = float4( max( existing.rgb, maxLighting ), existing.a );
        } else {
            RW_HDR[pixelCoord] = float4( existing.rgb + totalLighting, existing.a );
        }
    }
}
