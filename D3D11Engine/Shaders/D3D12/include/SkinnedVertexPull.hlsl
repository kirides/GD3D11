#ifndef SKINNED_VERTEX_PULL_HLSL
#define SKINNED_VERTEX_PULL_HLSL
// Vertex pulling of the skinned geometry (VERTEX_PULLING_PLAN.md phase 3): the posed streams SkinVertices.hlsl writes and the
// raw skeletal arena VB. `streamBase` is the stream-table base (D3D12GeometryStreams block start); the offsets mirror
// D3D12GeometryStreams::Stream (static_assert'ed in D3D12GeometryStreams.cpp). The vertex index is SV_VertexID + the draw's VertexBase.
#include "VertexFetch.hlsl"

static const uint kStreamSkelVB         = 6;
static const uint kStreamSkinnedPosUv   = 8;
static const uint kStreamSkinnedNrmPrev = 9;

ByteAddressBuffer PosedPosUvVb( uint streamBase )   { return GD_HEAP( streamBase + kStreamSkinnedPosUv ); }
ByteAddressBuffer PosedNrmPrevVb( uint streamBase ) { return GD_HEAP( streamBase + kStreamSkinnedNrmPrev ); }
ByteAddressBuffer SkelArenaVb( uint streamBase )    { return GD_HEAP( streamBase + kStreamSkelVB ); }

#endif // SKINNED_VERTEX_PULL_HLSL
