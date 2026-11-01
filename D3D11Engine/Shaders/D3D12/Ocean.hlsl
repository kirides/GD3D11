// D3D12/Vulkan entry points of the FFT ocean (../include/OceanFFT.hlsl), bindless. One entry per pass; CSFFT is compiled
// per tile size (OCEAN_N, OCEAN_LOG2N).
#include "../include/OceanFFTTypes.hlsl"

cbuffer OceanCB : register( b0 )
{
    OceanPC pc;
    uint4 ocIdxA;   // x h0 buffer UAV, y work A UAV, z work B UAV, w foam buffer UAV
    uint4 ocIdxB;   // x displacement array UAV (current mip), y derivative array UAV, z previous-mip displacement SRV, w derivative SRV
};

float4 OcH0Ld( uint i ) { RWStructuredBuffer<float4> b = GD_HEAP( ocIdxA.x ); return b[i]; }
void OcH0St( uint i, float4 v ) { RWStructuredBuffer<float4> b = GD_HEAP( ocIdxA.x ); b[i] = v; }
float4 OcWaLd( uint i ) { RWStructuredBuffer<float4> b = GD_HEAP( ocIdxA.y ); return b[i]; }
void OcWaSt( uint i, float4 v ) { RWStructuredBuffer<float4> b = GD_HEAP( ocIdxA.y ); b[i] = v; }
float4 OcWbLd( uint i ) { RWStructuredBuffer<float4> b = GD_HEAP( ocIdxA.z ); return b[i]; }
void OcWbSt( uint i, float4 v ) { RWStructuredBuffer<float4> b = GD_HEAP( ocIdxA.z ); b[i] = v; }
float OcFoamLd( uint i ) { RWStructuredBuffer<float> b = GD_HEAP( ocIdxA.w ); return b[i]; }
void OcFoamSt( uint i, float v ) { RWStructuredBuffer<float> b = GD_HEAP( ocIdxA.w ); b[i] = v; }
void OcDispSt( int2 p, float4 v ) { RWTexture2DArray<float4> t = GD_HEAP( ocIdxB.x ); t[int3( p, (int)pc.wind2.w )] = v; }
void OcDerivSt( int2 p, float4 v ) { RWTexture2DArray<float4> t = GD_HEAP( ocIdxB.y ); t[int3( p, (int)pc.wind2.w )] = v; }
float4 OcMipDispLd( int2 q ) { Texture2DArray<float4> t = GD_HEAP( ocIdxB.z ); return t.Load( int4( q, (int)pc.wind2.w, 0 ) ); }
float4 OcMipDerivLd( int2 q ) { Texture2DArray<float4> t = GD_HEAP( ocIdxB.w ); return t.Load( int4( q, (int)pc.wind2.w, 0 ) ); }

#define OCEAN_H0_LD( i ) OcH0Ld( i )
#define OCEAN_H0_ST( i, v ) OcH0St( i, v )
#define OCEAN_WA_LD( i ) OcWaLd( i )
#define OCEAN_WA_ST( i, v ) OcWaSt( i, v )
#define OCEAN_WB_LD( i ) OcWbLd( i )
#define OCEAN_WB_ST( i, v ) OcWbSt( i, v )
#define OCEAN_FOAM_LD( i ) OcFoamLd( i )
#define OCEAN_FOAM_ST( i, v ) OcFoamSt( i, v )
#define OCEAN_DISP_ST( p, v ) OcDispSt( p, v )
#define OCEAN_DERIV_ST( p, v ) OcDerivSt( p, v )
#define OCEAN_MIP_DISP_LD( q ) OcMipDispLd( q )
#define OCEAN_MIP_DERIV_LD( q ) OcMipDerivLd( q )
#define OCEAN_MIP_ST( p, d, v ) { OcDispSt( p, d ); OcDerivSt( p, v ); }

#include "../include/OceanFFT.hlsl"

[numthreads( 8, 8, 1 )]
void CSSpectrum( uint3 id : SV_DispatchThreadID ) { OceanSpectrumPass( id ); }

[numthreads( 8, 8, 1 )]
void CSEvolve( uint3 id : SV_DispatchThreadID ) { OceanEvolvePass( id ); }

#ifdef OCEAN_N
[numthreads( OCEAN_N / 2, 1, 1 )]
void CSFFT( uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID ) { OceanFFTPass( gid, tid ); }
#endif

[numthreads( 8, 8, 1 )]
void CSResolve( uint3 id : SV_DispatchThreadID ) { OceanResolvePass( id ); }

[numthreads( 8, 8, 1 )]
void CSMip( uint3 id : SV_DispatchThreadID ) { OceanMipPass( id ); }
