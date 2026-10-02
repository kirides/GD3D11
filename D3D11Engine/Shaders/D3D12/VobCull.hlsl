// GPU-driven instanced-VOB culling (D3D12Cull.cpp). Replaces the CPU per-VOB frustum test: the CPU now
// only DISTANCE-culls when collecting (GothicAPI's BSP walk, RndCullContext::drawFlags.SkipVobFrustumCull),
// uploads every in-range instance, and this pass decides visibility on the GPU.
//
//   CSCull      : per instance, frustum-test its world AABB and Hi-Z-occlusion-test it against the WORLD-MESH
//                 depth prepass, then COMPACT the survivors into the front of that visual's instance range.
//   CSPatchArgs : per ExecuteIndirect command, overwrite DrawIndexedInstanced::InstanceCount with the
//                 surviving count for that command's visual (0 == the whole visual was culled).
//
// Both the depth prepass and the lit color pass ExecuteIndirect over the same patched argument buffer, so one
// cull serves both. The CSM cascades cull only the GPU scene's static casters here (VOB_SHADOW); their other
// casters keep the CPU cull and draw from the uncompacted per-frame instance ring.
//
// Everything is bound through root descriptors / root constants (no descriptor tables); the Hi-Z pyramid is
// the one texture and it comes in bindlessly (SM6.6 ResourceDescriptorHeap) as a full-mip-chain SRV.

// Mirrors D3D12GraphicsEngine::VobCullVisual (32 B) — one record per visible VISUAL, describing where its
// instances live in the ring and the LOCAL-space bounding box shared by all of them.
struct VobCullVisual
{
    float3 BBoxMin;
    uint   InstanceBase;    // first instance index into the instance buffer (ring offset / sizeof(instance))
    float3 BBoxMax;
    uint   InstanceCount;
    // 0 = keep every instance in the near run. 1 = split at LodDistance (far run draws the simplified LOD
    // indices). Commands are emitted per sub-mesh, so splitting a visual whose far bucket has no command
    // strands those instances and they vanish from the main view — which is why the CPU only raises this
    // once it has emitted the far commands.
    uint   SplitMode;
    uint   SceneFlags;      // GPU scene: SCENE_VISUAL_*; 0 for ring records
};
#define SCENE_VISUAL_SMALL  1u   // outdoor instances use SmallRadius instead of OutdoorRadius
#define VOB_SPLIT_NONE  0u
#define VOB_SPLIT_LOD   1u

// Mirrors ConstantBufferStructs.h's VobInstanceInfo (144 B) — the per-instance VERTEX stream, read here as a
// structured buffer instead. World/PrevWorld are stored ROW-major and consumed as `mul( float4(p,1), world )`
// in Vob.hlsl's VS, so the four float4s below are the matrix ROWS (see BuildWorldMatrix).
struct VobInstanceGpu
{
    float4 World0, World1, World2;      // VobInstanceInfo::world — the (0,0,0,1) row dropped
    uint   Color;
    float  WindStrength;
    float  CanBeAffectedByPlayer;
    uint   GPSlot;
#if !VOB_NO_MOTION
    float4 PrevWorld0, PrevWorld1, PrevWorld2;   // last — the no-motion upload stops before it
#endif
};

// GPU scene (D3D12GpuScene): the persistent table holds static instances without a previous transform, and
// flags in GPSlot that only the cull reads.
struct SceneInstanceGpu
{
    float4 World0, World1, World2;
    uint   Color;
    float  WindStrength;
    float  CanBeAffectedByPlayer;
    uint   GPSlot;
};
#define SCENE_GPSLOT_FOCUS   0x80000000u   // Vob.hlsl's focus highlight
#define SCENE_GPSLOT_HIDDEN  0x20000000u   // not drawn (hidden, or handed to the CPU path)
#define SCENE_GPSLOT_INDOOR  0x10000000u   // IndoorRadius instead of the visual's DrawRadius

