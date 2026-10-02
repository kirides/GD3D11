#!/usr/bin/env python3
"""Generates Shaders/include/ZenGinWaveSpectrum.hlsl, the static wave spectrum behind ZenGin's water waves.

ZenGin (zCPolygon::ApplyMorphing -> zCFFT::S_CalcWave2D) moves every vertex of a material with a wave mode by
sin(phase + |h| * 1e7) * waveMaxAmplitude, where h is one cell of a 32x32 complex map that zCFFT::AnimateWaveMap
rebuilds per frame from a seeded Gaussian spectrum (FillH0Array) and a row-wise inverse FFT. This script replays
that spectrum bit-for-bit in float32 and emits it in the order the shader sums it:

    h(R, C, t) = 1/32 * sum_x A[R][x] * exp(-i * (w(x, R) * t + 2pi * x * C / 32))

A[R][x] = h0[(-x)&31][(-R)&31]: zComplex::operator+ returns only its right operand, so AnimateWaveMap's
r*p + s*q is s*q alone, and s is h0 read mirrored and transposed. w = sqrt(9.81 * LengthApprox(k)) stays in the
shader. fft_float's "inverse" keeps the forward twiddle sign, hence the minus on the DFT term too. --check
replays AnimateWaveMap + fft_float literally and compares it with the closed form above.

Usage: python tools/gen_zengin_wave_spectrum.py [--check]
"""
import math
import os
import sys

import numpy as np

F = np.float32
RES = 32
OUT = os.path.join(os.path.dirname(__file__), "..", "D3D11Engine", "Shaders", "include", "ZenGinWaveSpectrum.hlsl")


