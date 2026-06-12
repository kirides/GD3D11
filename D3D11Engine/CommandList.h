#pragma once
#include "pch.h"
#include "GothicGraphicsState.h"

class CommandList {
public:
    typedef void( __cdecl* PFN_DRAWMULTIINDEXEDINSTANCEDINDIRECT )(ID3D11DeviceContext* context, unsigned int drawCount,
    ID3D11Buffer* buffer, unsigned int alignedByteOffsetForArgs, unsigned int alignedByteStrideForArgs);

    enum ConstantBufferVisibility {
        VS = 1 << 0,
        PS = 1 << 1,
        DS = 1 << 2,
        HS = 1 << 3,
        GS = 1 << 4,
        CS = 1 << 5,
    };

    CommandList( ID3D11DeviceContext* context,
        PFN_DRAWMULTIINDEXEDINSTANCEDINDIRECT mdiFunc,
        GothicPipelineStateInfo* pipelineState)
        : _context( context ),
        _mdiFunc( mdiFunc ),
        _pipelineState( pipelineState ) {
    }
    CommandList() = delete;

    void Draw(
        unsigned int VertexCount,
        unsigned int StartVertexLocation );

    void DrawIndexed(
        unsigned int IndexCount,
        unsigned int StartIndexLocation,
        INT BaseVertexLocation );

    void DrawIndexedInstanced(
        unsigned int IndexCountPerInstance,
        unsigned int InstanceCount,
        unsigned int StartIndexLocation,
        INT BaseVertexLocation,
        unsigned int StartInstanceLocation );

    void DrawIndexedInstancedIndirect(
        ID3D11Buffer* pBufferForArgs,
        unsigned int AlignedByteOffsetForArgs );

    void DrawInstanced(
        unsigned int VertexCountPerInstance,
        unsigned int InstanceCount,
        unsigned int StartVertexLocation,
        unsigned int StartInstanceLocation );

    void DrawInstancedIndirect(
        ID3D11Buffer* pBufferForArgs,
        unsigned int AlignedByteOffsetForArgs );

    void MultiDrawInstancedIndirect( unsigned int drawCount,
        ID3D11Buffer* buffer,
        unsigned int alignedByteOffsetForArgs,
        unsigned int alignedByteStrideForArgs );

    void Dispatch(
            unsigned int ThreadGroupCountX,
            unsigned int ThreadGroupCountY,
            unsigned int ThreadGroupCountZ );

    void DispatchIndirect(
            ID3D11Buffer* pBufferForArgs,
            unsigned int AlignedByteOffsetForArgs );

    void SetConstantBuffers( UINT StartSlot, UINT NumBuffers, ID3D11Buffer* const* ppConstantBuffers, ConstantBufferVisibility visibility );
private:
    ID3D11DeviceContext* _context;
    PFN_DRAWMULTIINDEXEDINSTANCEDINDIRECT _mdiFunc;
    GothicPipelineStateInfo* _pipelineState;
};
