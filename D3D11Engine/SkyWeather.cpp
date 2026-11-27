#include "SkyWeather.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <string>
#include <unordered_map>

namespace SkyWeather {
    namespace {
        std::mutex s_Mutex;
        std::unordered_map<std::string, SkyTexStats> s_Stats;

        std::string Key( std::string_view name ) {
            auto dot = name.find_last_of( '.' );
            if ( dot != std::string_view::npos )
                name = name.substr( 0, dot );
            std::string key( name );
            for ( char& c : key )
                c = static_cast<char>( toupper( static_cast<unsigned char>( c ) ) );
            return key;
        }

        struct Rgba { float r, g, b, a; };

        void Expand565( uint16_t v, float& r, float& g, float& b ) {
            r = ((v >> 11) & 31) / 31.0f;
            g = ((v >> 5) & 63) / 63.0f;
            b = (v & 31) / 31.0f;
        }

        Rgba DecodeBlockPixel( const uint8_t* block, PixelFormat fmt, int px, int py ) {
            const uint8_t* color = (fmt == PixelFormat::DXT1) ? block : block + 8;
            uint16_t c0 = static_cast<uint16_t>( color[0] | (color[1] << 8) );
            uint16_t c1 = static_cast<uint16_t>( color[2] | (color[3] << 8) );
            float p[4][3];
            Expand565( c0, p[0][0], p[0][1], p[0][2] );
            Expand565( c1, p[1][0], p[1][1], p[1][2] );
            bool fourColor = fmt != PixelFormat::DXT1 || c0 > c1;
            for ( int i = 0; i < 3; i++ ) {
                if ( fourColor ) {
                    p[2][i] = (2 * p[0][i] + p[1][i]) / 3.0f;
                    p[3][i] = (p[0][i] + 2 * p[1][i]) / 3.0f;
                } else {
                    p[2][i] = (p[0][i] + p[1][i]) * 0.5f;
                    p[3][i] = 0.0f;
                }
            }
            uint32_t bits = color[4] | (color[5] << 8) | (color[6] << 16) | (uint32_t( color[7] ) << 24);
            int idx = (bits >> (2 * (py * 4 + px))) & 3;
            Rgba out = { p[idx][0], p[idx][1], p[idx][2], 1.0f };

            if ( fmt == PixelFormat::DXT1 ) {
                if ( !fourColor && idx == 3 ) out.a = 0.0f;
            } else if ( fmt == PixelFormat::DXT3 ) {
                int n = py * 4 + px;
                int a4 = (block[n >> 1] >> ((n & 1) * 4)) & 15;
                out.a = a4 / 15.0f;
            } else {
                uint8_t a0 = block[0], a1 = block[1];
                uint64_t abits = 0;
                for ( int i = 0; i < 6; i++ ) abits |= uint64_t( block[2 + i] ) << (8 * i);
                int ai = static_cast<int>( (abits >> (3 * (py * 4 + px))) & 7 );
                float a;
                if ( ai == 0 ) a = a0;
                else if ( ai == 1 ) a = a1;
                else if ( a0 > a1 ) a = ((8 - ai) * a0 + (ai - 1) * a1) / 7.0f;
                else if ( ai == 6 ) a = 0.0f;
                else if ( ai == 7 ) a = 255.0f;
                else a = ((6 - ai) * a0 + (ai - 1) * a1) / 5.0f;
                out.a = a / 255.0f;
            }
            return out;
        }

