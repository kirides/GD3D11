//--------------------------------------------------------------------------------------
// GPU Frustum + Distance Culling Compute Shader
// Tests each vob AABB against 6 frustum planes + draw distance,
// writes visible instances to RWStructuredBuffer and atomically
// increments InstanceCount in the indirect args buffer.
//--------------------------------------------------------------------------------------

cbuffer CullCB : register( b0 )
{
    float4 frustumPlanes[6];
    float3 cameraPosition;
    float drawDistance;
    float globalWindStrength;
    uint windAdvanced;
    uint numVobs;
    uint feedbackFrameNumber;
    uint enableHiZ;
    uint hiZMipCount;
    float hiZWidth;
    float hiZHeight;
    float4x4 viewProjection;
};

struct VobGPUData
{
    float3 aabbCenter;
    float pad0;
    float3 aabbExtent;
    float pad1;
    float4x4 world;
    float4x4 prevWorld;
    uint color;
    float aniModeStrength;
    float canBeAffectedByPlayer;
    uint submeshStart;
    uint submeshCount;
    float minHeight;
    float maxHeight;
    uint pad2;
};

struct SubmeshGPUData
{
    int slice;
    float uStart;
    float vStart;
    float uEnd;
    float vEnd;
    uint argIndex;
    uint instanceBaseOffset;
    uint globalSourceIndex;
};

struct VobInstanceInfoAtlas
{
    float4x4 world;
    float4x4 prevWorld;
    uint color;
    float windStrength;
    float canBeAffectedByPlayer;
    int slice;
    float uStart;
    float vStart;
    float uEnd;
    float vEnd;
    uint globalSourceIndex;
    float minHeight;
    float maxHeight;
};

StructuredBuffer<VobGPUData> VobBuffer : register( t0 );
StructuredBuffer<SubmeshGPUData> SubmeshBuffer : register( t1 );
Texture2D<float> HiZTexture : register( t2 );
RWStructuredBuffer<VobInstanceInfoAtlas> InstanceOutput : register( u0 );
RWByteAddressBuffer IndirectArgsUAV : register( u1 );

// GPU feedback for streaming: source-indexed RWTexture2D<uint>
// The CS stamps visible sources once per (vob, submesh) — orders of magnitude
// cheaper than per-pixel atomics in the pixel shader.
RWTexture2D<uint> FeedbackUAV : register( u5 );

// Hi-Z occlusion test: project AABB to screen, pick mip level, compare depth.
// Returns true if the AABB is OCCLUDED (should be culled).
bool IsOccludedHiZ( float3 aabbCenter, float3 aabbExtent )
{
    // Generate all 8 corners of the AABB
    float3 corners[8];
    corners[0] = aabbCenter + float3( -aabbExtent.x, -aabbExtent.y, -aabbExtent.z );
    corners[1] = aabbCenter + float3(  aabbExtent.x, -aabbExtent.y, -aabbExtent.z );
    corners[2] = aabbCenter + float3( -aabbExtent.x,  aabbExtent.y, -aabbExtent.z );
    corners[3] = aabbCenter + float3(  aabbExtent.x,  aabbExtent.y, -aabbExtent.z );
    corners[4] = aabbCenter + float3( -aabbExtent.x, -aabbExtent.y,  aabbExtent.z );
    corners[5] = aabbCenter + float3(  aabbExtent.x, -aabbExtent.y,  aabbExtent.z );
    corners[6] = aabbCenter + float3( -aabbExtent.x,  aabbExtent.y,  aabbExtent.z );
    corners[7] = aabbCenter + float3(  aabbExtent.x,  aabbExtent.y,  aabbExtent.z );

    float minX = 1.0, minY = 1.0, maxX = 0.0, maxY = 0.0;
    float maxDepth = 0.0; // Reversed-Z: nearest corner has the highest Z. Track max across corners.

    [unroll]
    for ( int i = 0; i < 8; i++ )
    {
        float4 clip = mul( float4( corners[i], 1.0 ), viewProjection );

        // Behind camera — can't occlude, bail out as visible
        if ( clip.w <= 0.0 )
            return false;

        float3 ndc = clip.xyz / clip.w;

        // NDC to UV [0,1] range (Y is flipped for texture space)
        float u = ndc.x * 0.5 + 0.5;
        float v = -ndc.y * 0.5 + 0.5;

        minX = min( minX, u );
        maxX = max( maxX, u );
        minY = min( minY, v );
        maxY = max( maxY, v );

        // Track the nearest AABB corner (highest Z in reversed-Z)
        maxDepth = max( maxDepth, ndc.z );
    }

    // Clamp to screen bounds
    minX = saturate( minX );
    maxX = saturate( maxX );
    minY = saturate( minY );
    maxY = saturate( maxY );

    // Degenerate or off-screen — treat as visible
    if ( minX >= maxX || minY >= maxY )
        return false;

    // Compute screen-space size in pixels at mip 0
    float sizeX = ( maxX - minX ) * hiZWidth;
    float sizeY = ( maxY - minY ) * hiZHeight;
    float maxSize = max( sizeX, sizeY );

    // Pick mip level: we want the mip where the AABB covers roughly 2x2 texels
    float mipF = ceil( log2( max( maxSize, 1.0 ) ) );
    uint mip = min( (uint)mipF, hiZMipCount - 1 );

    // Compute texel coordinates at this mip level
    float mipWidth = max( hiZWidth / (float)( 1u << mip ), 1.0 );
    float mipHeight = max( hiZHeight / (float)( 1u << mip ), 1.0 );

    int2 texMin = int2( minX * mipWidth, minY * mipHeight );
    int2 texMax = int2( maxX * mipWidth, maxY * mipHeight );

    // Clamp to valid range
    texMin = max( texMin, int2( 0, 0 ) );
    texMax = min( texMax, int2( (int)mipWidth - 1, (int)mipHeight - 1 ) );

    // Sample Hi-Z: take the min depth across the covered texels.
    // MIN mip chain stores farthest depth per texel (reversed-Z: smallest Z = farthest).
    // We take min across texels to get the overall farthest surface — conservative.
    float hiZDepth = 1.0;
    for ( int y = texMin.y; y <= texMax.y; y++ )
    {
        for ( int x = texMin.x; x <= texMax.x; x++ )
        {
            hiZDepth = min( hiZDepth, HiZTexture.Load( int3( x, y, mip ) ) );
        }
    }

    // Reversed-Z: near=1, far=0.
    // maxDepth = nearest AABB corner (highest Z in reversed-Z).
    // HiZ is a MAX mip chain: each texel = nearest surface (highest Z) in its region.
    // We take MIN across the AABB footprint texels to find the least-occluded tile.
    // AABB is occluded when its nearest corner is farther than the nearest surface
    // in every footprint tile, i.e. maxDepth < min(hiZMaxValues) = hiZDepth.
    return ( maxDepth < hiZDepth );
}

