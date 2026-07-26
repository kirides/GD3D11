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
// cull serves both. The CSM shadow cascades are NOT culled here — they keep their own CPU cull against their
// own frustum (a caster invisible to the player still casts into view), and they draw from the uncompacted
// per-frame instance ring.
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
};

// Mirrors ConstantBufferStructs.h's VobInstanceInfo (144 B) — the per-instance VERTEX stream, read here as a
// structured buffer instead. World/PrevWorld are stored ROW-major and consumed as `mul( float4(p,1), world )`
// in Vob.hlsl's VS, so the four float4s below are the matrix ROWS (see BuildWorldMatrix).
struct VobInstanceGpu
{
    float4 World0, World1, World2, World3;
    float4 PrevWorld0, PrevWorld1, PrevWorld2, PrevWorld3;
    uint   Color;
    float  WindStrength;
    float  CanBeAffectedByPlayer;
    uint   GPSlot;
};

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
    uint     _cullPad0;
    uint     _cullPad1;
};

StructuredBuffer<VobCullVisual>    Visuals       : register( t0 );
StructuredBuffer<VobInstanceGpu>   InInstances   : register( t1 );
RWStructuredBuffer<VobInstanceGpu> OutInstances  : register( u0 );
RWStructuredBuffer<uint>           VisibleCounts : register( u1 );

float4x4 BuildWorldMatrix( VobInstanceGpu inst )
{
    // float4x4(a,b,c,d) fills ROWS, matching how the vertex stream feeds INSTANCE_WORLD_MATRIX — so
    // mul( float4(p,1), W ) here is bit-for-bit the transform Vob.hlsl's VSMain applies.
    return float4x4( inst.World0, inst.World1, inst.World2, inst.World3 );
}

bool IsInstanceVisible( VobCullVisual v, VobInstanceGpu inst )
{
    // A degenerate/uninitialised visual box would collapse to a point and get culled at almost any angle;
    // treat it as always visible rather than risk making geometry disappear.
    if ( any( v.BBoxMin > v.BBoxMax ) )
        return true;

    float4x4 wvp = mul( BuildWorldMatrix( inst ), CullViewProj );

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

        float4 clip = mul( float4( corner, 1.0 ), wvp );

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
    float2 p0 = clamp( uvMin, 0.0, 1.0 ) * hizSize;
    float2 p1 = clamp( uvMax, 0.0, 1.0 ) * hizSize;

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

// One thread group per VISUAL: the group owns that visual's whole instance range, so the compaction counter
// can live in groupshared memory and only the final count needs a buffer write (no global atomics at all).
#define VOBCULL_GROUP_SIZE 64
groupshared uint gVisibleInGroup;

[numthreads(VOBCULL_GROUP_SIZE, 1, 1)]
void CSCull( uint3 gid : SV_GroupID, uint gtid : SV_GroupIndex )
{
    const uint visualIdx = gid.x;   // uniform across the group -> the barriers below are never divergent

    if ( gtid == 0 )
        gVisibleInGroup = 0;
    GroupMemoryBarrierWithGroupSync();

    if ( visualIdx < VisualCount )
    {
        VobCullVisual v = Visuals[visualIdx];
        for ( uint i = gtid; i < v.InstanceCount; i += VOBCULL_GROUP_SIZE )
        {
            VobInstanceGpu inst = InInstances[v.InstanceBase + i];
            if ( IsInstanceVisible( v, inst ) )
            {
                // Survivors are packed to the front of the SAME range the CPU uploaded into, so the
                // per-command InstVBV (base address + size) needs no GPU patching — only InstanceCount does.
                // Instance ORDER within a visual is irrelevant (opaque, one material per command).
                uint slot;
                InterlockedAdd( gVisibleInGroup, 1, slot );
                OutInstances[v.InstanceBase + slot] = inst;
            }
        }
    }

    GroupMemoryBarrierWithGroupSync();
    if ( gtid == 0 && visualIdx < VisualCount )
        VisibleCounts[visualIdx] = gVisibleInGroup;
}

//--------------------------------------------------------------------------------------
// Pass 2 — patch the indirect argument buffer
//--------------------------------------------------------------------------------------
cbuffer VobPatchCB : register( b0 )
{
    uint PatchCmdCount;
    uint PatchCmdStride;         // sizeof(VobDrawCommand)
    uint PatchInstCountOffset;   // byte offset of Draw.InstanceCount within a command
    uint PatchVisualIdxOffset;   // byte offset of VobDrawCommand::VisualIndex
};

StructuredBuffer<uint> PatchCounts : register( t0 );
RWByteAddressBuffer    PatchArgs   : register( u0 );

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

    PatchArgs.Store( base + PatchInstCountOffset, PatchCounts[visualIdx] );
}
