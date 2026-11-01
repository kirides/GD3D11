// FFT ocean compute kernels shared by D3D11 (SM5) and D3D12/Vulkan (SM6). The CPU side is OceanSpectrum.cpp: keep the
// spectrum in step. The including entry file first includes OceanFFTTypes.hlsl, then declares a global `pc` (OceanPC), the
// resources and these accessors, then includes this file:
//   OCEAN_H0_LD(i) / OCEAN_H0_ST(i,v)   float4 buffer, all cascades back to back (cascade * N * N + texel)
//   OCEAN_WA_LD/ST, OCEAN_WB_LD/ST      float4 FFT work buffers, N * N, shared by the cascades (serialised)
//   OCEAN_FOAM_LD(i) / OCEAN_FOAM_ST    float buffer, like h0
//   OCEAN_DISP_ST(p,v) / OCEAN_DERIV_ST(p,v)   mip 0 of the current cascade's slice (int2 p)
//   OCEAN_MIP_DISP_LD(q) / OCEAN_MIP_DERIV_LD(q) (previous mip of the slice), OCEAN_MIP_ST(p, disp, deriv)
#ifndef GD_OCEAN_FFT_HLSL
#define GD_OCEAN_FFT_HLSL

static const float OCEAN_PI = 3.14159265359;
static const float OCEAN_G = 9.81;

float2 OceanCmul( float2 a, float2 b ) { return float2( a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x ); }
float2 OceanCconj( float2 a ) { return float2( a.x, -a.y ); }
float2 OceanCi( float2 a ) { return float2( -a.y, a.x ); }

// wave vector of texel p of the N x N tile: k = 2 pi (p - N / 2) / L
float2 OceanWaveVector( int2 p ) {
    int n = int( pc.tile.w );
    return float2( p - int2( n / 2, n / 2 ) ) * (2.0 * OCEAN_PI / pc.tile.x);
}

// the dispersion quantised to the repeat time (the animation loops)
float OceanDispersion( float k ) {
    float w0 = 2.0 * OCEAN_PI / pc.foam.w;
    return floor( sqrt( OCEAN_G * k ) / w0 ) * w0;
}

// ---- spectrum (OceanSpectrum.cpp) -----------------------------------------------------------------------------------------

float OceanLgamma( float x ) {
    float shift = 0.0;
    [unroll] for ( int i = 0; i < 8; ++i ) {
        if ( x < 7.0 ) { shift += log( x ); x += 1.0; }
    }
    float inv = 1.0 / x;
    float inv2 = inv * inv;
    float series = inv * (1.0 / 12.0 - inv2 * (1.0 / 360.0 - inv2 * (1.0 / 1260.0 - inv2 / 1680.0)));
    return (x - 0.5) * log( x ) - x + 0.5 * log( 2.0 * OCEAN_PI ) + series - shift;
}

float OceanJonswap( float omega, float alpha, float omegaP, float gamma ) {
    if ( omega <= 1e-6 || alpha <= 0.0 ) return 0.0;
    float sigma = omega <= omegaP ? 0.07 : 0.09;
    float d = omega - omegaP;
    float r = exp( -d * d / (2.0 * sigma * sigma * omegaP * omegaP) );
    float ratio = omegaP / omega;
    float r2 = ratio * ratio;
    return alpha * OCEAN_G * OCEAN_G / (omega * omega * omega * omega * omega) * exp( -1.25 * r2 * r2 ) * pow( gamma, r );
}

float OceanHasselmannS( float omega, float omegaP, float windSpeed, float xi ) {
    float r = omega / max( omegaP, 1e-6 );
    float s;
    if ( r <= 1.0 ) s = 6.97 * pow( r, 4.06 );
    else s = 9.77 * pow( r, -2.33 - 1.45 * (windSpeed * omegaP / OCEAN_G - 1.17) );
    s += 16.0 * tanh( min( 1.0 / max( r, 1e-3 ), 20.0 ) ) * xi * xi;
    return clamp( s, 0.0, 200.0 );
}

float OceanHasselmannD( float theta, float s ) {
    float lnQ = (2.0 * s - 1.0) * log( 2.0 ) - log( OCEAN_PI ) + 2.0 * OceanLgamma( s + 1.0 ) - OceanLgamma( 2.0 * s + 1.0 );
    float c = abs( cos( 0.5 * theta ) );
    if ( c <= 0.0 ) return 0.0;
    return exp( lnQ + 2.0 * s * log( c ) );
}

float OceanWrap( float a ) { return a - 2.0 * OCEAN_PI * floor( (a + OCEAN_PI) / (2.0 * OCEAN_PI) ); }

