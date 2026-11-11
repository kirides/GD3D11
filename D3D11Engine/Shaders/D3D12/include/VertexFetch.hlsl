#ifndef VERTEX_FETCH_HLSL
#define VERTEX_FETCH_HLSL
// Vertex-pulling decoders (VERTEX_PULLING_PLAN.md sec. 2). Declares no resources: callers pass the ByteAddressBuffer
// holding the vertices. `v` is the vertex index with the draw's base vertex already added (SV_VertexID excludes it).
// Pass the format as a literal so the switches fold. Layouts: VertexTypes.h (ExVertexStruct[GPU]), SkinVertices.hlsl.

static const uint VF_PACKED36 = 0;   // ExVertexStructGPU, 36 B
static const uint VF_EX60     = 1;   // ExVertexStruct, 60 B
static const uint VF_POSED    = 2;   // skinned stream A, 20 B {pos f3, uv f2}; stream B via LoadPosed* (16 B)
static const uint VF_SKEL76   = 3;   // ExSkelVertexStruct, 76 B: LoadSkel76 only

static const uint kStridePacked36 = 36;
static const uint kStrideEx60     = 60;
static const uint kStridePosedA   = 20;
static const uint kStridePosedB   = 16;
static const uint kStrideSkel76   = 76;

struct FetchedVertex
{
    float3 pos;
    float3 nrm;   // unit for PACKED36 (decoded), raw stored value for EX60
    float4 tan;   // xyz unit tangent + w handedness for PACKED36, raw float4 for EX60
    float2 uv;
    float2 uv2;
    float4 col;   // R8G8B8A8_UNORM: byte 0 = x
};

// Duplicates of the octahedral/tangent decoders in Shaders/VertexPacking.h (which the include handler can't reach)
// and World.hlsl/Water.hlsl; named distinctly so a shader can include this next to either.
float3 VfDecodeOctNormal( float2 e )
{
    float3 n = float3( e.xy, 1.0 - abs( e.x ) - abs( e.y ) );
    float t = saturate( -n.z );
    n.xy += select( n.xy >= 0., -t, t );
    return normalize( n );
}

float4 VfDecodeTangent( float4 packed )
{
    float3 t = packed.xyz * 2.0 - 1.0;
    float sign = packed.w > 0.5 ? 1.0 : -1.0;
    return float4( normalize( t ), sign );
}

float4 VfUnpackUnorm4x8( uint u )
{
    return float4( u & 0xFF, ( u >> 8 ) & 0xFF, ( u >> 16 ) & 0xFF, u >> 24 ) * ( 1.0 / 255.0 );
}

// D3D SNORM rule: -32768 maps to -1 like -32767.
float2 VfUnpackSnorm2x16( uint u )
{
    const int2 i = int2( int( u << 16 ) >> 16, int( u ) >> 16 );
    return max( float2( i ) * ( 1.0 / 32767.0 ), -1.0 );
}

uint PackSnorm2x16( float2 v )
{
    const int2 i = int2( round( clamp( v, -1.0, 1.0 ) * 32767.0 ) );
    return ( uint( i.x ) & 0xFFFFu ) | ( uint( i.y ) << 16 );
}

float2 VfUnpackHalf2( uint u )
{
    return float2( f16tof32( u ), f16tof32( u >> 16 ) );
}

// R10G10B10A2_UNORM: x bits 0-9, y 10-19, z 20-29, w 30-31.
float4 VfUnpackUnorm10_10_10_2( uint u )
{
    return float4( u & 0x3FF, ( u >> 10 ) & 0x3FF, ( u >> 20 ) & 0x3FF, u >> 30 ) * float4( 1.0 / 1023.0, 1.0 / 1023.0, 1.0 / 1023.0, 1.0 / 3.0 );
}

uint VfStride( uint fmt )
{
    switch ( fmt )
    {
    case VF_PACKED36: return kStridePacked36;
    case VF_EX60:     return kStrideEx60;
    case VF_POSED:    return kStridePosedA;
    default:          return kStrideSkel76;
    }
}

// Position is @0 in every format that has one.
float3 LoadPosition( ByteAddressBuffer b, uint fmt, uint v )
{
    return asfloat( b.Load3( v * VfStride( fmt ) ) );
}

void LoadPosUv( ByteAddressBuffer b, uint fmt, uint v, out float3 pos, out float2 uv )
{
    const uint a = v * VfStride( fmt );
    pos = asfloat( b.Load3( a ) );
    switch ( fmt )
    {
    case VF_PACKED36: uv = asfloat( b.Load2( a + 20 ) ); break;
    case VF_EX60:     uv = asfloat( b.Load2( a + 24 ) ); break;
    default:          uv = asfloat( b.Load2( a + 12 ) ); break;   // VF_POSED
    }
}