//--------------------------------------------------------------------------------------
// Pass 1 — cull + compact
//--------------------------------------------------------------------------------------
cbuffer VobCullCB : register( b0 )
{
    float4x4 CullViewProj;      // same reversed-Z ViewProj the VOB passes use
    uint     VisualCount;
    uint     HiZIndex;          // SRV heap slot of the Hi-Z pyramid (full mip chain)
    uint     HiZWidth;          // Hi-Z mip-0 dimensions (half the render resolution)
    uint     HiZHeight;
    uint     HiZMipCount;
    uint     EnableOcclusion;   // 0 -> frustum cull only (debug toggle / no Hi-Z available)
    // 0 = no LOD split. Bucketing must be per INSTANCE: Gothic reuses a few hundred visuals map-wide, so a
    // per-visual decision would let one nearby barrel force full detail on every barrel in the world.
    float    LodDistance;       // @88
    float    MinMeshSize;       // @92  GPU scene casters: visuals with a smaller bbox diagonal are skipped
    float3   CullCamPosWS;      // @96
    uint     OutputOffset;      // @108 added to InstanceBase for the output element; 0 for the ring
    float    IndoorRadius;      // @112 GPU scene draw distances (pivot to camera)
    float    OutdoorRadius;     // @116
    float    SmallRadius;       // @120
    uint     FocusSlot;         // @124 GPU scene: table index of the focused vob, 0xFFFFFFFF = none
};                              // -> 128 B == 32 root constants

StructuredBuffer<VobCullVisual>    Visuals       : register( t0 );
#if VOB_SCENE
StructuredBuffer<SceneInstanceGpu> InInstances   : register( t1 );
#else
StructuredBuffer<VobInstanceGpu>   InInstances   : register( t1 );
#endif
RWStructuredBuffer<VobInstanceGpu> OutInstances  : register( u0 );
RWStructuredBuffer<uint>           VisibleCounts : register( u1 );

float3x4 BuildWorldMatrix( VobInstanceGpu inst )
{
    // The rows are the matrix, as in Vob.hlsl's row_major iworld input.
    return float3x4( inst.World0, inst.World1, inst.World2 );
}

#if VOB_SCENE
// The draw instance for a table entry: no motion of its own, so the previous transform is the current one.
VobInstanceGpu ExpandSceneInstance( SceneInstanceGpu s, uint tableIndex )
{
    VobInstanceGpu o;
    o.World0 = s.World0; o.World1 = s.World1; o.World2 = s.World2;
    o.Color = s.Color;
    o.WindStrength = s.WindStrength;
    o.CanBeAffectedByPlayer = s.CanBeAffectedByPlayer;
    o.GPSlot = ( s.GPSlot & ~SCENE_GPSLOT_FOCUS ) | ( tableIndex == FocusSlot ? SCENE_GPSLOT_FOCUS : 0u );
#if !VOB_NO_MOTION
    o.PrevWorld0 = s.World0; o.PrevWorld1 = s.World1; o.PrevWorld2 = s.World2;
#endif
    return o;
}

// The draw-distance and hidden tests the CPU leaf walk used to make.
bool IsSceneInstanceInRange( VobCullVisual v, SceneInstanceGpu s )
{
    if ( s.GPSlot & SCENE_GPSLOT_HIDDEN ) return false;
#if VOB_SHADOW
    // The cascades' walk: no indoor casters, none smaller than a few of this cascade's texels.
    if ( s.GPSlot & SCENE_GPSLOT_INDOOR ) return false;
    if ( length( v.BBoxMax - v.BBoxMin ) < MinMeshSize ) return false;
#endif
    const float radius = ( s.GPSlot & SCENE_GPSLOT_INDOOR ) ? IndoorRadius
        : ( v.SceneFlags & SCENE_VISUAL_SMALL ) ? SmallRadius : OutdoorRadius;
    const float3 pivot = float3( s.World0.w, s.World1.w, s.World2.w );
    return distance( pivot, CullCamPosWS ) < radius;
}
#endif