class ZRandomGauss:
    """zRandom_Gauss: Marsaglia's uniform generator (FSU-SCRI-87-50) + Kinderman-Monahan, in float like zREAL."""

    def __init__(self, ij, kl):
        i = (ij // 177) % 177 + 2
        j = (ij % 177) + 2
        k = (kl // 169) % 178 + 1
        l = kl % 169
        self.u = []
        for _ in range(97):
            s, t = F(0.0), F(0.5)
            for _ in range(24):
                m = (((i * j) % 179) * k) % 179
                i, j, k = j, k, m
                l = (53 * l + 1) % 169
                if (l * m) % 64 >= 32:
                    s = F(s + t)
                t = F(t * F(0.5))
            self.u.append(s)
        self.c = F(362436.0 / 16777216.0)
        self.cd = F(7654321.0 / 16777216.0)
        self.cm = F(16777213.0 / 16777216.0)
        self.i97, self.j97 = 97, 33

    def uniform(self):
        uni = F(self.u[self.i97 - 1] - self.u[self.j97 - 1])
        if uni <= 0.0:
            uni = F(uni + F(1.0))
        self.u[self.i97 - 1] = uni
        self.i97 = self.i97 - 1 or 97
        self.j97 = self.j97 - 1 or 97
        self.c = F(self.c - self.cd)
        if self.c < 0.0:
            self.c = F(self.c + self.cm)
        uni = F(uni - self.c)
        if uni < 0.0:
            uni = F(uni + F(1.0))
        return uni

    def gaussian(self):
        while True:
            u = self.uniform()
            v = self.uniform()
            if u <= 0.0 or v <= 0.0:
                u, v = F(1.0), F(1.0)
            v = F(1.7156 * (float(v) - 0.5))
            x = F(float(u) - 0.449871)
            y = F(abs(float(v)) + 0.386595)
            q = F(float(x) * float(x) + float(y) * (0.19600 * float(y) - 0.25472 * float(x)))
            if q < 0.27597:
                break
            if not (q > 0.27846 or float(v) * float(v) > -4.0 * math.log(float(u)) * float(u) * float(u)):
                break
        return F(v / u)


def make_wave(m, n):
    return np.array([2.0 * math.pi * m / RES, 0.01, 2.0 * math.pi * n / RES], dtype=F)


def length_approx(v):
    """zVEC3::LengthApprox (Graphics Gems IV dist_fast)."""
    a = sorted((abs(float(c)) for c in v), reverse=True)
    t = a[1] + a[2]
    return a[0] - a[0] / 16.0 + t / 4.0 + t / 8.0


def fill_h0():
    """zCFFT::FillH0Array( 1.0f, (1,0,0) ); h0[z][x], real part only (img is always 0)."""
    gauss = ZRandomGauss(0, 100)
    euler, a = F(2.71828), F(1.0)
    w = F(1.0)
    l = F(w * w / F(9.81))
    h0 = np.zeros((RES, RES), dtype=F)
    for z in range(RES):
        for x in range(RES):
            wave = make_wave(x - RES // 2, z - RES // 2)
            k = F(np.sqrt(np.sum(wave.astype(np.float64) ** 2)))
            with np.errstate(over="ignore", under="ignore", divide="ignore"):
                eulerterm = F(float(euler) ** float(F(-1.0) / F(k * l * k * l)))
                k4 = F(k * k * k * k)
                n = wave / k
                dotprod = F(n[0])  # wind is (1,0,0)
                dotprod = F(dotprod * dotprod)
                elim = F(float(euler) ** float(-(k * k) * (w * w)))
                sphk = F(math.sqrt(float(a) * float(eulerterm / k4) * float(dotprod) * float(elim)))
            h0[z][x] = F(F(1.0 / math.sqrt(2.0)) * F(float(gauss.gaussian()) * float(sphk)))
    return h0


def omega(x, row):
    return math.sqrt(9.81 * length_approx(make_wave(x - RES // 2, row - RES // 2)))


def spectrum(h0):
    return np.array([[h0[(-x) & 31][(-r) & 31] for x in range(RES)] for r in range(RES)], dtype=F)


def fft_float_inverse(inp):
    """zCFFT::fft_float( 32, 1, In, Out ): Don Cross' radix-2 FFT, inverse and normalized."""
    n = len(inp)
    bits = n.bit_length() - 1
    out = [0j] * n
    for i in range(n):
        j = int(format(i, f"0{bits}b")[::-1], 2)
        out[j] = inp[i]
    block_end = 1
    block_size = 2
    while block_size <= n:
        delta = -2.0 * math.pi / block_size
        sm2, cm2 = math.sin(-2 * delta), math.cos(-2 * delta)
        sm1, cm1 = math.sin(-delta), math.cos(-delta)
        w = 2 * cm1
        for i in range(0, n, block_size):
            ar = [0.0, cm1, cm2]
            ai = [0.0, sm1, sm2]
            for m in range(block_end):
                j = i + m
                ar[0] = w * ar[1] - ar[2]; ar[2] = ar[1]; ar[1] = ar[0]
                ai[0] = w * ai[1] - ai[2]; ai[2] = ai[1]; ai[1] = ai[0]
                k = j + block_end
                t = complex(ar[0] * out[k].real - ai[0] * out[k].imag, ar[0] * out[k].imag + ai[0] * out[k].real)
                out[k] = out[j] - t
                out[j] = out[j] + t
        block_end = block_size
        block_size <<= 1
    return [v / n for v in out]


def closed_form(a, r, c, t):
    return sum(float(a[r][x]) * complex(math.cos(phi), -math.sin(phi))
               for x in range(RES) for phi in [omega(x, r) * t + 2 * math.pi * x * c / RES]) / RES


def check(h0, a):
    for t in (0.0, 3.7, 1234.5):
        for r in (0, 5, 17, 31):
            row = []
            for x in range(RES):
                wkt = omega(x, r) * t
                s = float(h0[(-x) & 31][(-r) & 31])           # GetMap( m_h0Map, -x, -z ) with z = r
                row.append(s * complex(math.cos(wkt), -math.sin(wkt)))  # s * q, operator+ dropped r * p
            literal = fft_float_inverse(row)
            for c in range(RES):
                closed = closed_form(a, r, c, t)
                if abs(closed - literal[c]) > 1e-6 * max(abs(literal[c]), 1e-12):
                    sys.exit(f"mismatch t={t} R={r} C={c}: {closed} vs {literal[c]}")
    mags = []
    for t in np.linspace(0.0, 30.0, 61):
        for r in range(RES):
            for c in range(RES):
                mags.append(abs(closed_form(a, r, c, t)) * 1e7)
    mags = np.array(mags)
    print(f"closed form matches fft_float; |h|*1e7: min {mags.min():.3f} median {np.median(mags):.3f} "
          f"max {mags.max():.3f}")


def main():
    h0 = fill_h0()
    a = spectrum(h0)
    big = np.abs(a) > 1e-12
    print(f"non-negligible spectrum entries: {big.sum()} / {RES * RES}, max |A| {np.abs(a).max():.4g}")
    if "--check" in sys.argv:
        check(h0, a)

    lines = [
        "#ifndef ZENGIN_WAVE_SPECTRUM_HLSL",
        "#define ZENGIN_WAVE_SPECTRUM_HLSL",
        "// Generated by tools/gen_zengin_wave_spectrum.py - do not edit. ZenGin's zCFFT wave spectrum, row R at",
        "// [R * 8, R * 8 + 8): .x..w = A[R][4i..4i+3] (see the script for the exact derivation).",
        f"static const float4 ZenGinWaveSpectrum[{RES * RES // 4}] = {{",
    ]
    for r in range(RES):
        row = []
        for i in range(0, RES, 4):
            vals = ", ".join(f"{float(v):.9g}" for v in a[r][i:i + 4])
            row.append(f"float4( {vals} )")
        lines.append("    " + ", ".join(row) + ",")
    lines += ["};", "", "#endif // ZENGIN_WAVE_SPECTRUM_HLSL", ""]
    with open(OUT, "w", newline="\n") as f:
        f.write("\n".join(lines))
    print(f"wrote {os.path.normpath(OUT)}")


if __name__ == "__main__":
    main()
