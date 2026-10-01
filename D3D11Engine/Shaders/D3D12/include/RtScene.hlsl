// The ray-traced scene shared by WaterRT.hlsl and RtShadows.hlsl: the TLAS, its records and geometry
// (t0-t12, s0), triangle fetch and the alpha test. Records and bind order: D3D12RayTracing.cpp.
#ifndef D3D12_RTSCENE_HLSL
#define D3D12_RTSCENE_HLSL

// One record per BLAS geometry of every non-world instance, at InstanceID() + GeometryIndex().
struct RtGeom
{
    uint BaseVertex;
    uint StartIndex;
    uint Material;   // diffuse SRV slot (low 24 bits) | kMatAlphaTest | kMatNoTexture
    uint Kind;       // kKind*
};
// Per TLAS instance, at InstanceIndex().
struct RtInstance
{
    uint Color;      // R8G8B8A8 instance light; .g is the baked vertex light
    uint Pad0, Pad1, Pad2;
};

static const uint kKindVob = 1u;
static const uint kKindAttach = 2u;
static const uint kKindSkinned = 3u;
static const uint kWorldInstanceId = 0xFFFFFFu;
static const uint kMatAlphaTest = 0x80000000u;
static const uint kMatNoTexture = 0x40000000u;
static const uint kMatSlotMask = 0x00FFFFFFu;

RaytracingAccelerationStructure Scene : register( t0 );
StructuredBuffer<RtGeom>     Geoms      : register( t1 );
StructuredBuffer<RtInstance> Instances  : register( t2 );
StructuredBuffer<uint2>      WorldGeoms : register( t3 );   // x = start index, y = world material index
StructuredBuffer<uint>       WorldMats  : register( t4 );   // per-frame Material words of the world materials
ByteAddressBuffer WorldVB    : register( t5 );   // 36-byte ExVertexStructGPU, R32 indices
ByteAddressBuffer WorldIB    : register( t6 );
ByteAddressBuffer VobVB      : register( t7 );   // 60-byte ExVertexStruct, R16 indices
ByteAddressBuffer VobIB      : register( t8 );
ByteAddressBuffer AttachVB   : register( t9 );   // same layout as the VOB arena
ByteAddressBuffer AttachIB   : register( t10 );
ByteAddressBuffer SkinPosUv  : register( t11 );  // 20-byte posed {world pos, uv}
ByteAddressBuffer SkelIB     : register( t12 );

SamplerState smpWrap : register( s0 );

uint LoadIndex16( ByteAddressBuffer ib, uint index )
{
    uint addr = index * 2u;
    uint w = ib.Load( addr & ~3u );
    return ( addr & 2u ) ? ( w >> 16 ) : ( w & 0xFFFFu );
}

struct Tri
{
    float3 p[3];       // object space (world space for the world mesh and posed NPCs)
    float2 uv[3];
    float  light[3];   // baked vertex light (world mesh only, else the instance light)
    uint   material;
};

