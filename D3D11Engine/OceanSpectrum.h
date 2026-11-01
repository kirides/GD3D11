#pragma once

// FFT ocean spectrum (Tessendorf; JONSWAP + Hasselmann spreading after Horvath 2015) and cascade layout.
// CPU side of OceanFFT.hlsl: keep both in step. SI units (metres, seconds); Gothic's world unit is the centimetre.

#include <array>
#include <chrono>
#include <cstdint>
#include <vector>

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

/** CPU mirror of the simulated sea for gameplay (swimming, floating plants), and the one clock + sea state the GPU simulation
    of every backend follows. Only the long cascades are summed (a few hundred modes): the short ripples do not move a body.
    Refresh() once per frame from the main thread; it is cheap when nothing changed. */
class Surface {
public:
    /** simulation: 0 off, 1..3 low..high; maxQuality caps the tier (D3D11 1). windAngle in degrees. */
    void Refresh( int simulation, float seaState, float windAngleDeg, float waveScale, float choppiness, int maxQuality );

    bool Active() const { return m_Active; }
    int Quality() const { return m_Quality; }
    uint32_t Generation() const { return m_Generation; }   // bumps when the spectrum (h0) has to be rebuilt
    const SeaState& Sea() const { return m_Sea; }
    const SpectrumParams& Params() const { return m_Params; }
    const Cascades& GetCascades() const { return m_Cascades; }
    double Seconds() const { return m_Seconds; }            // monotonic, independent of game pause
    float WrappedSeconds() const { return m_Wrapped; }      // modulo RepeatTime, what the shaders use

    /** Displacement (world units) of the surface point whose rest position is (x, z); weight 0..1 calms the waves (shore). */
    void Displacement( float x, float z, float weight, float out[3] ) const;
    /** Height offset (world units) of the surface above the world position (x, z), allowing for the choppy horizontal shift. */
    float HeightAt( float x, float z, float weight ) const;

private:
    struct Mode { float kx, kz, k, omega, h0r, h0i, gr, gi, hr, hi; };
    void BuildModes();
    void EvolveModes();

    bool m_Active = false;
    int m_Quality = -1;
    uint32_t m_Generation = 0;
    SeaState m_Sea{};
    SpectrumParams m_Params{};
    Cascades m_Cascades{};
    std::vector<Mode> m_Modes;
    double m_Seconds = 0.0;
    float m_Wrapped = 0.0f;
    std::chrono::steady_clock::time_point m_Last{};
    bool m_HaveLast = false;
};

Surface& GetSurface();

/** Wave damping towards the shore from the water depth (world units), the CPU twin of OceanDepthWeight in the water shaders. */
inline float WaveWeightForDepth( float depth ) {
    const float t = depth <= 0.0f ? 0.0f : ( depth >= 600.0f ? 1.0f : depth / 600.0f );
    return t * t * ( 3.0f - 2.0f * t );
}

}  // namespace OceanFFT
