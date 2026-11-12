#ifndef WORLD_TESS_STREAM_HLSL
#define WORLD_TESS_STREAM_HLSL
// Welded world stream (v3, 20 B per wrapped vertex) + override table, read by vertex pulling. Needs WorldVertexPull.hlsl (WorldVbIndex, GD_HEAP).
// dword 0 oct normal, 1 roundness8 | table << 8, 2-3 canonical uv, 4 half kT | half kB << 16 (see WORLD_TESSELLATION_PLAN.md 12.1).
// Table entry (float4): .x roundness scale, .y factor scale, .z displacement, .w low16 0xFFFF = mesh entry else boundary (canonical height slot, 0xFFFE = none), high16 half uvDensity.

static const uint kWorldTessStreamOffset = 10;       // D3D12GeometryStreams::Stream::WorldTess
static const uint kWorldTessParamsStreamOffset = 11; // D3D12GeometryStreams::Stream::WorldTessParams; float4 per override entry
static const uint kWorldTessStreamStride = 20;
static const uint kWorldTessMeshEntry = 0xFFFFu;
static const uint kWorldTessNoSlot = 0xFFFEu;

struct WorldTessRaw
{
    uint  nrmPacked;
    uint  roundness8;
    uint  table;
    float2 canonUv;
    float2 curv;       // kT, kB
    uint4 entry;       // raw table entry
};

WorldTessRaw LoadWorldTessRaw( uint vid )
{
    const ByteAddressBuffer s = GD_HEAP( WorldVbIndex + kWorldTessStreamOffset );
    const uint a = vid * kWorldTessStreamStride;
    const uint4 w = s.Load4( a );
    WorldTessRaw o;
    o.nrmPacked = w.x;
    o.roundness8 = w.y & 0xFFu;
    o.table = w.y >> 8;
    o.canonUv = asfloat( w.zw );
    o.curv = VfUnpackHalf2( s.Load( a + 16 ) );
    const ByteAddressBuffer table = GD_HEAP( WorldVbIndex + kWorldTessParamsStreamOffset );
    o.entry = table.Load4( o.table * 16 );
    return o;
}

bool WorldTessIsBoundary( uint4 entry ) { return ( entry.w & 0xFFFFu ) != kWorldTessMeshEntry; }

// Height slot of the canonical mesh; a mesh entry uses `ownSlot`, 0xFFFFFFFF = none
uint WorldTessCanonSlot( uint4 entry, uint ownSlot )
{
    const uint low = entry.w & 0xFFFFu;
    return low == kWorldTessMeshEntry ? ownSlot : ( low == kWorldTessNoSlot ? 0xFFFFFFFFu : low );
}

// POM interpolant: x uvDensity, y borderFade (1 interior, 0 boundary/seam), zw curvature kT, kB
float4 LoadWorldPom( uint vid )
{
    const WorldTessRaw r = LoadWorldTessRaw( vid );
    return float4( f16tof32( r.entry.w >> 16 ), WorldTessIsBoundary( r.entry ) ? 0.0 : 1.0, r.curv );
}

#endif // WORLD_TESS_STREAM_HLSL