[numthreads( 64, 1, 1 )]
void CSMain( uint3 DTid : SV_DispatchThreadID )
{
    uint idx = DTid.x;
    if ( idx >= numVobs )
        return;

    VobGPUData vob = VobBuffer[idx];

    // Draw distance cull (center-to-camera distance)
    float3 toCamera = vob.aabbCenter - cameraPosition;
    float distSq = dot( toCamera, toCamera );
    if ( distSq > drawDistance * drawDistance )
        return;

    // Frustum cull: 6-plane AABB test
    [unroll]
    for ( int p = 0; p < 6; p++ )
    {
        float3 n = frustumPlanes[p].xyz;
        float d = frustumPlanes[p].w;
        float r = dot( abs( n ), vob.aabbExtent );
        float s = dot( n, vob.aabbCenter ) + d;
        if ( s - r > 0.0 )
            return; // fully outside this plane
    }

    // Hi-Z occlusion cull: test AABB against hierarchical depth buffer
    if ( enableHiZ )
    {
        if ( IsOccludedHiZ( vob.aabbCenter, vob.aabbExtent ) )
            return;
    }

    // Compute wind strength for this vob
    float windStr = 0.0;
    if ( vob.aniModeStrength > 0.0 && windAdvanced )
    {
        windStr = max( 0.1, vob.aniModeStrength ) * globalWindStrength;
    }

    // Emit one instance per submesh of this vob
    for ( uint s = 0; s < vob.submeshCount; s++ )
    {
        SubmeshGPUData sm = SubmeshBuffer[vob.submeshStart + s];

        // Atomic increment InstanceCount in the indirect args buffer.
        // Each D3D11_DRAW_INDEXED_INSTANCED_INDIRECT_ARGS is 20 bytes (5 x uint32):
        //   [0] IndexCountPerInstance
        //   [4] InstanceCount          <-- we increment this
        //   [8] StartIndexLocation
        //  [12] BaseVertexLocation
        //  [16] StartInstanceLocation
        uint slot;
        IndirectArgsUAV.InterlockedAdd( sm.argIndex * 20 + 4, 1, slot );

        // Write instance data at the pre-allocated offset + atomic slot
        VobInstanceInfoAtlas inst;
        inst.world = vob.world;
        inst.prevWorld = vob.prevWorld;
        inst.color = vob.color;
        inst.windStrength = windStr;
        inst.canBeAffectedByPlayer = vob.canBeAffectedByPlayer;
        inst.slice = sm.slice;
        inst.uStart = sm.uStart;
        inst.vStart = sm.vStart;
        inst.uEnd = sm.uEnd;
        inst.vEnd = sm.vEnd;
        inst.globalSourceIndex = sm.globalSourceIndex;
        inst.minHeight = vob.minHeight;
        inst.maxHeight = vob.maxHeight;

        InstanceOutput[sm.instanceBaseOffset + slot] = inst;

        // Stamp feedback: one atomic per visible (vob, submesh) pair.
        // Far cheaper than per-pixel atomics in the PS.
        if ( feedbackFrameNumber > 0 )
        {
            InterlockedMax( FeedbackUAV[uint2( sm.globalSourceIndex, 0 )], feedbackFrameNumber );
        }
    }
}
