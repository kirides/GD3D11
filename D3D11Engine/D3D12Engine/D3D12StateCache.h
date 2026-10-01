#pragma once
#include "../RHI/RhiCmdList.h"
#include "D3D12Barrier.h"

// The D3D12 renderer's command lists go through the backend-neutral redundant-state filter.
using D3D12CmdList = Rhi::CmdList;

/** Submits a command set partitioned opaque-first: [0, opaque) through `noAlpha`, the rest through `clip`.
    A null `noAlpha` draws everything through `clip`. */
inline void ExecuteIndirectAlphaSplit( D3D12CmdList& cl, Rhi::CommandSignature* sig, Rhi::Resource* args, UINT64 argOffset,
    UINT stride, UINT total, UINT opaque, Rhi::PipelineState* clip, Rhi::PipelineState* noAlpha ) {
    if ( !noAlpha ) opaque = 0;
    if ( opaque > 0 ) {
        cl.SetPipelineState( noAlpha );
        cl.ExecuteIndirect( sig, opaque, args, argOffset, nullptr, 0 );
    }
    if ( total > opaque ) {
        cl.SetPipelineState( clip );
        cl.ExecuteIndirect( sig, total - opaque, args, argOffset + static_cast<UINT64>( opaque ) * stride, nullptr, 0 );
    }
}
