// Intel XeGTAO — ground-truth ambient occlusion (MIT licensed; XeGTAO.h / XeGTAO.hlsli next to this file are
// Intel's 1.30 release, see their headers for copyright and the one local edit). This file is the GD3D11 side:
// the engine-specific entry points, resource loaders and noise source that Intel's vaGTAO.hlsl provides in
// their sample. The host side is D3D12Engine/D3D12GTAO.cpp.
//
// D3D12 ONLY. This replaces ASSAO on the D3D12 backend (AoMode == AO_ASSAO); the D3D11 renderer keeps its own
// ASSAO port untouched.
//
// DEPTH SOURCE. Every pass reads the PREVIOUS frame's COMPLETE depth buffer (m_PrevDepth, snapshotted by
// CopyDepthForAO after the last depth writer of the frame), exactly like the simple-SSAO path it replaces —
// Forward+ resolves lighting in the geometry pass, so this frame's depth does not exist yet when the AO mask is
// needed, and the prepass alone would omit grass/foliage/decals/water. The resulting mask lives in the previous
// frame's screen space and the lit pixel shaders reproject into it per-pixel (include/ScreenSpaceAO.hlsl).
//
// FP32, NOT FP16. XE_GTAO_USE_HALF_FLOAT_PRECISION is off and XE_GTAO_FP32_DEPTHS on (Intel's own
// `m_use32bitDepth` configuration). Two reasons, in order: Gothic's view-space depths run to tens of thousands
// of world units and would hit fp16's 65504 ceiling on anything near the horizon, and every resource then has a
// plain `float`/`uint` element type, which is what makes the SM6.6 `ResourceDescriptorHeap` fetches below
// straightforward. The cost is the working-depth chain being R32_FLOAT instead of R16_FLOAT; flipping both
// macros back is the one-line change if that bandwidth ever shows up in a profile.
//
// SKY / CLEARED DEPTH. Gothic's projection is reversed-Z with an INFINITE far plane (GothicAPI::
// GetProjectionMatrix: _33 = 0, depth = 1/viewZ), so a cleared depth of 0 linearizes to a division by zero.
// The host works around it in DepthUnpackConsts rather than here — see the comment on kSkyDepthEpsilon in
// D3D12GTAO.cpp — which keeps Intel's files unmodified and costs a relative error below 1e-4 at the far plane.

// XE_GTAO_USE_DEFAULT_CONSTANTS 0: take the heuristics from the constant buffer instead of baking Intel's
// defaults in, so the ImGui panel can tune radius multiplier / falloff / sample distribution / thin-occluder
// compensation live against real Gothic geometry.
#define XE_GTAO_USE_DEFAULT_CONSTANTS       0
#define XE_GTAO_USE_HALF_FLOAT_PRECISION    0
#define XE_GTAO_FP32_DEPTHS

// Intel's files use vaShared.hlsl's saturate alias in the R11G11B10 packer.
#define VA_SATURATE saturate

#include "XeGTAO.h"

cbuffer GTAOConstantBuffer : register( b0 )
{
    GTAOConstants g_GTAOConsts;
}

// Bindless heap indices for whichever pass is running. Not every pass uses every field; the host
// (D3D12GTAO.cpp) documents which it fills per dispatch and leaves the rest at 0xFFFFFFFF.
cbuffer GTAOBindingsCB : register( b1 )
{
    uint g_RawDepthIndex;       // Texture2D<float> : previous-frame depth snapshot, reversed-Z NDC (R32_FLOAT)
    uint g_WorkingDepthIndex;   // Texture2D<float> : view-space depth + 4 MIPs (R32_FLOAT), from the prefilter
    uint g_NormalsIndex;        // Texture2D<uint>  : view-space normals packed R11G11B10_UNORM (R32_UINT)
    uint g_EdgesIndex;          // Texture2D<float> : packed depth edges (R8_UNORM), from the main pass
    uint g_AOTermIndex;         // Texture2D<uint>  : working AO term (R8_UINT), from the main/denoise pass
    uint g_Out0Index;           // primary UAV: depth MIP0 / normals / AO term / denoised AO term
    uint g_Out1Index;           // secondary UAV: depth MIP1 / edges
    uint g_Out2Index;           // depth MIP2
    uint g_Out3Index;           // depth MIP3
    uint g_Out4Index;           // depth MIP4
    uint g_Pad0;
    uint g_Pad1;
};

