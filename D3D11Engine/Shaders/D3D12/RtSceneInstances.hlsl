// The GPU scene's share of the ray-traced scene (D3D12RayTracing.cpp): TLAS instance descs for the table's live
// instances in RT range, placed behind the CPU-written ones, and the geometry records their hits read
// (include/RtScene.hlsl). No per-frame CPU walk over the static VOBs.
//
//   CSClear     : zeroes the instance counter.
//   CSInstances : one group per scene visual; appends its instances, writes its records and its in-range flag.
//   CSTail      : the unused rest of the instance budget becomes inactive instances (null BLAS, mask 0).

// Mirrors VobCull.hlsl's VobCullVisual (the scene records).
struct SceneVisual
{
    float3 BBoxMin;
    uint   InstanceBase;
    float3 BBoxMax;
    uint   InstanceCount;
    uint   SplitMode;
    uint   SceneFlags;
};
#define SCENE_VISUAL_SMALL 1u
#define SCENE_VISUAL_MOB   2u

// Mirrors VobCull.hlsl's SceneInstanceGpu: the rows of the instance's world matrix.
struct SceneInstance
{
    float4 World0, World1, World2;
    uint   Color;
    float  WindStrength;
    float  CanBeAffectedByPlayer;
    uint   GPSlot;
};
#define SCENE_GPSLOT_HIDDEN 0x20000000u
#define SCENE_GPSLOT_INDOOR 0x10000000u

// Mirrors VobCull.hlsl's SceneTemplate.
struct SceneTemplate
{
    uint  MatNormalIndex;
    uint  MatOrmIndex;
    uint  MatDiffuseIndex;
    float WindMinHeight;
    float WindMaxHeight;
    uint  IndexCount;
    uint  StartIndex;
    int   BaseVertex;
    uint  VisualIndex;
    uint  LodBucket;
    uint  Flags;
    uint  CasterLodStart;
};
#define SCENE_TEMPLATE_ALPHA    1u
#define SCENE_TEMPLATE_READY    2u
#define SCENE_TEMPLATE_RESOLVED 8u

// Per scene visual, CPU-written per frame: its BLAS (0 = not traced), first template and template layout.
struct VisualRt
{
    uint2 Blas;
    uint  TemplateBase;
    uint  Layout;   // near template count | caster offset << 16
};

// Mirror include/RtScene.hlsl
struct RtGeom { uint BaseVertex; uint StartIndex; uint Material; uint Kind; };
struct RtInstance { uint Color; float SwayReach; uint Pad1, Pad2; };
static const uint kKindVob = 1u;
static const uint kMatAlphaTest = 0x80000000u;
static const uint kMatNoTexture = 0x40000000u;
static const uint kMatSlotMask = 0x0000FFFFu;   // MaterialFx packs its table slot above
static const uint kMaskVob = 0x02u, kMaskDynamic = 0x04u;   // D3D12RayTracing::kMask*
static const uint kInstanceForceNonOpaque = 0x8u;           // D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_NON_OPAQUE

cbuffer RtSceneCB : register( b0 )
{
    float3 CamPos;
    float  OutdoorRadius;
    float  SmallRadius;
    float  IndoorRadius;    // 0 = no indoor instances
    float  MobRadius;       // 0 = no MOB snapshots
    uint   VisualCount;
    uint   OutputBase;      // first instance desc of the scene: the CPU-written ones come before it
    uint   Budget;          // instance descs the TLAS build covers behind OutputBase
    uint   GeomBase;        // first geometry record of the scene; template t's records start at GeomBase + t
    uint   CbPad;
};

StructuredBuffer<SceneVisual>   Visuals   : register( t0 );
StructuredBuffer<SceneInstance> Table     : register( t1 );
StructuredBuffer<SceneTemplate> Templates : register( t2 );
StructuredBuffer<VisualRt>      VisualRts : register( t3 );
RWByteAddressBuffer             InstanceDescs : register( u0 );   // D3D12_RAYTRACING_INSTANCE_DESC, 64 bytes
RWStructuredBuffer<RtInstance>  Instances     : register( u1 );
RWStructuredBuffer<RtGeom>      Geoms         : register( u2 );
RWByteAddressBuffer             Feedback      : register( u3 );   // [0] instances wanted, then an in-range flag per visual

// The CPU collect's distance tests (VobCull.hlsl's IsSceneInstanceInRange).
bool InRange( SceneVisual v, SceneInstance s )
{
    if ( s.GPSlot & SCENE_GPSLOT_HIDDEN ) return false;
    const float radius = ( v.SceneFlags & SCENE_VISUAL_MOB ) ? MobRadius
        : ( s.GPSlot & SCENE_GPSLOT_INDOOR ) ? IndoorRadius
        : ( v.SceneFlags & SCENE_VISUAL_SMALL ) ? SmallRadius : OutdoorRadius;
    return distance( float3( s.World0.w, s.World1.w, s.World2.w ), CamPos ) < radius;
}

