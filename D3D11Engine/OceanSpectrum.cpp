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

}  // namespace OceanFFT
