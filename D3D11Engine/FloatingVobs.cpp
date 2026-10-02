#include "pch.h"
#include "FloatingVobs.h"
#include "GothicAPI.h"
#include "WaterProfile.h"
#include "WorldConverter.h"
#include "WorldMeshSection.h"
#include "zCMaterial.h"
#include "zCVisual.h"
#include "zCVob.h"

namespace {
    // ZenGin's FFT wave spectrum, the same table WaterVertexWaves.hlsl sums; the generated HLSL is plain
    // float4( ... ) initializers, which C++ reads with Types.h's float4
#pragma warning( push )
#pragma warning( disable : 4244 4305 )
#include "Shaders/include/ZenGinWaveSpectrum.hlsl"
#pragma warning( pop )

    constexpr float kRestAbove = 25.0f;           // a floating plant's lowest point sits at most this far above the water
    constexpr float kRestBelow = 40.0f;           // ... or this far below it; anything deeper is rooted
    constexpr float kFlatWaterNormalY = 0.77f;    // steeper water is a waterfall
    constexpr float kTwoPi = 6.28318531f;

    /** The water's wave parameters, quantized exactly like WorldConverter packs them into its vertex color. */
    bool ReadWaves( zCMaterial* mat, VobWaterBob& bob ) {
        const zTMat_WaveMode mode = mat->GetWaveMode();
        if ( mode == zTMode_NONE || mode >= zTMode_AMBIENT_WALL || mat->HasAlphaTest() ) return false;
        bob.Amplitude = static_cast<float>( static_cast<BYTE>( mat->GetWaveMaxAmplitude() / 5.0f ) ) * 5.0f;
        bob.Speed = static_cast<float>( static_cast<BYTE>( mat->GetWaveSpeed() * 10.0f ) ) / 10.0f;
        bob.GridSize = static_cast<float>( static_cast<UINT>( std::clamp( mat->GetWaveGridSize() + 0.5f, 1.0f, 32767.0f ) ) );
        return bob.Amplitude > 0.0f;
    }

    // --- C++ mirror of WaterVertexWaves.hlsl, in float like the shader -------------------------------------
    float LengthApprox( float x, float y, float z ) {
        x = std::abs( x );
        y = std::abs( y );
        z = std::abs( z );
        const float m = std::max( x, std::max( y, z ) );
        const float t = x + y + z - m;
        return m - m * ( 1.0f / 16.0f ) + t * ( 1.0f / 4.0f ) + t * ( 1.0f / 8.0f );
    }

    float ArcTan2Approx( float y, float x ) {
        const float c1 = 0.785398163f;
        const float absY = std::abs( y ) + 1e-10f;
        const float angle = x >= 0.0f ? c1 - c1 * ( ( x - absY ) / ( x + absY ) ) : 3.0f * c1 - c1 * ( ( x + absY ) / ( absY - x ) );
        return y < 0.0f ? -angle : angle;
    }

    float ZenGinWave2D( float px, float pz, float gridSize, float timeSec ) {
        const int cellX = static_cast<int>( px * ( 1.0f / gridSize ) + 0.5f );   // truncates toward zero like the shader
        const int cellZ = static_cast<int>( pz * ( 1.0f / gridSize ) + 0.5f );
        const uint32_t row = static_cast<uint32_t>( cellX ) & 31u;
        const uint32_t col = static_cast<uint32_t>( cellZ ) & 31u;
        const float kz = kTwoPi / 32.0f * ( static_cast<float>( row ) - 16.0f );

        float hx = 0.0f;
        float hy = 0.0f;
        for ( uint32_t i = 0; i < 8; ++i ) {
            const float4& a = ZenGinWaveSpectrum[row * 8 + i];
            const float amplitudes[4] = { a.x, a.y, a.z, a.w };
            for ( uint32_t j = 0; j < 4; ++j ) {
                const uint32_t x = i * 4 + j;
                const float omega = std::sqrt( 9.81f * LengthApprox( kTwoPi / 32.0f * ( static_cast<float>( x ) - 16.0f ), 0.01f, kz ) );
                const float phase = omega * timeSec * ( 1.0f / kTwoPi );
                const float turns = ( phase - std::floor( phase ) ) + static_cast<float>( ( x * col ) & 31u ) * ( 1.0f / 32.0f );
                hx += amplitudes[j] * std::cos( turns * kTwoPi );
                hy -= amplitudes[j] * std::sin( turns * kTwoPi );
            }
        }
        hx *= 1.0f / 32.0f;
        hy *= 1.0f / 32.0f;
        return std::clamp( std::sin( ArcTan2Approx( hy, hx ) + std::sqrt( hx * hx + hy * hy ) * 1e7f ), -1.0f, 1.0f );
    }