#if VOB_SHADOW
// Shadow cascade: CullViewProj maps the cascade's cull box (Frustum::BuildOrthographic) onto x,y in [-1,1],
// z in [0,1], with w = 1. No occlusion test, the casters may sit outside the player's view.
bool IsInstanceVisible( VobCullVisual v, VobInstanceGpu inst )
{
    if ( any( v.BBoxMin > v.BBoxMax ) )
        return true;

    const float3x4 world = BuildWorldMatrix( inst );
    bool outNegX = true, outPosX = true, outNegY = true, outPosY = true, outNear = true, outFar = true;
    [unroll]
    for ( uint c = 0; c < 8; ++c )
    {
        float3 corner = float3(
            ( c & 1 ) ? v.BBoxMax.x : v.BBoxMin.x,
            ( c & 2 ) ? v.BBoxMax.y : v.BBoxMin.y,
            ( c & 4 ) ? v.BBoxMax.z : v.BBoxMin.z );
        const float3 p = mul( float4( mul( world, float4( corner, 1.0 ) ), 1.0 ), CullViewProj ).xyz;
        outNegX = outNegX && ( p.x < -1.0 );
        outPosX = outPosX && ( p.x >  1.0 );
        outNegY = outNegY && ( p.y < -1.0 );
        outPosY = outPosY && ( p.y >  1.0 );
        outNear = outNear && ( p.z <  0.0 );
        outFar  = outFar  && ( p.z >  1.0 );
    }
    return !( outNegX || outPosX || outNegY || outPosY || outNear || outFar );
}
#else
bool IsInstanceVisible( VobCullVisual v, VobInstanceGpu inst )
{
    // A degenerate/uninitialised visual box would collapse to a point and get culled at almost any angle;
    // treat it as always visible rather than risk making geometry disappear.
    if ( any( v.BBoxMin > v.BBoxMax ) )
        return true;

    const float3x4 world = BuildWorldMatrix( inst );

    // Frustum reject: a box is outside only when ALL EIGHT corners fall outside the SAME clip plane. (Testing
    // per-plane like this can keep a box that is outside the frustum but not outside any single plane — that
    // is fine, it only ever under-culls.) Reversed-Z with an infinite far plane: the visible range is
    // 0 <= z <= w, so there is no far-plane rejection at all, only the near plane (z > w).
    bool outNegX = true, outPosX = true, outNegY = true, outPosY = true, outNear = true;

    // Screen-space extent + the closest depth over the box, for the Hi-Z test.
    float2 uvMin =  float2( 1e30, 1e30 );
    float2 uvMax = -float2( 1e30, 1e30 );
    float  closestDepth = 0.0;      // reversed-Z: bigger == closer, 0 == infinitely far
    bool   allInFront = true;       // every corner strictly in front of the eye (w > 0)

    [unroll]
    for ( uint c = 0; c < 8; ++c )
    {
        float3 corner = float3(
            ( c & 1 ) ? v.BBoxMax.x : v.BBoxMin.x,
            ( c & 2 ) ? v.BBoxMax.y : v.BBoxMin.y,
            ( c & 4 ) ? v.BBoxMax.z : v.BBoxMin.z );

        float4 clip = mul( float4( mul( world, float4( corner, 1.0 ) ), 1.0 ), CullViewProj );

        outNegX = outNegX && ( clip.x < -clip.w );
        outPosX = outPosX && ( clip.x >  clip.w );
        outNegY = outNegY && ( clip.y < -clip.w );
        outPosY = outPosY && ( clip.y >  clip.w );
        outNear = outNear && ( clip.z >  clip.w );

        if ( clip.w > 1e-4 )
        {
            float3 ndc = clip.xyz / clip.w;
            float2 uv  = ndc.xy * float2( 0.5, -0.5 ) + 0.5;
            uvMin = min( uvMin, uv );
            uvMax = max( uvMax, uv );
            closestDepth = max( closestDepth, ndc.z );
        }
        else
        {
            allInFront = false;
        }
    }

    if ( outNegX || outPosX || outNegY || outPosY || outNear )
        return false;

    // Occlusion test. Skipped when the box straddles the eye plane (its screen-space extent is then unbounded
    // and the projected rect meaningless) — a box that close is almost certainly visible anyway.
    if ( EnableOcclusion == 0 || HiZMipCount == 0 || !allInFront )
        return true;

    Texture2D<float> hiz = ResourceDescriptorHeap[HiZIndex];

    float2 hizSize = float2( HiZWidth, HiZHeight );
    float2 p0 = saturate(uvMin) * hizSize;
    float2 p1 = saturate(uvMax) * hizSize;

    // Pick the level where the box's footprint spans at most 2x2 texels, so four taps cover it completely.
    float2 extent = max( p1 - p0, 1e-4 );
    int    mip    = (int)clamp( ceil( log2( max( extent.x, extent.y ) ) ), 0.0, (float)( HiZMipCount - 1 ) );

    float  mipScale = exp2( (float)mip );
    int2   mipSize  = max( int2( HiZWidth >> mip, HiZHeight >> mip ), int2( 1, 1 ) );
    int2   t0 = clamp( int2( p0 / mipScale ), int2( 0, 0 ), mipSize - 1 );
    int2   t1 = clamp( int2( p1 / mipScale ), int2( 0, 0 ), mipSize - 1 );

    float occluderDepth = hiz.Load( int3( t0.x, t0.y, mip ) );
    occluderDepth = min( occluderDepth, hiz.Load( int3( t1.x, t0.y, mip ) ) );
    occluderDepth = min( occluderDepth, hiz.Load( int3( t0.x, t1.y, mip ) ) );
    occluderDepth = min( occluderDepth, hiz.Load( int3( t1.x, t1.y, mip ) ) );

    // Occluded when the FARTHEST world-mesh pixel over the footprint is still CLOSER than the box's nearest
    // point (reversed-Z: greater == closer). No epsilon: the tested depth belongs to the world mesh, never to
    // the VOB itself, so there is no self-occlusion to bias against — and the min-reduced pyramid plus the
    // "any corner in front" bail-out already err on the side of keeping geometry.
    return !( occluderDepth > closestDepth );
}
#endif

