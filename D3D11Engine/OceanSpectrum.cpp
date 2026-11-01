#include "OceanSpectrum.h"

#include <algorithm>
#include <cmath>

namespace OceanFFT {

namespace {

constexpr double Pi = 3.14159265358979323846;

struct Jonswap {
    double alpha = 0.0;   // Phillips constant
    double omegaP = 0.0;  // peak angular frequency
    double gamma = 3.3;   // peak enhancement
};

float Lerpf( float a, float b, float t ) { return a + (b - a) * t; }

// Hasselmann et al. 1973 (JONSWAP): alpha = 0.076 (U^2 / (F g))^0.22, w_p = 22 (g^2 / (U F))^(1/3)
Jonswap JonswapFor( double windSpeed, double fetch ) {
    const double u = std::max( windSpeed, 0.1 );
    const double f = std::max( fetch, 100.0 );
    Jonswap j;
    j.alpha = 0.076 * std::pow( u * u / (f * Gravity), 0.22 );
    j.omegaP = 22.0 * std::cbrt( Gravity * Gravity / (u * f) );
    return j;
}

double JonswapS( double omega, const Jonswap& j ) {
    if ( omega <= 1e-6 || j.alpha <= 0.0 ) return 0.0;
    const double sigma = omega <= j.omegaP ? 0.07 : 0.09;
    const double d = omega - j.omegaP;
    const double r = std::exp( -d * d / (2.0 * sigma * sigma * j.omegaP * j.omegaP) );
    const double ratio = j.omegaP / omega;
    return j.alpha * Gravity * Gravity / std::pow( omega, 5.0 ) * std::exp( -1.25 * ratio * ratio * ratio * ratio ) * std::pow( j.gamma, r );
}

double SignificantHeight( const Jonswap& j ) {
    double m0 = 0.0;
    const double w0 = 0.2 * j.omegaP, w1 = 20.0 * j.omegaP;
    const int steps = 4000;
    const double dw = (w1 - w0) / steps;
    for ( int i = 0; i < steps; ++i ) m0 += JonswapS( w0 + (i + 0.5) * dw, j ) * dw;
    return 4.0 * std::sqrt( m0 );
}

double HasselmannS( double omega, double omegaP, double windSpeed, double xi ) {
    const double r = omega / std::max( omegaP, 1e-6 );
    double s;
    if ( r <= 1.0 ) s = 6.97 * std::pow( r, 4.06 );
    else s = 9.77 * std::pow( r, -2.33 - 1.45 * (windSpeed * omegaP / Gravity - 1.17) );
    s += 16.0 * std::tanh( std::min( 1.0 / std::max( r, 1e-3 ), 20.0 ) ) * xi * xi;
    return std::clamp( s, 0.0, 200.0 );
}

double LGamma( double x ) {
    double shift = 0.0;
    while ( x < 7.0 ) { shift += std::log( x ); x += 1.0; }
    const double inv = 1.0 / x, inv2 = inv * inv;
    const double series = inv * (1.0 / 12.0 - inv2 * (1.0 / 360.0 - inv2 * (1.0 / 1260.0 - inv2 / 1680.0)));
    return (x - 0.5) * std::log( x ) - x + 0.5 * std::log( 2.0 * Pi ) + series - shift;
}

// D(theta) = Q(s) |cos(theta / 2)|^{2 s}, normalised over [-pi, pi]
double HasselmannD( double theta, double s ) {
    const double lnQ = (2.0 * s - 1.0) * std::log( 2.0 ) - std::log( Pi ) + 2.0 * LGamma( s + 1.0 ) - LGamma( 2.0 * s + 1.0 );
    const double c = std::abs( std::cos( 0.5 * theta ) );
    if ( c <= 0.0 ) return 0.0;
    return std::exp( lnQ + 2.0 * s * std::log( c ) );
}

double WrapAngle( double a ) {
    while ( a > Pi ) a -= 2.0 * Pi;
    while ( a < -Pi ) a += 2.0 * Pi;
    return a;
}

}  // namespace

SeaState SeaStateForLevel( float level ) {
    struct Level { float wind, fetch, swellH, swellT, chop; };
    static constexpr Level Levels[4] = {
        { 5.5f, 30000.0f, 1.50f, 12.0f, 0.80f },
        { 8.0f, 60000.0f, 1.70f, 12.5f, 0.90f },
        { 11.5f, 110000.0f, 2.00f, 13.0f, 1.10f },
        { 15.0f, 180000.0f, 2.50f, 14.0f, 1.25f },
    };
    const float l = std::clamp( level, 0.0f, 3.0f );
    const int i = std::min( static_cast<int>(l), 2 );
    const float t = l - static_cast<float>(i);
    const Level& a = Levels[i];
    const Level& b = Levels[i + 1];
    SeaState s;
    s.windSpeed = Lerpf( a.wind, b.wind, t );
    s.fetch = std::exp( Lerpf( std::log( a.fetch ), std::log( b.fetch ), t ) );
    s.swellHeight = Lerpf( a.swellH, b.swellH, t );
    s.swellPeriod = Lerpf( a.swellT, b.swellT, t );
    s.choppiness = Lerpf( a.chop, b.chop, t );
    s.swellSpread = 60.0f;
    return s;
}

SpectrumParams MakeSpectrumParams( const SeaState& s ) {
    SpectrumParams p;
    const Jonswap w = JonswapFor( s.windSpeed, s.fetch );
    const double scale2 = static_cast<double>(s.scale) * s.scale;  // amplitude -> variance
    p.windAlpha = static_cast<float>(w.alpha * scale2);
    p.windOmegaP = static_cast<float>(w.omegaP);
    p.windGamma = static_cast<float>(w.gamma);
    p.windSpeed = s.windSpeed;
    p.windAngle = s.windAngle;
    p.spreadXi = s.spreadXi;
    // swell: a JONSWAP shape at its own peak, alpha chosen so its significant height is swellHeight
    Jonswap sw;
    sw.omegaP = 2.0 * Pi / std::max( s.swellPeriod, 1.0f );
    sw.alpha = 1.0;
    const double hs1 = SignificantHeight( sw );
    const double ratio = hs1 > 0.0 ? s.swellHeight / hs1 : 0.0;
    p.swellAlpha = static_cast<float>(ratio * ratio * scale2);
    p.swellOmegaP = static_cast<float>(sw.omegaP);
    p.swellAngle = s.swellAngle;
    p.swellSpread = s.swellSpread;
    return p;
}

double SpectrumK( double kx, double kz, const SpectrumParams& p ) {
    const double k = std::sqrt( kx * kx + kz * kz );
    if ( k < 1e-6 ) return 0.0;
    const double omega = std::sqrt( Gravity * k );
    const double dOmegaDk = Gravity / (2.0 * omega);
    const double theta = std::atan2( kz, kx );
    double s = 0.0;
    if ( p.windAlpha > 0.0f ) {
        const Jonswap j{ p.windAlpha, p.windOmegaP, p.windGamma };
        s += JonswapS( omega, j ) * HasselmannD( WrapAngle( theta - p.windAngle ), HasselmannS( omega, p.windOmegaP, p.windSpeed, p.spreadXi ) );
    }
    if ( p.swellAlpha > 0.0f ) {
        const Jonswap j{ p.swellAlpha, p.swellOmegaP, 3.3 };
        s += JonswapS( omega, j ) * HasselmannD( WrapAngle( theta - p.swellAngle ), p.swellSpread );
    }
    const double cut = static_cast<double>(p.shortWaveCut) * k;
    return s * dOmegaDk / k * std::exp( -cut * cut );
}

Cascades CascadesFor( int quality ) {
    Cascades cs;
    static constexpr float Low3[3] = { 250.0f, 50.0f, 10.0f };
    static constexpr float Four[4] = { 310.0f, 87.0f, 23.0f, 6.0f };
    if ( quality <= 0 ) {
        cs.size = 128;
        cs.count = 3;
        for ( int i = 0; i < 3; ++i ) cs.c[i].length = Low3[i];
    } else {
        cs.size = quality >= 2 ? 512 : 256;
        cs.count = 4;
        for ( int i = 0; i < 4; ++i ) cs.c[i].length = Four[i];
    }
    for ( int i = 0; i < cs.count; ++i ) {
        Cascade& c = cs.c[i];
        c.kLow = i == 0 ? 0.0f : cs.c[i - 1].kHigh;
        c.kHigh = i + 1 < cs.count ? 4.0f * 2.0f * static_cast<float>(Pi) / cs.c[i + 1].length : 1e9f;
    }
    return cs;
}

void CascadeStats( Cascades& cs, const SpectrumParams& p ) {
    constexpr int Grid = 64;   // coarse: the stats only drive specular roughness
    const double step = static_cast<double>(cs.size) / Grid;
    for ( int c = 0; c < cs.count; ++c ) {
        Cascade& cc = cs.c[c];
        const double dk = 2.0 * Pi / cc.length * step;
        double hv = 0.0, sv = 0.0;
        for ( int z = 0; z < Grid; ++z ) {
            for ( int x = 0; x < Grid; ++x ) {
                const double kx = (x * step - cs.size / 2) * 2.0 * Pi / cc.length;
                const double kz = (z * step - cs.size / 2) * 2.0 * Pi / cc.length;
                const double k = std::sqrt( kx * kx + kz * kz );
                if ( k < cc.kLow || k >= cc.kHigh ) continue;
                const double e = SpectrumK( kx, kz, p ) * dk * dk;
                hv += e;
                sv += e * k * k;
            }
        }
        cc.heightVar = static_cast<float>(hv);
        cc.slopeVar = static_cast<float>(sv);
    }
}

// ---- CPU surface ------------------------------------------------------------------------------------------------------------

namespace {

// Mirrors of the hash and gaussian pair in OceanFFT.hlsl, so the CPU waves are the GPU waves
uint32_t HashU32( uint32_t v ) {
    const uint32_t state = v * 747796405u + 2891336453u;
    const uint32_t word = ( ( state >> ( ( state >> 28u ) + 4u ) ) ^ state ) * 277803737u;
    return ( word >> 22u ) ^ word;
}

void GaussianPair( uint32_t x, uint32_t z, uint32_t c, float out[2] ) {
    const uint32_t h1 = HashU32( x + HashU32( z + HashU32( c + 0x9e3779b9u ) ) );
    const uint32_t h2 = HashU32( h1 ^ 0x68bc21ebu );
    const float u1 = std::max( static_cast<float>( h1 >> 8 ) * ( 1.0f / 16777216.0f ), 1e-7f );
    const float u2 = static_cast<float>( h2 >> 8 ) * ( 1.0f / 16777216.0f );
    const float r = std::sqrt( -2.0f * std::log( u1 ) );
    const float a = 6.28318530718f * u2;
    out[0] = r * std::cos( a );
    out[1] = r * std::sin( a );
}

// h0 of texel (x, z) of cascade c, 0 outside its band
void H0( int x, int z, int n, const Cascade& cc, uint32_t c, const SpectrumParams& p, float out[2] ) {
    const float dk = 6.28318530718f / cc.length;
    const float kx = static_cast<float>( x - n / 2 ) * dk, kz = static_cast<float>( z - n / 2 ) * dk;
    const float k = std::sqrt( kx * kx + kz * kz );
    out[0] = out[1] = 0.0f;
    if ( k < cc.kLow || k >= cc.kHigh || k < 1e-6f ) return;
    const float amp = 0.5f * std::sqrt( static_cast<float>( SpectrumK( kx, kz, p ) ) ) * dk;
    float g[2];
    GaussianPair( static_cast<uint32_t>( x ), static_cast<uint32_t>( z ), c, g );
    out[0] = g[0] * amp;
    out[1] = g[1] * amp;
}

}  // namespace

Surface& GetSurface() {
    static Surface surface;
    return surface;
}

void Surface::BuildModes() {
    m_Modes.clear();
    const int n = m_Cascades.size;
    const float w0 = 6.28318530718f / RepeatTime;
    for ( int c = 0; c < m_Cascades.count; ++c ) {
        const Cascade& cc = m_Cascades.c[c];
        if ( cc.length < 20.0f || cc.kHigh > 1e8f ) continue;   // the ripple cascades do not move a body
        const float dk = 6.28318530718f / cc.length;
        const int radius = static_cast<int>( std::ceil( cc.kHigh / dk ) ) + 1;
        for ( int pz = std::max( 0, n / 2 - radius ); pz <= std::min( n - 1, n / 2 + radius ); ++pz ) {
            for ( int px = std::max( 0, n / 2 - radius ); px <= std::min( n - 1, n / 2 + radius ); ++px ) {
                float h[2], hm[2];
                H0( px, pz, n, cc, static_cast<uint32_t>( c ), m_Params, h );
                if ( h[0] == 0.0f && h[1] == 0.0f ) continue;
                H0( ( n - px ) % n, ( n - pz ) % n, n, cc, static_cast<uint32_t>( c ), m_Params, hm );   // the -k texel
                Mode m;
                m.kx = static_cast<float>( px - n / 2 ) * dk;
                m.kz = static_cast<float>( pz - n / 2 ) * dk;
                m.k = std::sqrt( m.kx * m.kx + m.kz * m.kz );
                m.omega = std::floor( std::sqrt( static_cast<float>( Gravity ) * m.k ) / w0 ) * w0;
                m.h0r = h[0]; m.h0i = h[1];
                m.gr = hm[0]; m.gi = -hm[1];   // conj( h0( -k ) )
                m.hr = m.hi = 0.0f;
                m_Modes.push_back( m );
            }
        }
    }
}

void Surface::EvolveModes() {
    for ( Mode& m : m_Modes ) {
        const float ph = m.omega * m_Wrapped;
        const float c = std::cos( ph ), s = std::sin( ph );
        // h = h0 e^{i w t} + conj( h0( -k ) ) e^{-i w t}
        m.hr = m.h0r * c - m.h0i * s + m.gr * c + m.gi * s;
        m.hi = m.h0r * s + m.h0i * c - m.gr * s + m.gi * c;
    }
}

void Surface::Refresh( int simulation, float seaState, float windAngleDeg, float waveScale, float choppiness, int maxQuality ) {
    if ( simulation <= 0 ) {
        m_Active = false;
        m_Quality = -1;
        return;
    }
    const int quality = std::min( simulation - 1, maxQuality );
    SeaState sea = SeaStateForLevel( seaState );
    sea.windAngle = windAngleDeg * 0.0174532925f;
    sea.swellAngle = sea.windAngle + 0.4f;
    sea.scale = waveScale;
    sea.choppiness *= choppiness;

    auto differs = []( float a, float b ) { return std::abs( a - b ) > 0.01f * std::max( 1.0f, std::abs( a ) ); };
    const bool rebuild = quality != m_Quality || differs( sea.windSpeed, m_Sea.windSpeed ) || differs( sea.fetch, m_Sea.fetch )
        || differs( sea.windAngle, m_Sea.windAngle ) || differs( sea.swellHeight, m_Sea.swellHeight )
        || differs( sea.swellPeriod, m_Sea.swellPeriod ) || differs( sea.scale, m_Sea.scale );
    m_Sea.choppiness = sea.choppiness;
    if ( rebuild ) {
        m_Sea = sea;
        m_Quality = quality;
        m_Params = MakeSpectrumParams( sea );
        m_Cascades = CascadesFor( quality );
        ++m_Generation;
        BuildModes();
    }

    // One clock for the CPU waves and every backend simulation
    const auto now = std::chrono::steady_clock::now();
    const float dt = m_HaveLast ? std::clamp( std::chrono::duration<float>( now - m_Last ).count(), 0.0f, 0.1f ) : 0.0f;
    m_Last = now;
    m_HaveLast = true;
    m_Seconds += dt;
    m_Wrapped = static_cast<float>( std::fmod( m_Seconds, static_cast<double>( RepeatTime ) ) );
    EvolveModes();
    m_Active = true;
}

void Surface::Displacement( float x, float z, float weight, float out[3] ) const {
    const float X = x / UnitsPerMetre, Z = z / UnitsPerMetre;
    float dx = 0.0f, dy = 0.0f, dz = 0.0f;
    for ( const Mode& m : m_Modes ) {
        const float theta = m.kx * X + m.kz * Z;
        const float c = std::cos( theta ), s = std::sin( theta );
        const float a = m.hr * c - m.hi * s;   // Re( h e^{i theta} ): the height
        const float b = m.hr * s + m.hi * c;   // Im: the choppy shift is Re( -i kx / k h e^{i theta} )
        const float f = b / m.k;
        dy += a;
        dx += m.kx * f;
        dz += m.kz * f;
    }
    const float scale = UnitsPerMetre * weight;
    out[0] = dx * m_Sea.choppiness * scale;
    out[1] = dy * scale;
    out[2] = dz * m_Sea.choppiness * scale;
}

float Surface::HeightAt( float x, float z, float weight ) const {
    if ( !m_Active || weight <= 0.0f ) return 0.0f;
    float px = x, pz = z, d[3] = {};
    for ( int i = 0; i < 3; ++i ) {   // find the rest position whose displaced position is (x, z)
        Displacement( px, pz, weight, d );
        px = x - d[0];
        pz = z - d[2];
    }
    Displacement( px, pz, weight, d );
    return d[1];
}

}  // namespace OceanFFT
