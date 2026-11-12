#pragma once
#include <cstdint>
#include <string_view>

/** Backend-neutral helpers for the dynamic weather model (GSky::UpdateWeather). */
namespace SkyWeather {
    constexpr int ThumbSize = 16;

    /** Pixel layout of the locked surface data as MyDirectDrawSurface7 stores it. */
    enum class PixelFormat { BGRA8, B5G6R5, B5G5R5A1, B4G4R4A4, DXT1, DXT3, DXT5 };

    /** Summary of one SKYDAY_ texture. Thumb is a 16x16 box average, premultiplied RGBA8. */
    struct SkyTexStats {
        float AlphaMean = 0.0f;
        float RgbMean[3] = {};      // alpha-weighted
        float LumMean = 0.0f;       // alpha-weighted
        float SatMean = 0.0f;       // alpha-weighted
        uint8_t Thumb[ThumbSize * ThumbSize * 4] = {};
    };

    /** True for names (no extension) of sky day textures, case-insensitive "SKYDAY_" prefix. */
    bool IsSkyDayTexture( std::string_view nameNoExt );

    /** Decodes a sampled grid of the top mip and stores the stats under the name. First call per name wins;
        thread-safe. hasAlpha false forces alpha 1 (opaque surfaces carry garbage in the alpha channel). */
    void RecordSkyTexture( std::string_view nameNoExt, const void* data, int width, int height, int pitchBytes,
        PixelFormat format, bool hasAlpha );

    /** Copies the stats recorded for the name (extension ignored, case-insensitive). False if none. */
    bool GetSkyTexStats( std::string_view name, SkyTexStats& out );

    /** Weather target in [0,1] for calendar day k: ~45% clear, ~35% broken, ~20% overcast. */
    float DaySeedTarget( int k );

    /** Cover/cloud-luminance/tint of Layer1 (alpha layer1Alpha) over Layer0 over clear blue, and the bias
        in [-0.3,0.6] derived from them. Tint is mean-normalized (1,1,1 = neutral). */
    struct SkyComposite {
        float Cover = 0.0f;
        float CloudLum = 0.0f;
        float Tint[3] = { 1.0f, 1.0f, 1.0f };
        float Bias = 0.0f;
    };
    SkyComposite CompositeSky( const SkyTexStats& layer0, const SkyTexStats& layer1, float layer1Alpha );
}
