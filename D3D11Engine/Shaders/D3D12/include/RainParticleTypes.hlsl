// Rain/snow particle records, shared by AdvanceRain.hlsl (the CS that integrates them) and Rain.hlsl
// (the VS/PS billboard draw). Layout-mirrors D3D11/D3D12's shared CPU struct of the same name in
// WorldObjects.h (RainParticleDynamic/RainParticleStatic) — the CPU never needs its own HLSL-facing
// header for these since D3D12Rain.cpp already includes WorldObjects.h directly, but a stride mismatch
// against THAT struct would silently corrupt every particle, so keep the two in lockstep by hand.
#ifndef D3D12_RAINPARTICLETYPES_HLSL
#define D3D12_RAINPARTICLETYPES_HLSL

// Mutable per-particle data, updated every frame by the advance CS.
struct RainParticleDynamic
{
    float3 position;
    float3 velocity;
};

// Immutable per-particle data, set once at initialization. Bound as StructuredBuffer SRV.
struct RainParticleStatic
{
    float3 seed;
    float  randomBrightness;
    int    drawMode;
};

#endif // D3D12_RAINPARTICLETYPES_HLSL