        Rgba DecodePixel( const uint8_t* data, int pitch, PixelFormat fmt, bool hasAlpha, int x, int y ) {
            Rgba o = { 0, 0, 0, 1 };
            switch ( fmt ) {
            case PixelFormat::BGRA8: {
                const uint8_t* p = data + size_t( y ) * pitch + size_t( x ) * 4;
                o = { p[2] / 255.0f, p[1] / 255.0f, p[0] / 255.0f, hasAlpha ? p[3] / 255.0f : 1.0f };
                break;
            }
            case PixelFormat::B5G6R5: {
                const uint16_t v = *reinterpret_cast<const uint16_t*>( data + size_t( y ) * pitch + size_t( x ) * 2 );
                Expand565( v, o.r, o.g, o.b );
                break;
            }
            case PixelFormat::B5G5R5A1: {
                const uint16_t v = *reinterpret_cast<const uint16_t*>( data + size_t( y ) * pitch + size_t( x ) * 2 );
                o = { ((v >> 10) & 31) / 31.0f, ((v >> 5) & 31) / 31.0f, (v & 31) / 31.0f, hasAlpha ? float( v >> 15 ) : 1.0f };
                break;
            }
            case PixelFormat::B4G4R4A4: {
                const uint16_t v = *reinterpret_cast<const uint16_t*>( data + size_t( y ) * pitch + size_t( x ) * 2 );
                o = { ((v >> 8) & 15) / 15.0f, ((v >> 4) & 15) / 15.0f, (v & 15) / 15.0f, hasAlpha ? (v >> 12) / 15.0f : 1.0f };
                break;
            }
            default: {
                const int blockBytes = fmt == PixelFormat::DXT1 ? 8 : 16;
                const int blocksPerRow = (pitch > 0) ? pitch / blockBytes : 0;
                const uint8_t* block = data + (size_t( y >> 2 ) * blocksPerRow + (x >> 2)) * blockBytes;
                o = DecodeBlockPixel( block, fmt, x & 3, y & 3 );
                if ( !hasAlpha ) o.a = 1.0f;
                break;
            }
            }
            return o;
        }

        float Luma( float r, float g, float b ) { return 0.2126f * r + 0.7152f * g + 0.0722f * b; }

        uint32_t Hash32( uint32_t x ) {
            x ^= x >> 16; x *= 0x7feb352dU;
            x ^= x >> 15; x *= 0x846ca68bU;
            x ^= x >> 16;
            return x;
        }
    }

    bool IsSkyDayTexture( std::string_view n ) {
        constexpr std::string_view prefix = "SKYDAY_";
        if ( n.size() < prefix.size() ) return false;
        for ( size_t i = 0; i < prefix.size(); i++ )
            if ( toupper( static_cast<unsigned char>( n[i] ) ) != prefix[i] ) return false;
        return true;
    }

    void RecordSkyTexture( std::string_view nameNoExt, const void* data, int width, int height, int pitchBytes,
        PixelFormat format, bool hasAlpha ) {
        if ( !data || width <= 0 || height <= 0 ) return;
        const std::string key = Key( nameNoExt );
        {
            std::scoped_lock lock( s_Mutex );
            if ( s_Stats.contains( key ) ) return;
        }

        // 4x4 samples per thumb cell: at most 64x64 decoded pixels whatever the texture size.
        constexpr int N = ThumbSize, S = 4;
        const auto* bytes = static_cast<const uint8_t*>( data );
        SkyTexStats st;
        float sumA = 0, sumRgb[3] = {}, sumSat = 0;
        for ( int ty = 0; ty < N; ty++ ) {
            for ( int tx = 0; tx < N; tx++ ) {
                float pr = 0, pg = 0, pb = 0, a = 0, sat = 0;
                for ( int sy = 0; sy < S; sy++ ) {
                    for ( int sx = 0; sx < S; sx++ ) {
                        int x = std::min( width - 1, int( (tx + (sx + 0.5f) / S) * width / N ) );
                        int y = std::min( height - 1, int( (ty + (sy + 0.5f) / S) * height / N ) );
                        Rgba c = DecodePixel( bytes, pitchBytes, format, hasAlpha, x, y );
                        pr += c.r * c.a; pg += c.g * c.a; pb += c.b * c.a; a += c.a;
                        float mx = std::max( { c.r, c.g, c.b } ), mn = std::min( { c.r, c.g, c.b } );
                        sat += (mx > 1e-4f ? (mx - mn) / mx : 0.0f) * c.a;
                    }
                }
                constexpr float inv = 1.0f / (S * S);
                uint8_t* t = &st.Thumb[(ty * N + tx) * 4];
                t[0] = uint8_t( std::clamp( pr * inv, 0.0f, 1.0f ) * 255.0f + 0.5f );
                t[1] = uint8_t( std::clamp( pg * inv, 0.0f, 1.0f ) * 255.0f + 0.5f );
                t[2] = uint8_t( std::clamp( pb * inv, 0.0f, 1.0f ) * 255.0f + 0.5f );
                t[3] = uint8_t( std::clamp( a * inv, 0.0f, 1.0f ) * 255.0f + 0.5f );
                sumA += a; sumSat += sat;
                sumRgb[0] += pr; sumRgb[1] += pg; sumRgb[2] += pb;
            }
        }
        st.AlphaMean = sumA / float( N * N * S * S );
        if ( sumA > 1e-3f ) {
            for ( int i = 0; i < 3; i++ ) st.RgbMean[i] = sumRgb[i] / sumA;
            st.LumMean = Luma( st.RgbMean[0], st.RgbMean[1], st.RgbMean[2] );
            st.SatMean = sumSat / sumA;
        }

        std::scoped_lock lock( s_Mutex );
        s_Stats.emplace( key, st );
    }

