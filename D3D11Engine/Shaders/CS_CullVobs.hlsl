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
    uint pad;
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
    uint pad2[3];
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
    uint pad;
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
};

StructuredBuffer<VobGPUData> VobBuffer : register( t0 );
StructuredBuffer<SubmeshGPUData> SubmeshBuffer : register( t1 );
RWStructuredBuffer<VobInstanceInfoAtlas> InstanceOutput : register( u0 );
RWByteAddressBuffer IndirectArgsUAV : register( u1 );

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

        InstanceOutput[sm.instanceBaseOffset + slot] = inst;
    }
}
