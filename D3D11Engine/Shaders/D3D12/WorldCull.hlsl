// GPU-driven world mesh (D3D12GpuWorld): per view, cull every world mesh and its ~128-triangle clusters and
// write the view's ExecuteIndirect lists (WorldDrawCommand, opaque and alpha-tested, GPU-counted). A run of
// contiguous visible clusters becomes one command, so hidden clusters drop out without multiplying draws.

struct WorldSection
{
    float3 Min;
    int    GridX;
    float3 Max;
    int    GridY;
};

struct WorldMesh
{
    uint   ClusterFirst;
    uint   ClusterCount;
    uint   MaterialIndex;
    uint   FlagsSection;    // WORLD_MESH_* in the low byte, the section index above
    float3 Min;
    float3 Max;
};
#define WORLD_MESH_MAIN    1u   // drawn by the main view
#define WORLD_MESH_CASTER  2u   // drawn by the shadow cascades

struct WorldCluster
{
    float3 Min;
    uint   IndexStart;      // absolute, into the wrapped world index buffer
    float3 Max;
    uint   IndexCount;
};

struct WorldMaterial
{
    uint  Normal;           // 0xFFFFFFFF = none
    uint  Orm;
    uint  Diffuse;
    float NormalStrength;
    uint  Flags;            // WORLD_MATERIAL_*
    uint3 _pad;
};
#define WORLD_MATERIAL_ALPHA  1u

cbuffer WorldCullCB : register( b0 )
{
    float4x4 CullViewProj;      // world -> D3D clip space (x,y in [-w,w], z in [0,w])
    float3   CamPos;
    float    SectionRadiusSq;   // main view: sections farther than this are skipped; 0 = no limit
    uint     MeshCount;
    uint     ViewFlags;         // VIEW_*
    uint     OpaqueCapacity;    // commands in the opaque list, which starts at command 0
    uint     AlphaCapacity;     // commands in the alpha list, which starts at command OpaqueCapacity
    uint     WetNormalSlot;     // rain normal for materials without one; 0xFFFFFFFF = dry
    float    WetNormalStrength;
    uint     DefaultOrm;
    uint     MaterialCount;
    int      CamSectionX;       // VIEW_GRID_RADIUS: the camera's section and the radius in sections
    int      CamSectionY;
    int      SectionGridRadius;
};
#define VIEW_MAIN         1u   // WORLD_MESH_MAIN meshes with full materials, else casters with diffuse only
#define VIEW_NO_FRUSTUM   2u
#define VIEW_FEEDBACK     4u   // mark the materials of drawn meshes in MaterialSeen
#define VIEW_GRID_RADIUS  8u   // section radius in grid cells instead of SectionRadiusSq

StructuredBuffer<WorldSection>  Sections     : register( t0 );
StructuredBuffer<WorldMesh>     Meshes       : register( t1 );
StructuredBuffer<WorldCluster>  Clusters     : register( t2 );
StructuredBuffer<WorldMaterial> Materials    : register( t3 );
RWByteAddressBuffer             Args         : register( u0 );   // WorldDrawCommand x (OpaqueCapacity + AlphaCapacity)
RWByteAddressBuffer             ArgCount     : register( u1 );   // [0] opaque, [4] alpha
RWByteAddressBuffer             MaterialSeen : register( u2 );   // one uint per material

#define WORLD_DRAW_COMMAND_STRIDE 36u
#define WORLDCULL_GROUP_SIZE 64
#define MAX_CLUSTER_WORDS 64        // 2048 clusters per mesh; the rest join the last run unculled

bool IsBoxVisible( float3 mn, float3 mx )
{
    if ( ViewFlags & VIEW_NO_FRUSTUM ) return true;
    bool outNegX = true, outPosX = true, outNegY = true, outPosY = true, outNear = true, outFar = true;
    [unroll]
    for ( uint c = 0; c < 8; ++c )
    {
        const float3 corner = float3( ( c & 1 ) ? mx.x : mn.x, ( c & 2 ) ? mx.y : mn.y, ( c & 4 ) ? mx.z : mn.z );
        const float4 p = mul( float4( corner, 1.0 ), CullViewProj );
        outNegX = outNegX && ( p.x < -p.w );
        outPosX = outPosX && ( p.x >  p.w );
        outNegY = outNegY && ( p.y < -p.w );
        outPosY = outPosY && ( p.y >  p.w );
        outNear = outNear && ( p.z <  0.0 );
        outFar  = outFar  && ( p.z >  p.w );
    }
    return !( outNegX || outPosX || outNegY || outPosY || outNear || outFar );
}

bool IsSectionInRange( uint sectionIndex )
{
    const WorldSection s = Sections[sectionIndex];
    if ( ViewFlags & VIEW_GRID_RADIUS )
        return abs( s.GridX - CamSectionX ) < SectionGridRadius && abs( s.GridY - CamSectionY ) < SectionGridRadius;
    if ( SectionRadiusSq <= 0.0 ) return true;
    const float3 d = CamPos - clamp( CamPos, s.Min, s.Max );
    return dot( d, d ) < SectionRadiusSq;
}

