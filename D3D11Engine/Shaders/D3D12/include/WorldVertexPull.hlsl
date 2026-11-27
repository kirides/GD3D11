#ifndef WORLD_VERTEX_PULL_HLSL
#define WORLD_VERTEX_PULL_HLSL
// Vertex pulling of the wrapped world VB (ExVertexStructGPU, 36 B). Every world draw has BaseVertexLocation 0, so
// SV_VertexID is the absolute vertex index. b9 = { stream-table base (== the world VB's raw SRV heap index, WorldVB is
// stream 0 of D3D12GeometryStreams), PackedVobInfo = VOB instance stride | (VB stream offset << 16) (VobVertexPull.hlsl) }.
#include "VertexFetch.hlsl"

cbuffer WorldPullCB : register(b9) { uint WorldVbIndex; uint PackedVobInfo; };

ByteAddressBuffer WorldVb() { return GD_HEAP( WorldVbIndex ); }

// The fields as the input assembler delivers them (R16G16_SNORM, R10G10B10A2_UNORM, R16G16_FLOAT, R8G8B8A8_UNORM).
struct WorldRawVertex
{
    float3 pos;
    float2 nrm;
    float4 tan;
    float2 uv;
    float2 scroll;
    float4 col;
};

WorldRawVertex LoadWorldRaw( uint vid )
{
    const ByteAddressBuffer b = WorldVb();
    const uint a = vid * kStridePacked36;
    WorldRawVertex o;
    o.pos    = asfloat( b.Load3( a ) );
    o.nrm    = VfUnpackSnorm2x16( b.Load( a + 12 ) );
    o.tan    = VfUnpackUnorm10_10_10_2( b.Load( a + 16 ) );
    o.uv     = asfloat( b.Load2( a + 20 ) );
    o.scroll = VfUnpackHalf2( b.Load( a + 28 ) );
    o.col    = VfUnpackUnorm4x8( b.Load( a + 32 ) );
    return o;
}

#endif // WORLD_VERTEX_PULL_HLSL
