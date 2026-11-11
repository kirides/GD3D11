// Startup self-test of the SV_VertexID / SV_InstanceID base semantics (VERTEX_PULLING_PLAN.md section 4).
// The viewport picks the pixel; the triangle covers all of it whatever base vid carries (corner = vid % 3).

struct VSOut {
    float4 pos : SV_Position;
    nointerpolation uint2 ids : IDS;
};

VSOut VSMain( uint vid : SV_VertexID, uint iid : SV_InstanceID ) {
    const uint corner = vid % 3;
    VSOut o;
    o.pos = float4( corner == 1 ? 3.0 : -1.0, corner == 2 ? 3.0 : -1.0, 0.5, 1.0 );
    o.ids = uint2( vid, iid );
    return o;
}

// +1 so an untouched (cleared) pixel reads as "not drawn".
uint2 PSMain( VSOut i ) : SV_Target0 {
    return i.ids + 1;
}