void EmitCommand( WorldMaterial mat, uint startIndex, uint indexCount )
{
    const bool alpha = ( mat.Flags & WORLD_MATERIAL_ALPHA ) != 0u;
    uint slot;
    ArgCount.InterlockedAdd( alpha ? 4u : 0u, 1u, slot );
    if ( slot >= ( alpha ? AlphaCapacity : OpaqueCapacity ) ) return;   // the draw clamps the count to capacity

    uint normal = 0xFFFFFFFFu, orm = DefaultOrm;
    float strength = 0.0;
    if ( ViewFlags & VIEW_MAIN )
    {
        normal = mat.Normal;
        orm = mat.Orm;
        strength = mat.NormalStrength;
        if ( normal == 0xFFFFFFFFu && WetNormalSlot != 0xFFFFFFFFu )
        {
            normal = WetNormalSlot;
            strength = WetNormalStrength;
        }
    }
    const uint at = ( ( alpha ? OpaqueCapacity : 0u ) + slot ) * WORLD_DRAW_COMMAND_STRIDE;
    Args.Store4( at,      uint4( normal, orm, mat.Diffuse, asuint( strength ) ) );
    Args.Store4( at + 16, uint4( indexCount, 1u, startIndex, 0u ) );
    Args.Store( at + 32, 0u );
}

groupshared uint gVisible[MAX_CLUSTER_WORDS];

bool IsClusterVisible( uint i )
{
    return ( gVisible[i >> 5] >> ( i & 31u ) ) & 1u;
}

// First cluster at or after `from` that is not visible, else `count`.
uint RunEnd( uint from, uint count )
{
    if ( from >= count ) return count;
    uint w = from >> 5;
    uint open = ~gVisible[w] & ( 0xFFFFFFFFu << ( from & 31u ) );
    const uint words = ( count + 31u ) >> 5;
    for ( ;; )
    {
        if ( open != 0u ) return min( w * 32u + firstbitlow( open ), count );
        if ( ++w >= words ) return count;
        open = ~gVisible[w];
    }
}

// One group per mesh. Every early-out below is uniform across the group, so the barriers stay convergent.
[numthreads(WORLDCULL_GROUP_SIZE, 1, 1)]
void CSWorldCull( uint3 gid : SV_GroupID, uint gtid : SV_GroupIndex )
{
    const uint meshIndex = min( gid.x, MeshCount - 1u );
    const WorldMesh m = Meshes[meshIndex];
    const uint wanted = ( ViewFlags & VIEW_MAIN ) ? WORLD_MESH_MAIN : WORLD_MESH_CASTER;
    const bool active = gid.x < MeshCount && ( m.FlagsSection & wanted ) != 0u
        && IsSectionInRange( m.FlagsSection >> 8 ) && IsBoxVisible( m.Min, m.Max );

    const uint culled = min( m.ClusterCount, MAX_CLUSTER_WORDS * 32u );
    if ( gtid < MAX_CLUSTER_WORDS ) gVisible[gtid] = 0u;
    GroupMemoryBarrierWithGroupSync();
    if ( active )
    {
        for ( uint i = gtid; i < culled; i += WORLDCULL_GROUP_SIZE )
        {
            const WorldCluster c = Clusters[m.ClusterFirst + i];
            if ( IsBoxVisible( c.Min, c.Max ) ) InterlockedOr( gVisible[i >> 5], 1u << ( i & 31u ) );
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if ( !active ) return;

    const WorldMaterial mat = Materials[m.MaterialIndex];
    bool any = false;
    for ( uint i = gtid; i < culled; i += WORLDCULL_GROUP_SIZE )
    {
        if ( !IsClusterVisible( i ) || ( i > 0u && IsClusterVisible( i - 1u ) ) ) continue;   // not a run start
        any = true;
        uint end = RunEnd( i + 1u, culled );
        if ( end == culled ) end = m.ClusterCount;   // clusters past the bitmask ride along with the last run
        const uint start = Clusters[m.ClusterFirst + i].IndexStart;
        const WorldCluster last = Clusters[m.ClusterFirst + end - 1u];
        EmitCommand( mat, start, last.IndexStart + last.IndexCount - start );
    }
    // Past the bitmask with no run reaching it: the tail draws on its own.
    if ( gtid == 0u && m.ClusterCount > culled && !IsClusterVisible( culled - 1u ) )
    {
        any = true;
        const uint start = Clusters[m.ClusterFirst + culled].IndexStart;
        const WorldCluster last = Clusters[m.ClusterFirst + m.ClusterCount - 1u];
        EmitCommand( mat, start, last.IndexStart + last.IndexCount - start );
    }
    if ( ( ViewFlags & VIEW_FEEDBACK ) && WaveActiveAnyTrue( any ) && WaveIsFirstLane() )
        MaterialSeen.Store( m.MaterialIndex * 4u, 1u );
}

// Zeroes a view's two command counts and, with VIEW_FEEDBACK, the material feedback.
[numthreads(64, 1, 1)]
void CSWorldClear( uint3 DTid : SV_DispatchThreadID )
{
    if ( DTid.x == 0u ) ArgCount.Store2( 0, uint2( 0u, 0u ) );
    if ( ( ViewFlags & VIEW_FEEDBACK ) && DTid.x < MaterialCount ) MaterialSeen.Store( DTid.x * 4u, 0u );
}