// PACKED36 and EX60 only; posed streams use LoadPosed* (their data spans two buffers).
FetchedVertex LoadFull( ByteAddressBuffer b, uint fmt, uint v )
{
    FetchedVertex o;
    if ( fmt == VF_PACKED36 )
    {
        const uint a = v * kStridePacked36;
        o.pos  = asfloat( b.Load3( a ) );
        o.nrm  = VfDecodeOctNormal( VfUnpackSnorm2x16( b.Load( a + 12 ) ) );
        o.tan  = VfDecodeTangent( VfUnpackUnorm10_10_10_2( b.Load( a + 16 ) ) );
        o.uv   = asfloat( b.Load2( a + 20 ) );
        o.uv2  = VfUnpackHalf2( b.Load( a + 28 ) );
        o.col  = VfUnpackUnorm4x8( b.Load( a + 32 ) );
    }
    else
    {
        const uint a = v * kStrideEx60;
        o.pos  = asfloat( b.Load3( a ) );
        o.nrm  = asfloat( b.Load3( a + 12 ) );
        o.uv   = asfloat( b.Load2( a + 24 ) );
        o.uv2  = asfloat( b.Load2( a + 32 ) );
        o.col  = VfUnpackUnorm4x8( b.Load( a + 40 ) );
        o.tan  = asfloat( b.Load4( a + 44 ) );
    }
    return o;
}

// Compute-skinned streams: A = {pos f3, uv f2} (20 B), B = {octN snorm16x2, prevPos f3} (16 B).
float3 LoadPosedNormal( ByteAddressBuffer b, uint v )
{
    return VfDecodeOctNormal( VfUnpackSnorm2x16( b.Load( v * kStridePosedB ) ) );
}

float3 LoadPosedPrevPos( ByteAddressBuffer b, uint v )
{
    return asfloat( b.Load3( v * kStridePosedB + 4 ) );
}

// Tangent is not stored; col is white.
FetchedVertex LoadPosedFull( ByteAddressBuffer bufA, ByteAddressBuffer bufB, uint v )
{
    FetchedVertex o;
    LoadPosUv( bufA, VF_POSED, v, o.pos, o.uv );
    o.nrm = LoadPosedNormal( bufB, v );
    o.tan = float4( 0, 0, 0, 1 );
    o.uv2 = float2( 0, 0 );
    o.col = float4( 1, 1, 1, 1 );
    return o;
}

// ExSkelVertexStruct: Position[4] half4 @0, Normal float3 @32, BindPoseNormal @44, TexCoord @56,
// boneIndices uint8x4 @64, weights half4 @68.
float4 LoadHalf4( ByteAddressBuffer b, uint address )
{
    const uint2 h = b.Load2( address );
    return float4( f16tof32( h.x ), f16tof32( h.x >> 16 ), f16tof32( h.y ), f16tof32( h.y >> 16 ) );
}

struct SkelVertexIn
{
    float4 pos[4];
    float3 normal;
    float2 uv;
    uint4  bones;
    float4 weights;
};

SkelVertexIn LoadSkel76( ByteAddressBuffer b, uint v )
{
    const uint src = v * kStrideSkel76;
    SkelVertexIn o;
    [unroll]
    for ( uint i = 0; i < 4; ++i )
        o.pos[i] = LoadHalf4( b, src + i * 8 );
    o.normal = asfloat( b.Load3( src + 32 ) );
    o.uv     = asfloat( b.Load2( src + 56 ) );
    const uint boneIds = b.Load( src + 64 );
    o.bones   = uint4( boneIds & 0xFF, ( boneIds >> 8 ) & 0xFF, ( boneIds >> 16 ) & 0xFF, boneIds >> 24 );
    o.weights = LoadHalf4( b, src + 68 );
    return o;
}

// VobInstanceInfo (ConstantBufferStructs.h): world 3x4 rows @0, color @48 (R8G8B8A8_UNORM), { windStrength,
// canBeAffectedByPlayer } @52, GP_Slot @60, prevWorld @64. Stride 112, or 64 without motion (prevWorld = world).
struct VobInstance
{
    float3x4 world;
    float3x4 prevWorld;
    float4   color;
    float2   wind;
    uint     gpSlot;
};

VobInstance LoadVobInstance( ByteAddressBuffer b, uint strideBytes, uint i )
{
    const uint a = i * strideBytes;
    VobInstance o;
    o.world = float3x4( asfloat( b.Load4( a ) ), asfloat( b.Load4( a + 16 ) ), asfloat( b.Load4( a + 32 ) ) );
    o.color = VfUnpackUnorm4x8( b.Load( a + 48 ) );
    o.wind  = asfloat( b.Load2( a + 52 ) );
    o.gpSlot = b.Load( a + 60 );
    if ( strideBytes >= 112 )
        o.prevWorld = float3x4( asfloat( b.Load4( a + 64 ) ), asfloat( b.Load4( a + 80 ) ), asfloat( b.Load4( a + 96 ) ) );
    else
        o.prevWorld = o.world;
    return o;
}

#endif // VERTEX_FETCH_HLSL
