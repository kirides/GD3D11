// Material texAniMap UV scroll (TexAniScroll.h). A diffuse index's top byte names a slot in the per-frame
// offset table behind heap slot kTexAniTableSlot (D3D12GraphicsEngine.h); 0 = no scroll.
#ifndef D3D12_TEXANISCROLL_HLSL
#define D3D12_TEXANISCROLL_HLSL

static const uint kTexAniTableSlot = 65535;
static const uint kTexAniSlotShift = 24;
static const uint kDiffuseSlotMask = 0x00FFFFFFu;

/** The SRV heap slot of a packed diffuse index. */
uint DiffuseSlot( uint packedDiffuse )
{
    return packedDiffuse & kDiffuseSlotMask;
}

/** `uv` shifted by the material's scroll offset this frame. */
float2 TexAniUv( float2 uv, uint packedDiffuse )
{
    const uint slot = packedDiffuse >> kTexAniSlotShift;
    [branch]
    if ( slot != 0 )
    {
        StructuredBuffer<float4> table = ResourceDescriptorHeap[kTexAniTableSlot];
        uv += table[slot].xy;
    }
    return uv;
}

#endif // D3D12_TEXANISCROLL_HLSL