// One thread group per VISUAL: the group owns that visual's whole instance range, so the compaction counters
// can live in groupshared memory and only the final counts need a buffer write (no global atomics at all).
#define VOBCULL_GROUP_SIZE 64
// Near packs FORWARD from InstanceBase, far packs BACKWARD from the end. Packing far backward is what keeps
// this single-pass: head-to-tail would need the near total before placing the first far instance. Every
// visible instance lands in exactly one run, so near + far <= InstanceCount and they cannot collide.
groupshared uint gNearInGroup;
groupshared uint gFarInGroup;

[numthreads(VOBCULL_GROUP_SIZE, 1, 1)]
void CSCull( uint3 gid : SV_GroupID, uint gtid : SV_GroupIndex )
{
    const uint visualIdx = gid.x;   // uniform across the group -> the barriers below are never divergent

    if ( gtid == 0 )
    {
        gNearInGroup = 0;
        gFarInGroup  = 0;
    }
    GroupMemoryBarrierWithGroupSync();

    if ( visualIdx < VisualCount )
    {
        VobCullVisual v = Visuals[visualIdx];
        for ( uint i = gtid; i < v.InstanceCount; i += VOBCULL_GROUP_SIZE )
        {
#if VOB_SCENE
            const SceneInstanceGpu sceneInst = InInstances[v.InstanceBase + i];
            VobInstanceGpu inst = ExpandSceneInstance( sceneInst, v.InstanceBase + i );
            const bool inRange = IsSceneInstanceInRange( v, sceneInst );
#else
            VobInstanceGpu inst = InInstances[v.InstanceBase + i];
            const bool inRange = true;
#endif

            // NOTE: no early-out/continue on visibility -- every lane must reach the wave ops below with the
            // same activity mask, so an invisible instance falls through with both predicates false instead
            // of exiting the loop.
            bool isFar = false;
            const bool visible = inRange && IsInstanceVisible( v, inst );
            if ( visible )
            {
                // Bbox centre, not the origin: Gothic vob pivots are often off the mesh entirely (a door's
                // hinge, a banner's mount point).
                const float3 centreLocal = ( v.BBoxMin + v.BBoxMax ) * 0.5;
                const float3 centreWorld = mul( BuildWorldMatrix( inst ), float4( centreLocal, 1.0 ) );
                const float  dist  = distance(centreWorld, CullCamPosWS);
                const float  splitDist = ( v.SplitMode == VOB_SPLIT_LOD ) ? LodDistance : 0.0;
                isFar = ( splitDist > 0.0 ) && ( dist > splitDist );
            }
            const bool isNear = visible && !isFar;
            const bool isFarVisible = visible && isFar;

            // One LDS atomic per wave instead of one per surviving instance: the wave reserves its whole run
            // and each lane takes its slot from the prefix popcount. Wave-size agnostic (no ballot-word
            // assumptions) -- mirrors LightCull.hlsl's Phase 1 compaction.
            const uint nearHits = WaveActiveCountBits( isNear );
            if ( nearHits != 0 )
            {
                uint nearBase = 0;
                if ( WaveIsFirstLane() ) InterlockedAdd( gNearInGroup, nearHits, nearBase );
                nearBase = WaveReadLaneFirst( nearBase );
                if ( isNear ) OutInstances[OutputOffset + v.InstanceBase + nearBase + WavePrefixCountBits( isNear )] = inst;
            }

            const uint farHits = WaveActiveCountBits( isFarVisible );
            if ( farHits != 0 )
            {
                uint farBase = 0;
                if ( WaveIsFirstLane() ) InterlockedAdd( gFarInGroup, farHits, farBase );
                farBase = WaveReadLaneFirst( farBase );
                if ( isFarVisible )
                    OutInstances[OutputOffset + v.InstanceBase + v.InstanceCount - 1 - ( farBase + WavePrefixCountBits( isFarVisible ) )] = inst;
            }
        }
    }

    GroupMemoryBarrierWithGroupSync();
    if ( gtid == 0 && visualIdx < VisualCount )
    {
        VisibleCounts[visualIdx * 2u] = gNearInGroup;   // CSPatchArgs indexes (visualIdx * 2 + bucket)
        VisibleCounts[visualIdx * 2u + 1u] = gFarInGroup;
    }
}

