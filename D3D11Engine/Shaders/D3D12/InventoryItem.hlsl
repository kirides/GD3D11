// Inventory item previews (UIRenderer2D item batches). Mirrors Shaders/VS_InventoryItem*.hlsl + PS_Preview_Textured;
// instances come from a StructuredBuffer and the texture from the SRV heap, so static items go out as one ExecuteIndirect.
struct ItemInstance
{
    float4 clipRow0;    // object -> slot clip space (column vectors)
    float4 clipRow1;
    float4 clipRow2;
    float4 clipRow3;
    float4 remap;       // slot NDC -> target NDC: x*sx + w*ox, y*sy + w*oy
};

cbuffer DrawCB : register( b0 ) { uint InstanceIndex; uint TextureIndex; };
StructuredBuffer<ItemInstance> Instances : register( t0 );

static const int NUM_MAX_BONES = 96;
cbuffer BonesCB : register( b1 ) { float4x4 Bones[NUM_MAX_BONES]; };

SamplerState smp : register( s0 );

struct VS_OUT
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
    nointerpolation uint tex : TEXCOORD1;
    float4 clipDistance : SV_ClipDistance0;   // the slot's viewport edges
};

VS_OUT Project( float3 position, float2 uv )
{
    ItemInstance inst = Instances[InstanceIndex];
    float4 p = float4( position, 1.0 );
    float4 slot = float4( dot( inst.clipRow0, p ), dot( inst.clipRow1, p ), dot( inst.clipRow2, p ), dot( inst.clipRow3, p ) );

    VS_OUT o;
    o.clipDistance = float4( slot.w + slot.x, slot.w - slot.x, slot.w + slot.y, slot.w - slot.y );
    o.pos = float4( slot.x * inst.remap.x + slot.w * inst.remap.z, slot.y * inst.remap.y + slot.w * inst.remap.w, slot.z, slot.w );
    o.uv = uv;
    o.tex = TextureIndex;
    return o;
}

struct VS_IN { float3 pos : POSITION; float2 uv : TEXCOORD0; };

VS_OUT VSStatic( VS_IN i )
{
    return Project( i.pos, i.uv );
}

struct VS_SKEL_IN
{
    float4 pos[4]     : POSITION;    // 4 per-bone-space positions (half4)
    float3 normal     : NORMAL;
    float3 bindNormal : TEXCOORD0;
    float2 uv         : TEXCOORD1;
    uint4  boneIds    : BONEIDS;
    float4 weights    : WEIGHTS;
};

VS_OUT VSSkinned( VS_SKEL_IN i )
{
    float3 skinned = float3( 0, 0, 0 );
    [unroll]
    for ( int b = 0; b < 4; ++b )
        skinned += i.weights[b] * mul( float4( i.pos[b].xyz, 1.0 ), Bones[i.boneIds[b]] ).xyz;
    return Project( skinned, i.uv );
}

float4 PSMain( VS_OUT i ) : SV_TARGET
{
    Texture2D tex = ResourceDescriptorHeap[i.tex];
    float4 t = tex.Sample( smp, i.uv );
    clip( t.a - 0.5 );
    return t;
}
