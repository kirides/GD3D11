#pragma once

// FFT ocean spectrum (Tessendorf; JONSWAP + Hasselmann spreading after Horvath 2015) and cascade layout.
// CPU side of OceanFFT.hlsl: keep both in step. SI units (metres, seconds); Gothic's world unit is the centimetre.

#include <array>
#include <cstdint>

namespace OceanFFT {

inline constexpr double Gravity = 9.81;        // m / s^2
inline constexpr float UnitsPerMetre = 100.0f; // Gothic world units are centimetres
inline constexpr float RepeatTime = 256.0f;    // s: the dispersion is quantised so the animation loops
inline constexpr int MaxCascades = 4;

struct SeaState {
    float windSpeed = 8.0f;      // U10, m / s
    float fetch = 60000.0f;      // m
    float windAngle = 0.0f;      // rad, direction the wind blows towards (world xz: x = cos, z = sin)
    float spreadXi = 0.2f;       // Horvath's swell parameter of the wind sea
    float swellHeight = 1.0f;    // significant wave height of the swell, m
    float swellPeriod = 12.0f;   // s
    float swellAngle = 0.4f;     // rad
    float swellSpread = 60.0f;   // Hasselmann exponent (large = narrow)
    float choppiness = 0.9f;     // horizontal displacement scale (lambda)
    float scale = 1.0f;          // amplitude multiplier
};

// 0 calm, 1 moderate, 2 rough, 3 storm (in-between values interpolate); windAngle and scale are the caller's
SeaState SeaStateForLevel( float level );

// Everything the spectrum shader needs for a sea state
struct SpectrumParams {
    float windAlpha = 0.0f, windOmegaP = 0.0f, windGamma = 3.3f, windSpeed = 0.0f;
    float windAngle = 0.0f, spreadXi = 0.0f;
    float swellAlpha = 0.0f, swellOmegaP = 0.0f, swellAngle = 0.0f, swellSpread = 0.0f;
    float shortWaveCut = 0.01f;  // m: exp(-(k l)^2) damps the smallest ripples
};
SpectrumParams MakeSpectrumParams( const SeaState& s );

// Directional spectrum S(k) [m^4] at wave vector (kx, kz) [rad / m]
double SpectrumK( double kx, double kz, const SpectrumParams& p );

struct Cascade {
    float length = 0.0f;     // tile size, m
    float kLow = 0.0f;       // |k| band [kLow, kHigh) in rad / m
    float kHigh = 0.0f;
    float heightVar = 0.0f;  // m^2
    float slopeVar = 0.0f;
};
struct Cascades {
    int size = 256;  // N x N per cascade
    int count = 4;
    std::array<Cascade, MaxCascades> c{};
};

// quality 0 low (3 x 128), 1 medium (4 x 256), 2 high (4 x 512); non-commensurate tiles, disjoint k bands
Cascades CascadesFor( int quality );
// fills heightVar / slopeVar of each cascade for a sea state (coarse 64^2 sampling of the band)
void CascadeStats( Cascades& cs, const SpectrumParams& p );

}  // namespace OceanFFT