    XMFLOAT3 GerstnerOffset( const XMFLOAT3& p, float amplitude, float waveSpeed, float totalTimeMs ) {
        constexpr int iterations = 10;
        constexpr float dragMult = 0.48f;
        float wsum = 0.0f;
        float wx = 1.0f;
        for ( int j = 0; j < iterations; ++j ) {
            wsum += wx;
            wx *= 0.8f;
        }

        XMFLOAT3 offset( 0.0f, 0.0f, 0.0f );
        float posX = p.x;
        float posZ = p.z;
        float freq = 0.6f * 0.005f;
        float speed = 2.0f;
        float iter = 0.0f;
        float weight = 1.0f;
        for ( int i = 0; i < iterations; ++i ) {
            const float dirX = std::cos( iter );
            const float dirZ = std::sin( iter );
            const float waveAmplitude = weight * amplitude / wsum;
            const float x = ( dirX * posX + dirZ * posZ ) * freq + totalTimeMs * 0.001f * ( speed * waveSpeed );
            const float res = waveAmplitude * std::cos( x );
            offset.x += dirX * res;
            offset.y += waveAmplitude * std::sin( x );
            offset.z += dirZ * res;

            // Drag the sample point along each wave so the next one rides on it
            posX += res * weight * dirX * dragMult;
            posZ += res * weight * dirZ * dragMult;

            iter += 12.0f;
            weight *= 0.8f;
            freq *= 1.18f;
            speed *= 1.07f;
        }
        return offset;
    }

    XMFLOAT3 WaveOffset( const XMFLOAT3& p, const VobWaterBob& bob, float totalTimeMs, int waveMode ) {
        if ( waveMode == GothicRendererSettings::WATER_WAVES_ORIGINAL ) {
            const float timeSec = totalTimeMs * 0.001f * ( bob.Speed * 0.5f );
            return XMFLOAT3( 0.0f, ZenGinWave2D( p.x, p.z, bob.GridSize, timeSec ) * bob.Amplitude, 0.0f );
        }
        return GerstnerOffset( p, bob.Amplitude, bob.Speed, totalTimeMs );
    }
}