//--------------------------------------------------------------------------------------
// Pass 2 — patch the indirect argument buffer
//--------------------------------------------------------------------------------------
cbuffer VobPatchCB : register( b0 )
{
    uint PatchCmdCount;
    uint PatchCmdStride;          // sizeof(VobDrawCommand)
    uint PatchInstCountOffset;    // byte offset of Draw.InstanceCount within a command
    uint PatchVisualIdxOffset;    // byte offset of VobDrawCommand::VisualIndex
    uint PatchStartInstOffset;    // byte offset of Draw.StartInstanceLocation
    uint PatchLodBucketOffset;    // byte offset of VobDrawCommand::LodBucket
    uint _patchPad0;
};

StructuredBuffer<uint>          PatchCounts  : register( t0 );
// Needed for InstanceBase (the visual's block in the shared instance buffer) and InstanceCount (the far run
// is packed against the END of that block, so its start is InstanceCount - farCount and cannot be derived
// from the survivor counts alone).
StructuredBuffer<VobCullVisual> PatchVisuals : register( t1 );
RWByteAddressBuffer             PatchArgs    : register( u0 );

[numthreads(64, 1, 1)]
void CSPatchArgs( uint3 DTid : SV_DispatchThreadID )
{
    if ( DTid.x >= PatchCmdCount )
        return;

    uint base       = DTid.x * PatchCmdStride;
    uint visualIdx  = PatchArgs.Load( base + PatchVisualIdxOffset );

    // 0xFFFFFFFF = "not culled" (the visual overflowed the cull-record cap, so the CPU pointed this command at
    // the uncompacted ring and wrote the full instance count). Leave the CPU's count alone.
    if ( visualIdx == 0xFFFFFFFF )
        return;

    const uint bucket = PatchArgs.Load( base + PatchLodBucketOffset );
    const uint count  = PatchCounts[visualIdx * 2u + bucket];
    // ABSOLUTE, not range-relative. With the VOB mega-buffers there is no per-command instance VBV any more
    // — each pass binds the whole instance buffer once — so StartInstanceLocation has to carry the visual's
    // block base itself. Near packs forward from the base, far backward from the end of the block.
    const VobCullVisual v = PatchVisuals[visualIdx];
    const uint start = v.InstanceBase + ( ( bucket == 0u ) ? 0u : ( v.InstanceCount - count ) );

    PatchArgs.Store( base + PatchInstCountOffset, count );
    PatchArgs.Store( base + PatchStartInstOffset, start );
}

//--------------------------------------------------------------------------------------
// GPU scene — generate the draw commands of every visual with survivors
//--------------------------------------------------------------------------------------
// Mirrors D3D12GpuScene::Template: the VobDrawCommand fields that do not depend on the cull.
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
    uint  VisualIndex;      // scene record
    uint  LodBucket;        // caster template: the LOD index count
    uint  Flags;            // SCENE_TEMPLATE_*
    uint  CasterLodStart;   // caster template: the first LOD index
};
#define SCENE_TEMPLATE_ALPHA   1u   // goes into the alpha-tested list
#define SCENE_TEMPLATE_READY   2u   // main view: its textures are resolved; not drawn until then
#define SCENE_TEMPLATE_CASTER  4u   // a shadow-caster template; IndexCount/StartIndex name its near-cascade range

