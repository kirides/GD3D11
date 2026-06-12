#include "CommandList.h"

void CommandList::Draw( unsigned int VertexCount, unsigned int StartVertexLocation )
{
    _context->Draw( VertexCount, StartVertexLocation );
}

void CommandList::DrawIndexed( unsigned int IndexCount, unsigned int StartIndexLocation, INT BaseVertexLocation )
{
    _context->DrawIndexed( IndexCount, StartIndexLocation, BaseVertexLocation );
}

void CommandList::DrawIndexedInstanced( unsigned int IndexCountPerInstance, unsigned int InstanceCount, unsigned int StartIndexLocation, INT BaseVertexLocation, unsigned int StartInstanceLocation )
{
    _context->DrawIndexedInstanced( IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation, StartInstanceLocation );
}

void CommandList::DrawIndexedInstancedIndirect( ID3D11Buffer* pBufferForArgs, unsigned int AlignedByteOffsetForArgs )
{
    _context->DrawIndexedInstancedIndirect( pBufferForArgs, AlignedByteOffsetForArgs );
}

void CommandList::DrawInstanced( unsigned int VertexCountPerInstance, unsigned int InstanceCount, unsigned int StartVertexLocation, unsigned int StartInstanceLocation )
{
    _context->DrawInstanced( VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation );
}

void CommandList::DrawInstancedIndirect( ID3D11Buffer* pBufferForArgs, unsigned int AlignedByteOffsetForArgs )
{
    _context->DrawInstancedIndirect( pBufferForArgs, AlignedByteOffsetForArgs );
}

void CommandList::MultiDrawInstancedIndirect( unsigned int drawCount, ID3D11Buffer* buffer, unsigned int alignedByteOffsetForArgs, unsigned int alignedByteStrideForArgs )
{
    _mdiFunc( _context, drawCount, buffer, alignedByteOffsetForArgs, alignedByteStrideForArgs );
}
