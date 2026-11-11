#ifndef VOB_VERTEX_PULL_HLSL
#define VOB_VERTEX_PULL_HLSL
// Vertex + instance pulling for the VOB / attachment arena draws (VERTEX_PULLING_PLAN.md phase 2). Vertices are VF_EX60 from
// the arena VB picked by b9.PackedVobInfo; instances are VobInstanceInfo records read from the root SRV t6 (bound at the
// buffer start). b10 = per-draw { VertexBase, InstanceBase }, since SV_VertexID/SV_InstanceID exclude the draw's bases.
#include "WorldVertexPull.hlsl"

cbuffer VobDrawCB : register(b10) { uint VobVertexBase; uint VobInstanceBase; };
ByteAddressBuffer VobInstances : register(t6);

static const uint kVobFarRun = 0x80000000u;   // InstanceBase top bit: the run is walked backward from its last element

ByteAddressBuffer VobVb() { return GD_HEAP( WorldVbIndex + ( PackedVobInfo >> 16 ) ); }

uint VobVertexIndex( uint vid ) { return vid + VobVertexBase; }

// Near runs ascend from InstanceBase; a GPU-culled far run is packed backward from the end of its visual's block.
uint VobInstanceIndex( uint iid )
{
    return ( VobInstanceBase & kVobFarRun ) ? ( VobInstanceBase & ~kVobFarRun ) - iid : VobInstanceBase + iid;
}

VobInstance LoadPulledVobInstance( uint iid )
{
    return LoadVobInstance( VobInstances, PackedVobInfo & 0xFFFFu, VobInstanceIndex( iid ) );
}

#endif // VOB_VERTEX_PULL_HLSL