// Point-clamp. XeGTAO gathers depth quads and samples explicit MIP levels; any filtering would blend across
// depth discontinuities and (per Intel's note in the main pass) interpolate neighbouring depths within a MIP.
SamplerState g_samplerPointClamp : register( s0 );

#include "XeGTAO.hlsli"

// Engine-specific normal loader. The normals are GENERATED from the same depth snapshot by CSGenerateNormals
// below, so they are already in the view space of that snapshot and need no basis change here.
//
// Deliberately not the depth-prepass normal G-buffer (D3D12Motion.cpp), even though it exists and carries real
// per-vertex normals: that buffer covers only the prepass writers, so grass, foliage and water would read the
// sentinel, and it belongs to THIS frame's camera while everything else in this pass belongs to the previous
// one's. Depth-derived normals are consistent with the depth by construction.
lpfloat3 LoadNormal( int2 pos )
{
    Texture2D<uint> srcNormalmap = ResourceDescriptorHeap[g_NormalsIndex];
    uint packedInput = srcNormalmap.Load( int3( pos, 0 ) ).x;
    float3 unpackedOutput = XeGTAO_R11G11B10_UNORM_to_FLOAT3( packedInput );
    return (lpfloat3)normalize( unpackedOutput * 2.0.xxx - 1.0.xxx );
}

// Hilbert-curve-driven R2 sequence, generated in place rather than from a 64x64 lookup texture (Intel ships
// both; the LUT saves a handful of ALU ops and costs a resource + a heap slot). NoiseIndex is the frame counter
// mod 64 when TAA is on and 0 otherwise, so the spatial dither only rotates temporally when something is
// actually accumulating it.
lpfloat2 SpatioTemporalNoise( uint2 pixCoord, uint temporalIndex )
{
    uint index = HilbertIndex( pixCoord.x, pixCoord.y );
    index += 288 * ( temporalIndex % 64 );
    return lpfloat2( frac( 0.5 + index * float2( 0.75487766624669276005, 0.5698402909980532659114 ) ) );
}

// --- Pass 1: raw NDC depth -> view-space depth + 4 MIPs -------------------------------------------------------
// 8x8 threads, each handling a 2x2 block, so one group covers 16x16 pixels: dispatch (w+15)/16, (h+15)/16.
[numthreads( 8, 8, 1 )]
void CSPrefilterDepths16x16( uint2 dispatchThreadID : SV_DispatchThreadID, uint2 groupThreadID : SV_GroupThreadID )
{
    Texture2D<float>     srcRawDepth = ResourceDescriptorHeap[g_RawDepthIndex];
    RWTexture2D<lpfloat> outMip0     = ResourceDescriptorHeap[g_Out0Index];
    RWTexture2D<lpfloat> outMip1     = ResourceDescriptorHeap[g_Out1Index];
    RWTexture2D<lpfloat> outMip2     = ResourceDescriptorHeap[g_Out2Index];
    RWTexture2D<lpfloat> outMip3     = ResourceDescriptorHeap[g_Out3Index];
    RWTexture2D<lpfloat> outMip4     = ResourceDescriptorHeap[g_Out4Index];

    XeGTAO_PrefilterDepths16x16( dispatchThreadID, groupThreadID, g_GTAOConsts, srcRawDepth,
        g_samplerPointClamp, outMip0, outMip1, outMip2, outMip3, outMip4 );
}

// --- Pass 2: view-space normals from the same depth snapshot --------------------------------------------------
[numthreads( XE_GTAO_NUMTHREADS_X, XE_GTAO_NUMTHREADS_Y, 1 )]
void CSGenerateNormals( const uint2 pixCoord : SV_DispatchThreadID )
{
    Texture2D<float>  srcRawDepth  = ResourceDescriptorHeap[g_RawDepthIndex];
    RWTexture2D<uint> outNormalmap = ResourceDescriptorHeap[g_Out0Index];

    float3 viewspaceNormal = XeGTAO_ComputeViewspaceNormal( pixCoord, g_GTAOConsts, srcRawDepth, g_samplerPointClamp );
    outNormalmap[pixCoord] = XeGTAO_FLOAT3_to_R11G11B10_UNORM( saturate( viewspaceNormal * 0.5 + 0.5 ) );
}

