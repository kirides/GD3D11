//--------------------------------------------------------------------------------------
// Decoders for the packed 36-byte Ex vertex (see C++ VertexPacking.h / VertexTypes.h).
// The input assembler already converts the stored formats to floats:
//   NORMAL   : float2  (R16G16_SNORM,      octahedral-encoded unit normal, [-1,1])
//   TANGENT  : float4  (R10G10B10A2_UNORM, xyz in [0,1], w = 2-bit handedness in {0,1/3,2/3,1})
//--------------------------------------------------------------------------------------
#ifndef VERTEX_PACKING_H
#define VERTEX_PACKING_H

/** Decode an octahedral-encoded unit normal (matches VertexPacking::EncodeOctNormal). */
float3 DecodeOctNormal( float2 e )
{
    float3 n = float3( e.xy, 1.0 - abs( e.x ) - abs( e.y ) );
    float t = saturate( -n.z );
    n.xy += (n.xy >= 0.0 ? -t : t);
    return normalize( n );
}

/** Decode a packed tangent. Returns xyz = unit tangent, w = bitangent handedness sign (+1/-1).
    The bitangent is reconstructed in the shader as cross(N, T) * w. */
float4 DecodeTangent( float4 packed )
{
    float3 t = packed.xyz * 2.0 - 1.0;   // [0,1] -> [-1,1]
    float sign = packed.w > 0.5 ? 1.0 : -1.0;
    return float4( normalize( t ), sign );
}

#endif // VERTEX_PACKING_H
