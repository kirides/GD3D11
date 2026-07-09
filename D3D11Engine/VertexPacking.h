#pragma once

#include "VertexTypes.h"
#include <meshoptimizer/src/meshoptimizer.h>
#include <cmath>
#include <cstdint>

/** CPU-side encoding of the full-precision ExVertexStruct into the packed 36-byte
    ExVertexStructGPU that is uploaded to vertex buffers. See VertexTypes.h for the
    field layout and the matching input layout in D3D11VShader.cpp / Shaders/VertexPacking.h. */

namespace VertexPacking {

    /** Octahedral-encode a (near-)unit vector into two [-1,1] components.
        Standard "survey of efficient representations for independent unit vectors" mapping. */
    inline void EncodeOctNormal( const float3& n, float& outX, float& outY ) {
        float ax = std::fabs( n.x );
        float ay = std::fabs( n.y );
        float az = std::fabs( n.z );
        float invL1 = 1.0f / (ax + ay + az + 1e-8f);
        float px = n.x * invL1;
        float py = n.y * invL1;
        if ( n.z < 0.0f ) {
            // Fold the lower hemisphere over.
            float ox = (1.0f - std::fabs( py )) * (px >= 0.0f ? 1.0f : -1.0f);
            float oy = (1.0f - std::fabs( px )) * (py >= 0.0f ? 1.0f : -1.0f);
            px = ox;
            py = oy;
        }
        outX = px;
        outY = py;
    }

    /** Pack an xyz unit tangent + handedness sign into R10G10B10A2_UNORM.
        xyz mapped [-1,1]->[0,1] over 10 bits each; the 2-bit alpha stores the sign
        (3 => +1, 0 => -1) so the shader can recover the bitangent as cross(N,T)*sign. */
    inline uint32_t EncodeTangent( const float3& t, float sign ) {
        auto to10 = []( float v ) -> uint32_t {
            float u = v * 0.5f + 0.5f;               // [-1,1] -> [0,1]
            if ( u < 0.0f ) u = 0.0f;
            if ( u > 1.0f ) u = 1.0f;
            return static_cast<uint32_t>(u * 1023.0f + 0.5f) & 0x3FFu;
        };
        uint32_t x = to10( t.x );
        uint32_t y = to10( t.y );
        uint32_t z = to10( t.z );
        uint32_t w = (sign >= 0.0f) ? 3u : 0u;       // 2-bit handedness
        return (w << 30) | (z << 20) | (y << 10) | x;
    }

    inline ExVertexStructGPU Pack( const ExVertexStruct& v ) {
        ExVertexStructGPU out;
        out.Position = v.Position;

        float ox, oy;
        EncodeOctNormal( v.Normal, ox, oy );
        out.Normal[0] = static_cast<int16_t>(meshopt_quantizeSnorm( ox, 16 ));
        out.Normal[1] = static_cast<int16_t>(meshopt_quantizeSnorm( oy, 16 ));

        out.Tangent = EncodeTangent( float3( v.Tangent.x, v.Tangent.y, v.Tangent.z ),
            -v.Tangent.w ); // negate tangent.w to produce OpenGL (Y+) compatible tangents

        out.TexCoord = v.TexCoord;                   // UV0 kept full precision

        out.TexCoord2[0] = meshopt_quantizeHalf( v.TexCoord2.x );
        out.TexCoord2[1] = meshopt_quantizeHalf( v.TexCoord2.y );

        out.Color = v.Color;
        return out;
    }

    /** Pack a whole array in one call (upload boundary helper). */
    inline std::vector<ExVertexStructGPU> Pack( const ExVertexStruct* src, size_t count ) {
        std::vector<ExVertexStructGPU> out;
        out.reserve( count );
        for ( size_t i = 0; i < count; ++i ) {
            out.push_back( Pack( src[i] ) );
        }
        return out;
    }

} // namespace VertexPacking