cbuffer SceneArgsCB : register( b0 )
{
    uint TemplateCount;
    uint OpaqueCapacity;    // commands in the opaque list, which starts at command 0
    uint AlphaCapacity;     // commands in the alpha list, which starts at command OpaqueCapacity
    uint SceneOutputOffset; // the cull's OutputOffset
    uint UseLodIndices;     // casters: draw the LOD range (outer cascades)
};

StructuredBuffer<SceneTemplate> Templates     : register( t0 );
StructuredBuffer<VobCullVisual> SceneVisuals  : register( t1 );
StructuredBuffer<uint>          SceneCounts   : register( t2 );
RWByteAddressBuffer             SceneArgs     : register( u0 );   // VobDrawCommand x (OpaqueCapacity + AlphaCapacity)
RWByteAddressBuffer             SceneArgCount : register( u1 );   // [0] opaque, [4] alpha: the ExecuteIndirect counts

#define VOB_DRAW_COMMAND_STRIDE 48u

[numthreads(1, 1, 1)]
void CSClearCounts()
{
    SceneArgCount.Store2( 0, uint2( 0, 0 ) );
}

void AppendSceneCommand( SceneTemplate t, uint indexCount, uint startIndex, uint count, uint start, uint lodBucket )
{
    const bool alpha = ( t.Flags & SCENE_TEMPLATE_ALPHA ) != 0u;
    uint slot;
    SceneArgCount.InterlockedAdd( alpha ? 4u : 0u, 1u, slot );
    if ( slot >= ( alpha ? AlphaCapacity : OpaqueCapacity ) ) return;   // the draw clamps the count to capacity

    const uint at = ( ( alpha ? OpaqueCapacity : 0u ) + slot ) * VOB_DRAW_COMMAND_STRIDE;
    SceneArgs.Store4( at,      uint4( t.MatNormalIndex, t.MatOrmIndex, t.MatDiffuseIndex, asuint( t.WindMinHeight ) ) );
    SceneArgs.Store4( at + 16, uint4( asuint( t.WindMaxHeight ), indexCount, count, startIndex ) );
    SceneArgs.Store4( at + 32, uint4( asuint( t.BaseVertex ), start, t.VisualIndex, lodBucket ) );
}

[numthreads(64, 1, 1)]
void CSBuildArgs( uint3 DTid : SV_DispatchThreadID )
{
    if ( DTid.x >= TemplateCount ) return;
    const SceneTemplate t = Templates[DTid.x];
    if ( ( t.Flags & ( SCENE_TEMPLATE_READY | SCENE_TEMPLATE_CASTER ) ) != SCENE_TEMPLATE_READY ) return;
    const uint count = SceneCounts[t.VisualIndex * 2u + t.LodBucket];
    if ( count == 0u ) return;

    // Near packs forward from the output base, far backward from its end (see CSCull).
    const VobCullVisual v = SceneVisuals[t.VisualIndex];
    const uint start = SceneOutputOffset + v.InstanceBase + ( t.LodBucket == 0u ? 0u : v.InstanceCount - count );
    AppendSceneCommand( t, t.IndexCount, t.StartIndex, count, start, t.LodBucket );
}

// One shadow cascade's caster commands, over the counts its CSCull (VOB_SHADOW) wrote. Casters never split.
[numthreads(64, 1, 1)]
void CSBuildCasterArgs( uint3 DTid : SV_DispatchThreadID )
{
    if ( DTid.x >= TemplateCount ) return;
    const SceneTemplate t = Templates[DTid.x];
    if ( ( t.Flags & ( SCENE_TEMPLATE_READY | SCENE_TEMPLATE_CASTER ) ) != ( SCENE_TEMPLATE_READY | SCENE_TEMPLATE_CASTER ) ) return;
    const uint count = SceneCounts[t.VisualIndex * 2u];
    if ( count == 0u ) return;

    const uint start = SceneOutputOffset + SceneVisuals[t.VisualIndex].InstanceBase;
    AppendSceneCommand( t, UseLodIndices ? t.LodBucket : t.IndexCount,
        UseLodIndices ? t.CasterLodStart : t.StartIndex, count, start, 0u );
}
