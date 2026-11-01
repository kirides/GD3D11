// Constants of the FFT ocean compute passes (mirror: OceanPassConstants in the C++ side).
#ifndef GD_OCEAN_FFT_TYPES_HLSL
#define GD_OCEAN_FFT_TYPES_HLSL

struct OceanPC {
    float4 tile;   // x tile size (m), y band kLow, z band kHigh (rad / m), w N
    float4 wind;   // x alpha, y omega_p, z gamma, w U10 (m / s)
    float4 wind2;  // x wind angle (rad), y spreading xi, z short-wave cut (m), w cascade index
    float4 swell;  // x alpha, y omega_p, z angle (rad), w spreading exponent s
    float4 time;   // x t (s, mod the repeat time), y dt (s), z choppiness, w FFT direction (0 rows, 1 columns)
    float4 foam;   // x decay (1 / s), y Jacobian threshold, z coverage gain, w repeat time (s)
};

#endif