Tri FetchTri( uint instanceId, uint instanceIndex, uint geometry, uint primitive )
{
    Tri t;
    if ( instanceId == kWorldInstanceId )
    {
        uint2 g = WorldGeoms[geometry];
        t.material = WorldMats[g.y];
        [unroll] for ( uint k = 0; k < 3; ++k )
        {
            uint v = WorldIB.Load( ( g.x + primitive * 3u + k ) * 4u );
            uint a = v * 36u;
            t.p[k] = asfloat( WorldVB.Load3( a ) );
            t.uv[k] = asfloat( WorldVB.Load2( a + 20u ) );
            t.light[k] = float( ( WorldVB.Load( a + 32u ) >> 8 ) & 0xFFu ) / 255.0;
        }
        return t;
    }

    RtGeom g = Geoms[instanceId + geometry];
    t.material = g.Material;
    float instLight = float( ( Instances[instanceIndex].Color >> 8 ) & 0xFFu ) / 255.0;
    [unroll] for ( uint k = 0; k < 3; ++k )
    {
        uint i = g.StartIndex + primitive * 3u + k;
        t.light[k] = instLight;
        if ( g.Kind == kKindSkinned )
        {
            uint a = ( g.BaseVertex + LoadIndex16( SkelIB, i ) ) * 20u;
            t.p[k] = asfloat( SkinPosUv.Load3( a ) );
            t.uv[k] = asfloat( SkinPosUv.Load2( a + 12u ) );
        }
        else if ( g.Kind == kKindAttach )
        {
            uint a = ( g.BaseVertex + LoadIndex16( AttachIB, i ) ) * 60u;
            t.p[k] = asfloat( AttachVB.Load3( a ) );
            t.uv[k] = asfloat( AttachVB.Load2( a + 24u ) );
        }
        else
        {
            uint a = ( g.BaseVertex + LoadIndex16( VobIB, i ) ) * 60u;
            t.p[k] = asfloat( VobVB.Load3( a ) );
            t.uv[k] = asfloat( VobVB.Load2( a + 24u ) );
        }
    }
    return t;
}

float2 InterpUV( Tri t, float2 bary ) { return t.uv[0] + bary.x * ( t.uv[1] - t.uv[0] ) + bary.y * ( t.uv[2] - t.uv[0] ); }

// Ray-cone mip selection (Akenine-Moeller et al.): texel-to-surface area ratio of the triangle plus the cone width.
float TextureLod( Tri t, Texture2D tex, float coneWidth, float cosine )
{
    float w, h;
    tex.GetDimensions( w, h );
    float2 duv1 = t.uv[1] - t.uv[0], duv2 = t.uv[2] - t.uv[0];
    float ta = abs( duv1.x * duv2.y - duv2.x * duv1.y ) * w * h;
    float pa = length( cross( t.p[1] - t.p[0], t.p[2] - t.p[0] ) );
    float base = 0.5 * log2( max( ta, 1e-12 ) / max( pa, 1e-12 ) );
    return base + log2( max( coneWidth, 1e-4 ) / max( cosine, 0.1 ) );
}

// Candidate alpha test; true = the hit stands
bool PassesAlpha( Tri t, float2 bary, float coneWidth, float lodBias )
{
    if ( ( t.material & kMatAlphaTest ) == 0u || ( t.material & kMatNoTexture ) != 0u ) return true;
    Texture2D tex = ResourceDescriptorHeap[t.material & kMatSlotMask];
    float lod = max( TextureLod( t, tex, coneWidth, 1.0 ) + lodBias, 0.0 );
    return tex.SampleLevel( smpWrap, InterpUV( t, bary ), lod ).a >= 0.5;
}

// 1 = nothing in the way. Alpha-tested candidates are resolved against their diffuse alpha unless alphaTest is off;
// coneWidth picks the alpha mip.
float TraceVisibility( RayDesc ray, uint mask, bool alphaTest, float coneWidth )
{
    [branch] if ( !alphaTest )
    {
        RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> q;
        q.TraceRayInline( Scene, RAY_FLAG_NONE, mask, ray );
        q.Proceed();
        return q.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? 0.0 : 1.0;
    }

    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> q;
    q.TraceRayInline( Scene, RAY_FLAG_NONE, mask, ray );
    while ( q.Proceed() )
    {
        if ( q.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE ) continue;
        Tri t = FetchTri( q.CandidateInstanceID(), q.CandidateInstanceIndex(), q.CandidateGeometryIndex(), q.CandidatePrimitiveIndex() );
        if ( PassesAlpha( t, q.CandidateTriangleBarycentrics(), coneWidth, 0.0 ) )
            q.CommitNonOpaqueTriangleHit();
    }
    return q.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? 0.0 : 1.0;
}

#endif // D3D12_RTSCENE_HLSL