// S(k) [m^4] at the wave vector k (rad / m)
float OceanSpectrumK( float2 kv ) {
    float k = length( kv );
    if ( k < 1e-6 ) return 0.0;
    float omega = sqrt( OCEAN_G * k );
    float dOmegaDk = OCEAN_G / (2.0 * omega);
    float theta = atan2( kv.y, kv.x );
    float s = 0.0;
    if ( pc.wind.x > 0.0 ) {
        float spread = OceanHasselmannS( omega, pc.wind.y, pc.wind.w, pc.wind2.y );
        s += OceanJonswap( omega, pc.wind.x, pc.wind.y, pc.wind.z ) * OceanHasselmannD( OceanWrap( theta - pc.wind2.x ), spread );
    }
    if ( pc.swell.x > 0.0 ) {
        s += OceanJonswap( omega, pc.swell.x, pc.swell.y, 3.3 ) * OceanHasselmannD( OceanWrap( theta - pc.swell.z ), pc.swell.w );
    }
    float cut = pc.wind2.z * k;
    return s * dOmegaDk / k * exp( -cut * cut );
}

uint OceanHash( uint v ) {
    uint state = v * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

float2 OceanGaussianPair( uint x, uint z, uint c ) {
    uint h1 = OceanHash( x + OceanHash( z + OceanHash( c + 0x9e3779b9u ) ) );
    uint h2 = OceanHash( h1 ^ 0x68bc21ebu );
    float u1 = max( float( h1 >> 8 ) * (1.0 / 16777216.0), 1e-7 );
    float u2 = float( h2 >> 8 ) * (1.0 / 16777216.0);
    float r = sqrt( -2.0 * log( u1 ) );
    float a = 6.28318530718 * u2;
    return float2( r * cos( a ), r * sin( a ) );
}

// h0 of texel p (0 outside the cascade's band)
float2 OceanH0( int2 p ) {
    float2 kv = OceanWaveVector( p );
    float k = length( kv );
    if ( k < pc.tile.y || k >= pc.tile.z || k < 1e-6 ) return float2( 0.0, 0.0 );
    float dk = 2.0 * OCEAN_PI / pc.tile.x;
    float amp = 0.5 * sqrt( OceanSpectrumK( kv ) ) * dk;
    return OceanGaussianPair( uint( p.x ), uint( p.y ), uint( pc.wind2.w ) ) * amp;
}

uint OceanTexelIndex( int2 p ) { int n = int( pc.tile.w ); return uint( p.y * n + p.x ); }
uint OceanCascadeBase() { int n = int( pc.tile.w ); return uint( pc.wind2.w ) * uint( n * n ); }

// ---- passes ---------------------------------------------------------------------------------------------------------------

// the initial spectrum of one cascade: h0(k) and conj(h0(-k))
void OceanSpectrumPass( uint3 id ) {
    int2 p = int2( id.xy );
    int n = int( pc.tile.w );
    if ( p.x >= n || p.y >= n ) return;
    int2 m = (int2( n, n ) - p) % n;  // the texel of -k
    OCEAN_H0_ST( OceanCascadeBase() + OceanTexelIndex( p ), float4( OceanH0( p ), OceanCconj( OceanH0( m ) ) ) );
}

// h(k, t) = h0 e^{i w t} + conj(h0(-k)) e^{-i w t} and the spectra of the displacement and its derivatives, packed as four
// complex fields whose inverse transforms are real:
//   A.xy = Dx + i Dz,  A.zw = Dy + i dDy/dx,  B.xy = dDy/dz + i dDx/dx,  B.zw = dDz/dz + i dDx/dz
void OceanEvolvePass( uint3 id ) {
    int2 p = int2( id.xy );
    int n = int( pc.tile.w );
    if ( p.x >= n || p.y >= n ) return;
    uint ti = OceanTexelIndex( p );
    float2 kv = OceanWaveVector( p );
    float k = length( kv );
    if ( k < 1e-6 ) {
        OCEAN_WA_ST( ti, float4( 0.0, 0.0, 0.0, 0.0 ) );
        OCEAN_WB_ST( ti, float4( 0.0, 0.0, 0.0, 0.0 ) );
        return;
    }
    float4 h0 = OCEAN_H0_LD( OceanCascadeBase() + ti );
    float ph = OceanDispersion( k ) * pc.time.x;
    float2 e = float2( cos( ph ), sin( ph ) );
    float2 h = OceanCmul( h0.xy, e ) + OceanCmul( h0.zw, OceanCconj( e ) );
    float2 ih = OceanCi( h );
    float2 dx = -ih * (kv.x / k);
    float2 dz = -ih * (kv.y / k);
    float2 dyx = ih * kv.x;
    float2 dyz = ih * kv.y;
    float2 dxx = h * (kv.x * kv.x / k);
    float2 dzz = h * (kv.y * kv.y / k);
    float2 dxz = h * (kv.x * kv.y / k);
    OCEAN_WA_ST( ti, float4( dx + OceanCi( dz ), h + OceanCi( dyx ) ) );
    OCEAN_WB_ST( ti, float4( dyz + OceanCi( dxx ), dzz + OceanCi( dxz ) ) );
}

// one 1D inverse FFT per line (rows: pc.time.w == 0, columns: 1) over both work buffers, in place; one group per line,
// N / 2 threads, the line in shared memory: bit-reversed load, log2(N) radix-2 stages, + sign for the inverse.
#ifdef OCEAN_N
groupshared float4 gsA[OCEAN_N];
groupshared float4 gsB[OCEAN_N];

uint OceanLineIndex( int ln, int i ) {
    int2 p = pc.time.w < 0.5 ? int2( i, ln ) : int2( ln, i );
    return OceanTexelIndex( p );
}

void OceanFFTPass( uint3 groupId, uint3 localId ) {
    int ln = int( groupId.x );
    int t = int( localId.x );
    [unroll] for ( int j = 0; j < 2; ++j ) {
        int i = t + j * (OCEAN_N / 2);
        int r = int( reversebits( uint( i ) ) >> uint( 32 - OCEAN_LOG2N ) );
        uint ti = OceanLineIndex( ln, i );
        gsA[r] = OCEAN_WA_LD( ti );
        gsB[r] = OCEAN_WB_LD( ti );
    }
    GroupMemoryBarrierWithGroupSync();
    for ( int half_ = 1; half_ < OCEAN_N; half_ *= 2 ) {
        int group = t / half_;
        int pos = t - group * half_;
        int i0 = group * 2 * half_ + pos;
        int i1 = i0 + half_;
        float ang = OCEAN_PI * float( pos ) / float( half_ );
        float2 w = float2( cos( ang ), sin( ang ) );
        float4 a = gsA[i0], b = gsA[i1];
        float4 bw = float4( OceanCmul( b.xy, w ), OceanCmul( b.zw, w ) );
        gsA[i0] = a + bw;
        gsA[i1] = a - bw;
        a = gsB[i0];
        b = gsB[i1];
        bw = float4( OceanCmul( b.xy, w ), OceanCmul( b.zw, w ) );
        gsB[i0] = a + bw;
        gsB[i1] = a - bw;
        GroupMemoryBarrierWithGroupSync();
    }
    [unroll] for ( int j2 = 0; j2 < 2; ++j2 ) {
        int i = t + j2 * (OCEAN_N / 2);
        uint ti = OceanLineIndex( ln, i );
        OCEAN_WA_ST( ti, gsA[i] );
        OCEAN_WB_ST( ti, gsB[i] );
    }
}
#endif

// the transformed fields -> displacement + derivatives of one cascade. The k-grid is centred, so each texel of the plain
// inverse FFT carries a (-1)^(x + z) sign. Foam: where the horizontal displacement folds the surface (Jacobian below the
// threshold, Tessendorf 2001 4.6) foam appears and decays over time (history in a separate buffer).
void OceanResolvePass( uint3 id ) {
    int2 p = int2( id.xy );
    int n = int( pc.tile.w );
    if ( p.x >= n || p.y >= n ) return;
    uint ti = OceanTexelIndex( p );
    float sgn = ((p.x + p.y) & 1) != 0 ? -1.0 : 1.0;
    float4 a = OCEAN_WA_LD( ti ) * sgn;
    float4 b = OCEAN_WB_LD( ti ) * sgn;
    float lambda = pc.time.z;
    float dx = a.x * lambda, dz = a.y * lambda, dy = a.z;
    float dyx = a.w, dyz = b.x;
    float dxx = b.y * lambda, dzz = b.z * lambda, dxz = b.w * lambda;
    float jac = (1.0 + dxx) * (1.0 + dzz) - dxz * dxz;
    uint fi = OceanCascadeBase() + ti;
    float prev = OCEAN_FOAM_LD( fi );
    if ( isnan( prev ) || isinf( prev ) ) prev = 0.0;
    float fresh = clamp( (pc.foam.y - jac) * pc.foam.z, 0.0, 1.0 );
    float foam = clamp( max( prev * exp( -pc.foam.x * pc.time.y ), fresh ), 0.0, 1.0 );
    OCEAN_FOAM_ST( fi, foam );
    OCEAN_DISP_ST( p, float4( dx, dy, dz, foam ) );
    OCEAN_DERIV_ST( p, float4( dyx, dyz, dxx, dzz ) );
}

// box downsample of the previous mip into the next (pc.tile.w = destination size, pc.wind2.w = cascade slice)
void OceanMipPass( uint3 id ) {
    int2 p = int2( id.xy );
    int n = int( pc.tile.w );
    if ( p.x >= n || p.y >= n ) return;
    float4 d = 0, v = 0;
    [unroll] for ( int i = 0; i < 4; ++i ) {
        int2 q = p * 2 + int2( i & 1, i >> 1 );
        d += OCEAN_MIP_DISP_LD( q );
        v += OCEAN_MIP_DERIV_LD( q );
    }
    OCEAN_MIP_ST( p, d * 0.25, v * 0.25 );
}

#endif
