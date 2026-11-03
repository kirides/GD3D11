// Static-light far field (StaticLightFarField.h): the static point lights' baked, occluded irradiance beyond
// FarFieldNear. Included by PBRLighting.hlsl, so ShadowCB (FarFieldNear/InvFade/Enabled) and FogCB are declared.
#ifndef D3D12_FARFIELD_HLSL
#define D3D12_FARFIELD_HLSL

static const uint kFarFieldVertexSlot = 65532;   // D3D12GraphicsEngine::kFarFieldVertexSlot
static const float kVobFarFieldMax = 4.0;        // StaticLightFarField.cpp kVobFarMax

float3 DecodeRGB9E5( uint v )
{
    const float scale = exp2( (float)( v >> 27 ) - 24.0 );
    return float3( v & 511u, ( v >> 9 ) & 511u, ( v >> 18 ) & 511u ) * scale;
}

// VS: one wrapped world vertex's baked irradiance, w = 1 while a bake is bound (the placeholder has one element)
float4 LoadWorldFarField( uint vertexId )
{
    StructuredBuffer<uint> buf = GD_HEAP( kFarFieldVertexSlot );
    uint count, stride;
    buf.GetDimensions( count, stride );
    if ( count <= 1u || vertexId >= count ) return 0.0;
    return float4( DecodeRGB9E5( buf[vertexId] ), 1.0 );
}

// The instance color's spare bytes (StaticLightFarField PackVob): .b = red, .r = blue, .a = 0b10 tag + 6-bit green
float4 DecodeVobFarField( float4 c )
{
    const uint a = (uint)round( c.a * 255.0 );
    if ( ( a & 0xC0u ) != 0x80u ) return 0.0;
    const float3 s = float3( c.b, ( a & 63u ) / 63.0, c.r );
    return float4( s * s * kVobFarFieldMax, 1.0 );
}

// 0 near the camera (real lights), 1 where only the bake lights the static point lights
float FarFieldWeight( float3 wpos, float valid )
{
    if ( FarFieldEnabled == 0u || valid <= 0.0 ) return 0.0;
    return saturate( ( distance( wpos, CamPosWS ) - FarFieldNear ) * FarFieldInvFade );
}

// The bake's diffuse term, in PBR_DirectLighting's units (kD ~0.96, Lambert)
float3 FarFieldDiffuse( float3 albedo, float metallic, float3 irradiance )
{
    return albedo * ( 1.0 - saturate( metallic ) ) * irradiance * ( 0.96 / PBR_PI );
}

#endif // D3D12_FARFIELD_HLSL