// --- Pass 3: the GTAO integral, one entry point per quality level ---------------------------------------------
// (sliceCount, stepsPerSlice) are Intel's tuned pairs: Low 1x2, Medium 2x2, High 3x3, Ultra 9x3.
#define XE_GTAO_MAIN_PASS_BODY( sliceCount, stepsPerSlice )                                                 \
    Texture2D<lpfloat>          srcWorkingDepth = ResourceDescriptorHeap[g_WorkingDepthIndex];               \
    RWTexture2D<uint>           outWorkingAOTerm = ResourceDescriptorHeap[g_Out0Index];                      \
    RWTexture2D<unorm float>    outWorkingEdges = ResourceDescriptorHeap[g_Out1Index];                       \
    XeGTAO_MainPass( pixCoord, sliceCount, stepsPerSlice,                                                   \
        SpatioTemporalNoise( pixCoord, g_GTAOConsts.NoiseIndex ), LoadNormal( pixCoord ),                    \
        g_GTAOConsts, srcWorkingDepth, g_samplerPointClamp, outWorkingAOTerm, outWorkingEdges )

[numthreads( XE_GTAO_NUMTHREADS_X, XE_GTAO_NUMTHREADS_Y, 1 )]
void CSGTAOLow( const uint2 pixCoord : SV_DispatchThreadID )    { XE_GTAO_MAIN_PASS_BODY( 1, 2 ); }

[numthreads( XE_GTAO_NUMTHREADS_X, XE_GTAO_NUMTHREADS_Y, 1 )]
void CSGTAOMedium( const uint2 pixCoord : SV_DispatchThreadID ) { XE_GTAO_MAIN_PASS_BODY( 2, 2 ); }

[numthreads( XE_GTAO_NUMTHREADS_X, XE_GTAO_NUMTHREADS_Y, 1 )]
void CSGTAOHigh( const uint2 pixCoord : SV_DispatchThreadID )   { XE_GTAO_MAIN_PASS_BODY( 3, 3 ); }

[numthreads( XE_GTAO_NUMTHREADS_X, XE_GTAO_NUMTHREADS_Y, 1 )]
void CSGTAOUltra( const uint2 pixCoord : SV_DispatchThreadID )  { XE_GTAO_MAIN_PASS_BODY( 9, 3 ); }

// --- Pass 4: edge-aware denoise -------------------------------------------------------------------------------
// Each thread does 2 horizontal pixels, so dispatch (w + 2*8-1) / (2*8), (h+7)/8. The LAST pass differs only in
// re-applying XE_GTAO_OCCLUSION_TERM_SCALE, which is why it needs its own entry point: the working term is
// stored pre-divided by that scale so the pre-denoise value can overshoot 1 without clipping in UNORM.
#define XE_GTAO_DENOISE_BODY( finalApply )                                                                  \
    const uint2 pixCoordBase = dispatchThreadID * uint2( 2, 1 );                                            \
    Texture2D<uint>     srcWorkingAOTerm = ResourceDescriptorHeap[g_AOTermIndex];                            \
    Texture2D<lpfloat>  srcWorkingEdges = ResourceDescriptorHeap[g_EdgesIndex];                              \
    RWTexture2D<uint>   outFinalAOTerm = ResourceDescriptorHeap[g_Out0Index];                                \
    XeGTAO_Denoise( pixCoordBase, g_GTAOConsts, srcWorkingAOTerm, srcWorkingEdges,                          \
        g_samplerPointClamp, outFinalAOTerm, finalApply )

[numthreads( XE_GTAO_NUMTHREADS_X, XE_GTAO_NUMTHREADS_Y, 1 )]
void CSDenoisePass( const uint2 dispatchThreadID : SV_DispatchThreadID )     { XE_GTAO_DENOISE_BODY( false ); }

[numthreads( XE_GTAO_NUMTHREADS_X, XE_GTAO_NUMTHREADS_Y, 1 )]
void CSDenoiseLastPass( const uint2 dispatchThreadID : SV_DispatchThreadID ) { XE_GTAO_DENOISE_BODY( true ); }