    bool GetSkyTexStats( std::string_view name, SkyTexStats& out ) {
        const std::string key = Key( name );
        std::scoped_lock lock( s_Mutex );
        auto it = s_Stats.find( key );
        if ( it == s_Stats.end() ) return false;
        out = it->second;
        return true;
    }

    float DaySeedTarget( int k ) {
        uint32_t h = Hash32( static_cast<uint32_t>( k ) ^ 0x9E3779B9U );
        float u1 = (h & 0xFFFF) / 65536.0f;
        float u2 = (Hash32( h ) & 0xFFFF) / 65536.0f;
        if ( u1 < 0.45f ) return 0.00f + 0.15f * u2;
        if ( u1 < 0.80f ) return 0.30f + 0.25f * u2;
        return 0.70f + 0.25f * u2;
    }

    SkyComposite CompositeSky( const SkyTexStats& l0, const SkyTexStats& l1, float layer1Alpha ) {
        constexpr float blue[3] = { 0.45f, 0.60f, 0.85f };
        constexpr int N = ThumbSize * ThumbSize;
        float clearSum = 0, wSum = 0, lumSum = 0, colSum[3] = {};
        for ( int i = 0; i < N; i++ ) {
            const uint8_t* b = &l0.Thumb[i * 4];
            const uint8_t* o = &l1.Thumb[i * 4];
            float ab = b[3] / 255.0f, ao = o[3] / 255.0f * layer1Alpha;
            float sky[3];
            for ( int c = 0; c < 3; c++ ) {
                sky[c] = blue[c] * (1 - ab) + b[c] / 255.0f;       // thumbs are premultiplied
                sky[c] = sky[c] * (1 - ao) + o[c] / 255.0f * layer1Alpha;
            }
            float clear = std::clamp( (sky[2] - 0.5f * (sky[0] + sky[1])) / 0.3f, 0.0f, 1.0f );
            float w = 1 - clear;
            clearSum += clear; wSum += w;
            lumSum += Luma( sky[0], sky[1], sky[2] ) * w;
            for ( int c = 0; c < 3; c++ ) colSum[c] += sky[c] * w;
        }
        SkyComposite r;
        r.Cover = 1 - clearSum / N;
        if ( wSum > 1e-3f ) {
            r.CloudLum = lumSum / wSum;
            float mean = (colSum[0] + colSum[1] + colSum[2]) / (3 * wSum);
            if ( mean > 1e-3f )
                for ( int c = 0; c < 3; c++ ) r.Tint[c] = colSum[c] / wSum / mean;
        }
        float tintRange = std::max( { r.Tint[0], r.Tint[1], r.Tint[2] } ) - std::min( { r.Tint[0], r.Tint[1], r.Tint[2] } );
        float dark = std::clamp( 1 - r.CloudLum / 0.65f, 0.0f, 1.0f );
        float grey = std::clamp( 1 - tintRange / 0.25f, 0.0f, 1.0f );
        r.Bias = std::clamp( r.Cover * (0.10f + 0.60f * dark + 0.25f * grey) - 0.15f, -0.3f, 0.6f );
        return r;
    }
}
