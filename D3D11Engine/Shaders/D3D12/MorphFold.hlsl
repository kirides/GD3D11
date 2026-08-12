// GPU morph fold (D3D12MorphFold.cpp / MorphGpu.h) — the compute form of zCMorphMesh::CalcVertPositions.
//
// One thread per output VERTEX of one morph submesh. Each thread folds that vertex's blend channels and
// rewrites ONLY the 12-byte Position in place; the normal/tangent/UV/color bytes beside it are per-wedge data
// written once at conversion. The buffer therefore stays bindable exactly as it was — no new input layouts,
// no PSO changes, and the CSM/point-shadow passes fold for free because they draw the same buffer.
//
// The blend is a SEQUENTIAL FOLD, not a weighted sum:
//     acc = Weight1M * acc + sample * Weight     (per channel, IN CHANNEL ORDER)
// so ChannelFirst..ChannelFirst+ChannelCount must stay in the order ZENGIN folds them, and a channel that
// does not touch this vertex has to be skipped ENTIRELY (leaving acc alone) rather than contributing zero.
// See MorphBlend.h for the full derivation; the easing (ZENGIN's double zSinusEase over its quantised
// zSinApprox table) is already baked into Weight/Weight1M on the CPU, so it never appears here.
//
// Everything rides on root descriptors / root constants — no descriptor tables, no heap slots.

// Mirrors MorphGpu::ChannelRecord (24 B).
struct ChannelRecord
{
    uint  FrameA;     // float3 index into Positions: this channel's current frame, slot 0
    uint  FrameB;     // ...and the frame it lerps towards (== FrameA for a single-frame ani)
    uint  Inverse;    // uint index into Indices: this ani's dense vertex -> slot table
    float Frac;       // FrameA -> FrameB lerp; exactly 0 for a single-frame ani
    float Weight;
    float Weight1M;   // 1 - Weight, or exactly 1 for a shape ani (replaces instead of blending)
};

cbuffer MorphFoldCB : register( b0 )
{
    uint OutVertexCount;   // wedges in this submesh — the dispatch is padded up, so threads must range-check
    uint VertexStride;     // bytes per vertex in the output buffer (sizeof(ExVertexStruct))
    uint RestBase;         // float3 index of THIS INSTANCE's rest base (refShape shape, or morphRefMeshVertPos)
    uint WedgeBase;        // uint index of this submesh's wedge -> mesh-vertex table
    uint ChannelFirst;     // first ChannelRecord of this instance
    uint ChannelCount;     // 0 is legal and normal: an instance with no active channel is the pure rest pose
    uint _morphPad0;
    uint _morphPad1;
};

// Both are per-PROTOTYPE (per .MMS) and immutable for the session — built once by MorphGpu and uploaded to
// VRAM. Positions holds the rest bases plus every ani's vertPosMatrix concatenated; Indices holds the
// per-ani inverse tables followed by the per-submesh wedge tables.
StructuredBuffer<float3>        Positions : register( t0 );
StructuredBuffer<uint>          Indices   : register( t1 );
StructuredBuffer<ChannelRecord> Channels  : register( t2 );   // per-frame, all instances of the frame

// The submesh's own vertex buffer, in place. Raw rather than structured because the vertex is 60 bytes of
// mixed formats and only its first 12 are ours to touch.
RWByteAddressBuffer OutVertices : register( u0 );

#define MORPH_NO_SLOT 0xFFFFFFFFu

[numthreads( 64, 1, 1 )]
void CSFold( uint3 dtid : SV_DispatchThreadID )
{
    const uint wedge = dtid.x;
    if ( wedge >= OutVertexCount )
        return;

    // Several wedges share one mesh vertex (a crease duplicates the wedge, not the position), so this is a
    // gather: every wedge of a position folds the same channels and arrives at the same result.
    const uint v = Indices[WedgeBase + wedge];

    float3 acc = float3( 0.0f, 0.0f, 0.0f );
    for ( uint i = 0; i < ChannelCount; ++i )
    {
        const ChannelRecord c = Channels[ChannelFirst + i];
        const uint slot = Indices[c.Inverse + v];
        if ( slot == MORPH_NO_SLOT )
            continue;   // this ani does not touch this vertex — acc must survive untouched, not be scaled

        const float3 a = Positions[c.FrameA + slot];
        const float3 b = Positions[c.FrameB + slot];
        // Written as a + frac*(b-a) to match MorphBlend::Apply's `sample += frac * (next - sample)` term
        // for term; with Frac == 0 it degenerates to `a` exactly, which is the CPU's non-lerping branch.
        const float3 sample = a + c.Frac * ( b - a );
        acc = c.Weight1M * acc + sample * c.Weight;
    }

    // CalcVertPositions' trailing add of the rest base.
    acc += Positions[RestBase + v];

    OutVertices.Store3( wedge * VertexStride, asuint( acc ) );
}
