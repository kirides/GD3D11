// D3D11 entry points of the FFT ocean (include/OceanFFT.hlsl). One pass per compile, picked by OCEAN_PASS_*; the FFT is
// compiled per tile size (OCEAN_N, OCEAN_LOG2N).
#include <include/OceanFFTTypes.hlsl>

cbuffer OceanCB : register( b0 ) { OceanPC pc; };

RWStructuredBuffer<float4> RW_H0 : register( u0 );
RWStructuredBuffer<float4> RW_WorkA : register( u1 );
RWStructuredBuffer<float4> RW_WorkB : register( u2 );
RWStructuredBuffer<float> RW_Foam : register( u3 );
RWTexture2DArray<float4> RW_Disp : register( u4 );    // R16G16B16A16_FLOAT, store only (no typed loads on FL11.0)
RWTexture2DArray<float4> RW_Deriv : register( u5 );
Texture2DArray<float4> TX_Disp : register( t0 );      // the previous mip (mip pass)
Texture2DArray<float4> TX_Deriv : register( t1 );

#define OCEAN_H0_LD( i ) RW_H0[i]
#define OCEAN_H0_ST( i, v ) RW_H0[i] = v
#define OCEAN_WA_LD( i ) RW_WorkA[i]
#define OCEAN_WA_ST( i, v ) RW_WorkA[i] = v
#define OCEAN_WB_LD( i ) RW_WorkB[i]
#define OCEAN_WB_ST( i, v ) RW_WorkB[i] = v
#define OCEAN_FOAM_LD( i ) RW_Foam[i]
#define OCEAN_FOAM_ST( i, v ) RW_Foam[i] = v
#define OCEAN_DISP_ST( p, v ) RW_Disp[int3( p, (int)pc.wind2.w )] = v
#define OCEAN_DERIV_ST( p, v ) RW_Deriv[int3( p, (int)pc.wind2.w )] = v
#define OCEAN_MIP_DISP_LD( q ) TX_Disp.Load( int4( q, (int)pc.wind2.w, 0 ) )
#define OCEAN_MIP_DERIV_LD( q ) TX_Deriv.Load( int4( q, (int)pc.wind2.w, 0 ) )
#define OCEAN_MIP_ST( p, d, v ) { RW_Disp[int3( p, (int)pc.wind2.w )] = d; RW_Deriv[int3( p, (int)pc.wind2.w )] = v; }

#include <include/OceanFFT.hlsl>

#if defined( OCEAN_PASS_SPECTRUM )
[numthreads( 8, 8, 1 )]
void CSMain( uint3 id : SV_DispatchThreadID ) { OceanSpectrumPass( id ); }
#elif defined( OCEAN_PASS_EVOLVE )
[numthreads( 8, 8, 1 )]
void CSMain( uint3 id : SV_DispatchThreadID ) { OceanEvolvePass( id ); }
#elif defined( OCEAN_PASS_FFT )
[numthreads( OCEAN_N / 2, 1, 1 )]
void CSMain( uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID ) { OceanFFTPass( gid, tid ); }
#elif defined( OCEAN_PASS_RESOLVE )
[numthreads( 8, 8, 1 )]
void CSMain( uint3 id : SV_DispatchThreadID ) { OceanResolvePass( id ); }
#elif defined( OCEAN_PASS_MIP )
// The mip pass loads the previous mip through TX_* and stores into the next mip's UAVs
[numthreads( 8, 8, 1 )]
void CSMain( uint3 id : SV_DispatchThreadID ) { OceanMipPass( id ); }
#endif