bool AttachWaterBob( VobInfo* vi, const std::map<int, std::map<int, WorldMeshSectionInfo>>& sections, std::string_view identifiers ) {
    if ( !vi || !vi->Vob || identifiers.empty() ) return false;
    zCVob* vob = vi->Vob;
    zCVisual* visual = vob->GetVisual();
    const std::string_view visualName = visual ? visual->GetObjectNameView() : std::string_view{};
    if ( !MatchesIdentifierList( visualName, identifiers ) && !MatchesIdentifierList( vob->GetObjectName().ToView(), identifiers ) ) {
        return false;
    }

    // The flat, wave-animated water triangle right under the plant whose surface its lowest point rests on
    const XMFLOAT3 pos = vob->GetPositionWorld();
    const float bottom = vob->GetBBox().Min.y;
    const INT2 home = WorldConverter::GetSectionOfPos( pos );
    VobWaterBob best = {};
    float bestGap = FLT_MAX;
    for ( int sx = home.x - 1; sx <= home.x + 1; ++sx ) {
        const auto row = sections.find( sx );
        if ( row == sections.end() ) continue;
        for ( int sy = home.y - 1; sy <= home.y + 1; ++sy ) {
            const auto section = row->second.find( sy );
            if ( section == row->second.end() ) continue;
            for ( const auto& [key, mesh] : section->second.WorldMeshes ) {
                if ( !mesh || !key.Info || !key.Info->IsWater() || !key.Material ) continue;
                VobWaterBob waves = {};
                if ( !ReadWaves( key.Material, waves ) ) continue;
                const std::vector<WorldVertexCPU>* vertices = mesh->GetCpuVertices();
                if ( !vertices ) continue;

                for ( size_t i = 0; i + 2 < mesh->Indices.size(); i += 3 ) {
                    const VERTEX_INDEX i0 = mesh->Indices[i], i1 = mesh->Indices[i + 1], i2 = mesh->Indices[i + 2];
                    if ( i0 >= vertices->size() || i1 >= vertices->size() || i2 >= vertices->size() ) continue;
                    const XMFLOAT3& a = ( *vertices )[i0].Position;
                    const XMFLOAT3& b = ( *vertices )[i1].Position;
                    const XMFLOAT3& c = ( *vertices )[i2].Position;

                    // xz barycentrics; the projected area against the 3D one rejects waterfalls
                    const float area = ( b.x - a.x ) * ( c.z - a.z ) - ( b.z - a.z ) * ( c.x - a.x );
                    const XMVECTOR n = XMVector3Cross( XMVectorSubtract( XMLoadFloat3( &b ), XMLoadFloat3( &a ) ),
                                                       XMVectorSubtract( XMLoadFloat3( &c ), XMLoadFloat3( &a ) ) );
                    if ( std::abs( area ) < kFlatWaterNormalY * XMVectorGetX( XMVector3Length( n ) ) || area == 0.0f ) continue;
                    const float w0 = ( ( c.x - b.x ) * ( pos.z - b.z ) - ( c.z - b.z ) * ( pos.x - b.x ) ) / area;
                    const float w1 = ( ( a.x - c.x ) * ( pos.z - c.z ) - ( a.z - c.z ) * ( pos.x - c.x ) ) / area;
                    const float w2 = 1.0f - w0 - w1;
                    if ( w0 < -1e-4f || w1 < -1e-4f || w2 < -1e-4f ) continue;

                    const float gap = bottom - ( w0 * a.y + w1 * b.y + w2 * c.y );
                    if ( gap > kRestAbove || gap < -kRestBelow || std::abs( gap ) >= bestGap ) continue;
                    bestGap = std::abs( gap );
                    best = waves;
                    best.Corners[0] = a;
                    best.Corners[1] = b;
                    best.Corners[2] = c;
                    best.Weights = XMFLOAT3( w0, w1, w2 );
                }
            }
        }
    }
    if ( bestGap == FLT_MAX ) return false;

    vi->WaterBob = std::make_unique<VobWaterBob>( best );
    return true;
}

void UpdateWaterBob( VobWaterBob& bob, float totalTimeMs, int waveMode ) {
    XMFLOAT3 offset( 0.0f, 0.0f, 0.0f );
    if ( waveMode != GothicRendererSettings::WATER_WAVES_OFF ) {
        const float weights[3] = { bob.Weights.x, bob.Weights.y, bob.Weights.z };
        for ( int i = 0; i < 3; ++i ) {
            const XMFLOAT3 corner = WaveOffset( bob.Corners[i], bob, totalTimeMs, waveMode );
            offset.x += weights[i] * corner.x;
            offset.y += weights[i] * corner.y;
            offset.z += weights[i] * corner.z;
        }
    }
    bob.PrevOffset = bob.HasOffset ? bob.Offset : offset;
    bob.Offset = offset;
    bob.HasOffset = true;
}
