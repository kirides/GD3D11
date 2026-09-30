// D3D12 low clouds (include/LowClouds.hlsl): CSGenerate ray-marches the half-resolution layer, PSComposite blends
// it onto the linear HDR scene. The layer is stored in gamma space, like D3D11, and linearized on the way out.

#include "include/AtmosphericScattering.hlsl"   // the Atmosphere cbuffer (b1)

// Generate: Idx0 = scene depth SRV, Idx1 = cloud UAV, Idx2 = footprint depth UAV, Idx3 = sky cloud UAV
// Composite: Idx0 = cloud SRV, Idx1 = footprint depth SRV, Idx2 = sky cloud SRV, Idx3 = scene depth SRV
cbuffer LowCloudPassCB : register(b0) { uint Idx0; uint Idx1; uint Idx2; uint Idx3; };

float4 LowCloudLoadLayer( int2 p ) { Texture2D t = ResourceDescriptorHeap[Idx0]; return t.Load( int3( p, 0 ) ); }
float LowCloudLoadDepth( int2 p ) { Texture2D<float> t = ResourceDescriptorHeap[Idx1]; return t.Load( int3( p, 0 ) ); }
float4 LowCloudLoadSky( int2 p ) { Texture2D t = ResourceDescriptorHeap[Idx2]; return t.Load( int3( p, 0 ) ); }
int2 LowCloudLayerSize()
{
    Texture2D t = ResourceDescriptorHeap[Idx0];
    uint width, height;
    t.GetDimensions( width, height );
    return max( int2( width, height ), int2( 1, 1 ) );
}

#define LOW_CLOUD_CB_REGISTER b2
#define LOW_CLOUDS_COMPOSITE
#include "../include/LowClouds.hlsl"

[numthreads( 8, 8, 1 )]
void CSGenerate( uint3 id : SV_DispatchThreadID )
{
    RWTexture2D<float4> cloudOut = ResourceDescriptorHeap[Idx1];
    RWTexture2D<float>  depthOut = ResourceDescriptorHeap[Idx2];
    RWTexture2D<float4> skyOut   = ResourceDescriptorHeap[Idx3];
    uint width, height;
    cloudOut.GetDimensions( width, height );
    if ( id.x >= width || id.y >= height ) return;

    Texture2D<float> depthTex = ResourceDescriptorHeap[Idx0];
    uint depthWidth, depthHeight;
    depthTex.GetDimensions( depthWidth, depthHeight );
    float2 uv = ( float2( id.xy ) + 0.5f ) / float2( width, height );
    int2 maxPixel = int2( depthWidth, depthHeight ) - 1;
    int2 base = int2( floor( uv * float2( depthWidth, depthHeight ) + 0.5f ) ) - 1;
    float4 footprint = float4(
        depthTex.Load( int3( clamp( base, 0, maxPixel ), 0 ) ),
        depthTex.Load( int3( clamp( base + int2( 1, 0 ), 0, maxPixel ), 0 ) ),
        depthTex.Load( int3( clamp( base + int2( 0, 1 ), 0, maxPixel ), 0 ) ),
        depthTex.Load( int3( clamp( base + int2( 1, 1 ), 0, maxPixel ), 0 ) ) );

    LowCloudLayerTexel texel = GenerateLowCloudTexel( uv, footprint, float2( id.xy ) + 0.5f );
    cloudOut[id.xy] = texel.clouds;
    depthOut[id.xy] = texel.depth;
    skyOut[id.xy] = texel.skyClouds;
}

struct VS_OUT { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

VS_OUT VSFullscreen( uint vid : SV_VertexID )
{
    VS_OUT o;
    o.uv = float2( ( vid << 1 ) & 2, vid & 2 );
    o.pos = float4( o.uv * float2( 2, -2 ) + float2( -1, 1 ), 0, 1 );
    return o;
}

// ONE / INV_SRC_ALPHA onto the scene
float4 PSComposite( VS_OUT i ) : SV_TARGET
{
    Texture2D<float> sceneDepth = ResourceDescriptorHeap[Idx3];
    float4 clouds = CompositeLowClouds( i.uv, sceneDepth.Load( int3( int2( i.pos.xy ), 0 ) ), i.pos.xy );
    float3 color = pow( max( clouds.rgb / max( clouds.a, 0.00001f ), 0.0f ), 2.2f );
    return float4( color * clouds.a, clouds.a );
}
