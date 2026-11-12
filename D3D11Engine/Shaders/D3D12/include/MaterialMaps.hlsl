#ifndef D3D12_MATERIALMAPS_HLSL
#define D3D12_MATERIALMAPS_HLSL
// Per-material extra maps: StructuredBuffer<uint2> behind kMaterialMapsSlot, indexed by the diffuse SRV slot.
// .x = height-map SRV slot (0xFFFFFFFF = none), .y reserved. Needs MaterialFx.hlsl (DiffuseSlot) and GD_HEAP.

static const uint kMaterialMapsSlot = 65531;   // D3D12GraphicsEngine::kMaterialMapsSlot
static const uint kNoMaterialMap = 0xFFFFFFFFu;

// The placeholder has one element, so out-of-range diffuse slots read as none
uint MaterialHeightSlot( uint packedDiffuse )
{
    StructuredBuffer<uint2> buf = GD_HEAP( kMaterialMapsSlot );
    uint count, stride;
    buf.GetDimensions( count, stride );
    const uint d = DiffuseSlot( packedDiffuse );
    return d < count ? buf[d].x : kNoMaterialMap;
}

#endif
