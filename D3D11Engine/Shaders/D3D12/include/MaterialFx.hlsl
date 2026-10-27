// ZenGin's per-material stages (MaterialFx.h): texAniMap scroll, vertex waves, env overlay and detail texture. A
// diffuse index's top 16 bits name an entry of the table behind heap slot kMaterialFxTableSlot; 0 = none.
#ifndef D3D12_MATERIALFX_HLSL
#define D3D12_MATERIALFX_HLSL

#include "../../include/WaterVertexWaves.hlsl"

static const uint kMaterialFxTableSlot = 65535;
static const uint kMaterialFxSlotShift = 16;
static const uint kDiffuseSlotMask = 0x0000FFFFu;

struct MaterialFxEntry
{
    float2 Scroll;
    float  EnvStrength;   // < 0: additive
    float  DetailScale;   // 0 = no detail texture
    uint   DetailSlot;
    uint   EnvCubeSlot;   // entry 0: the reflection cube, 0xFFFFFFFF = env mapping off
    float  EnvGlobal;     // entry 0: sky-fog luma * user strength
    float  WaveTimeMs;    // entry 0: this frame's wave clock
    float4 Wave;          // { amplitude (0 = none), grid, time scale, wall }; entry 0: .x = last frame's clock
};

/** The SRV heap slot of a packed diffuse index. */
uint DiffuseSlot( uint packedDiffuse )
{
    return packedDiffuse & kDiffuseSlotMask;
}

/** The material's entry; all zero (no stage) when it has none. */
MaterialFxEntry LoadMaterialFx( uint packedDiffuse )
{
    MaterialFxEntry e = (MaterialFxEntry)0;
    const uint slot = packedDiffuse >> kMaterialFxSlotShift;
    [branch]
    if ( slot != 0 )
    {
        StructuredBuffer<MaterialFxEntry> table = GD_HEAP( kMaterialFxTableSlot );
        e = table[slot];
    }
    return e;
}

/** World-space offset of a waving material's vertex (zCPolygon::ApplyMorphing); `previous` = last frame's. */
float3 MaterialWaveOffset( float3 positionWorld, uint packedDiffuse, bool previous )
{
    float3 offset = float3( 0.0, 0.0, 0.0 );
    const uint slot = packedDiffuse >> kMaterialFxSlotShift;
    [branch]
    if ( slot != 0 )
    {
        StructuredBuffer<MaterialFxEntry> table = GD_HEAP( kMaterialFxTableSlot );
        const float4 wave = table[slot].Wave;
        [branch]
        if ( wave.x > 0.0 )
        {
            const MaterialFxEntry frame = table[0];
            offset = ZenGinMaterialWaveOffset( positionWorld, wave, previous ? frame.Wave.x : frame.WaveTimeMs );
        }
    }
    return offset;
}

/** `uv` shifted by the material's scroll offset this frame. */
float2 TexAniUv( float2 uv, uint packedDiffuse )
{
    return uv + LoadMaterialFx( packedDiffuse ).Scroll;
}

/** ZenGin's MUL2 detail stage, folded into the gamma-space albedo (exact for diffuse light). */
float3 ApplyDetailTexture( float3 albedoGamma, float2 uv, MaterialFxEntry fx, SamplerState s )
{
    [branch]
    if ( fx.DetailScale > 0.0 )
    {
        Texture2D detail = GD_HEAP( fx.DetailSlot );
        albedoGamma = saturate( albedoGamma * 2.0 * detail.Sample( s, uv * fx.DetailScale ).rgb );
    }
    return albedoGamma;
}

/** ZenGin's env overlay stage on the lit linear color: blended (additive for water) with alpha strength * sky-fog
    luma * the object's light, like zCProgMeshProto. Samples the reflection cube by the world reflection vector. */
float3 ApplyEnvMap( float3 rgb, float3 N, float3 V, MaterialFxEntry fx, float objectLight, SamplerState s )
{
    [branch]
    if ( fx.EnvStrength != 0.0 )
    {
        StructuredBuffer<MaterialFxEntry> table = GD_HEAP( kMaterialFxTableSlot );
        const MaterialFxEntry frame = table[0];
        [branch]
        if ( frame.EnvCubeSlot != 0xFFFFFFFFu )
        {
            TextureCube cube = GD_HEAP( frame.EnvCubeSlot );
            const float3 c = cube.Sample( s, reflect( -V, N ) ).rgb;
            const float3 env = select( c <= 0.04045, c / 12.92, pow( ( c + 0.055 ) / 1.055, 2.4 ) );
            const float alpha = saturate( abs( fx.EnvStrength ) * frame.EnvGlobal * objectLight );
            rgb = fx.EnvStrength < 0.0 ? rgb + env * alpha : lerp( rgb, env, alpha );
        }
    }
    return rgb;
}

#endif // D3D12_MATERIALFX_HLSL