// D3D12RayTracing.cpp's SwayReach: how far a swaying instance's drawn surface strays from its rest-pose BLAS.
float SwayReach( SceneInstance s )
{
    if ( s.WindStrength <= 0.0 && s.CanBeAffectedByPlayer <= 0.0 ) return 0.0;
    const float local = max( s.WindStrength, 0.0 ) * 16.0 * 1.4 + ( s.CanBeAffectedByPlayer > 0.0 ? 38.0 : 0.0 );
    const float3 c0 = float3( s.World0.x, s.World1.x, s.World2.x );
    const float3 c1 = float3( s.World0.y, s.World1.y, s.World2.y );
    const float3 c2 = float3( s.World0.z, s.World1.z, s.World2.z );
    const float scaleSq = max( dot( c0, c0 ), max( dot( c1, c1 ), dot( c2, c2 ) ) );
    return local * sqrt( scaleSq ) * 2.0 + 5.0;
}

void WriteInstance( uint index, SceneInstance s, VisualRt rt, bool mob )
{
    const float reach = SwayReach( s );
    const uint at = index * 64u;
    InstanceDescs.Store4( at,       asuint( s.World0 ) );
    InstanceDescs.Store4( at + 16u, asuint( s.World1 ) );
    InstanceDescs.Store4( at + 32u, asuint( s.World2 ) );
    // InstanceID:24 | InstanceMask:8, ContributionToHitGroupIndex:24 | Flags:8, then the BLAS address
    const uint mask = mob ? kMaskDynamic : kMaskVob;
    const uint flags = reach > 0.0 ? kInstanceForceNonOpaque : 0u;
    InstanceDescs.Store4( at + 48u, uint4( ( GeomBase + rt.TemplateBase ) | ( mask << 24 ), flags << 24, rt.Blas.x, rt.Blas.y ) );
    RtInstance inst;
    inst.Color = s.Color;
    inst.SwayReach = reach;
    inst.Pad1 = 0u;
    inst.Pad2 = 0u;
    Instances[index] = inst;
}

[numthreads( 1, 1, 1 )]
void CSClear()
{
    Feedback.Store( 0, 0u );
}

groupshared uint gInRange;

[numthreads( 64, 1, 1 )]
void CSInstances( uint3 gid : SV_GroupID, uint gtid : SV_GroupIndex )
{
    const uint visualIdx = gid.x;   // one group per visual, uniform across the group
    if ( gtid == 0 ) gInRange = 0u;
    GroupMemoryBarrierWithGroupSync();

    const SceneVisual v = Visuals[visualIdx];
    const VisualRt rt = VisualRts[visualIdx];
    const bool traced = any( rt.Blas != 0u );
    const bool mob = ( v.SceneFlags & SCENE_VISUAL_MOB ) != 0u;
    for ( uint i = gtid; i < v.InstanceCount; i += 64u )
    {
        const SceneInstance s = Table[v.InstanceBase + i];
        const bool inRange = InRange( v, s );
        const uint hits = WaveActiveCountBits( inRange );
        if ( hits == 0u ) continue;
        if ( WaveIsFirstLane() ) gInRange = 1u;
        if ( !traced ) continue;
        // One counter atomic per wave; the count goes on past the budget, so the CPU sees what was wanted.
        uint first = 0u;
        if ( WaveIsFirstLane() ) Feedback.InterlockedAdd( 0, hits, first );
        first = WaveReadLaneFirst( first );
        const uint slot = first + WavePrefixCountBits( inRange );
        if ( inRange && slot < Budget ) WriteInstance( OutputBase + slot, s, rt, mob );
    }

    // BLAS geometry g is near template g; its material comes from the caster template, which knows residency.
    if ( traced )
    {
        const uint nearCount = rt.Layout & 0xFFFFu, casterOffset = rt.Layout >> 16;
        for ( uint g = gtid; g < nearCount; g += 64u )
        {
            const SceneTemplate n = Templates[rt.TemplateBase + g];
            const SceneTemplate c = Templates[rt.TemplateBase + casterOffset + g];
            const uint resolved = SCENE_TEMPLATE_READY | SCENE_TEMPLATE_RESOLVED;
            RtGeom rec;
            rec.BaseVertex = asuint( n.BaseVertex );
            rec.StartIndex = n.StartIndex;
            rec.Material = ( c.Flags & resolved ) == resolved
                ? ( c.MatDiffuseIndex & kMatSlotMask ) | ( ( c.Flags & SCENE_TEMPLATE_ALPHA ) ? kMatAlphaTest : 0u )
                : kMatNoTexture;
            rec.Kind = kKindVob;
            Geoms[GeomBase + rt.TemplateBase + g] = rec;
        }
    }

    GroupMemoryBarrierWithGroupSync();
    if ( gtid == 0 ) Feedback.Store( 4u + visualIdx * 4u, gInRange );
}

[numthreads( 64, 1, 1 )]
void CSTail( uint3 dtid : SV_DispatchThreadID )
{
    const uint used = min( Feedback.Load( 0 ), Budget );
    if ( dtid.x < used || dtid.x >= Budget ) return;
    const uint at = ( OutputBase + dtid.x ) * 64u;
    [unroll] for ( uint o = 0u; o < 64u; o += 16u ) InstanceDescs.Store4( at + o, uint4( 0u, 0u, 0u, 0u ) );
}
