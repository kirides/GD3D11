//--------------------------------------------------------------------------------------
// Inventory item previews: one instance per slot, object -> slot clip space, remapped into the slot's rect
//--------------------------------------------------------------------------------------

struct ItemProjection
{
	float4 Position;
	float4 ClipDistance;	// the slot's viewport edges, replacing a per-slot viewport
};

ItemProjection ProjectItem( float3 position, float4 row0, float4 row1, float4 row2, float4 row3, float4 remap )
{
	float4 p = float4( position, 1.0f );
	float4 slot = float4( dot( row0, p ), dot( row1, p ), dot( row2, p ), dot( row3, p ) );

	ItemProjection o;
	o.ClipDistance = float4( slot.w + slot.x, slot.w - slot.x, slot.w + slot.y, slot.w - slot.y );
	o.Position = float4( slot.x * remap.x + slot.w * remap.z, slot.y * remap.y + slot.w * remap.w, slot.z, slot.w );
	return o;
}
