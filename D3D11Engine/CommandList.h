#pragma once
#include "pch.h"

class CommandList {
public:
    typedef void( __cdecl* PFN_DRAWMULTIINDEXEDINSTANCEDINDIRECT )(ID3D11DeviceContext* context, unsigned int drawCount,
    ID3D11Buffer* buffer, unsigned int alignedByteOffsetForArgs, unsigned int alignedByteStrideForArgs);

    CommandList( ID3D11DeviceContext* context,
        PFN_DRAWMULTIINDEXEDINSTANCEDINDIRECT mdiFunc )
        : _context( context ),
        _mdiFunc( mdiFunc ) {
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
private:
    ID3D11DeviceContext* _context;
    PFN_DRAWMULTIINDEXEDINSTANCEDINDIRECT _mdiFunc;
};
