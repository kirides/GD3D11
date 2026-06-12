#include "CommandList.h"

void CommandList::Draw( unsigned int VertexCount, unsigned int StartVertexLocation )
{
    _pipelineState->Apply();
    _context->Draw( VertexCount, StartVertexLocation );
}

void CommandList::DrawIndexed( unsigned int IndexCount, unsigned int StartIndexLocation, INT BaseVertexLocation )
{
    _pipelineState->Apply();
    _context->DrawIndexed( IndexCount, StartIndexLocation, BaseVertexLocation );
}

void CommandList::DrawIndexedInstanced( unsigned int IndexCountPerInstance, unsigned int InstanceCount, unsigned int StartIndexLocation, INT BaseVertexLocation, unsigned int StartInstanceLocation )
{
    _pipelineState->Apply();
    _context->DrawIndexedInstanced( IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation, StartInstanceLocation );
}

void CommandList::DrawIndexedInstancedIndirect( ID3D11Buffer* pBufferForArgs, unsigned int AlignedByteOffsetForArgs )
{
    _pipelineState->Apply();
    _context->DrawIndexedInstancedIndirect( pBufferForArgs, AlignedByteOffsetForArgs );
}

void CommandList::DrawInstanced( unsigned int VertexCountPerInstance, unsigned int InstanceCount, unsigned int StartVertexLocation, unsigned int StartInstanceLocation )
{
    _pipelineState->Apply();
    _context->DrawInstanced( VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation );
}

void CommandList::DrawInstancedIndirect( ID3D11Buffer* pBufferForArgs, unsigned int AlignedByteOffsetForArgs )
{
    _pipelineState->Apply();
    _context->DrawInstancedIndirect( pBufferForArgs, AlignedByteOffsetForArgs );
}

void CommandList::MultiDrawInstancedIndirect( unsigned int drawCount, ID3D11Buffer* buffer, unsigned int alignedByteOffsetForArgs, unsigned int alignedByteStrideForArgs )
{
    _pipelineState->Apply();
    _mdiFunc( _context, drawCount, buffer, alignedByteOffsetForArgs, alignedByteStrideForArgs );
}

void CommandList::Dispatch( unsigned int ThreadGroupCountX, unsigned int ThreadGroupCountY, unsigned int ThreadGroupCountZ )
{
    _pipelineState->Apply();
    _context->Dispatch( ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ );
}

void CommandList::DispatchIndirect( ID3D11Buffer* pBufferForArgs, unsigned int AlignedByteOffsetForArgs )
{
    _pipelineState->Apply();
    _context->DispatchIndirect( pBufferForArgs, AlignedByteOffsetForArgs );
}

void CommandList::SetConstantBuffers( UINT StartSlot, UINT NumBuffers, ID3D11Buffer* const* ppConstantBuffers, ConstantBufferVisibility visibility )
{
    if ( visibility & ConstantBufferVisibility::VS ) {
        _context->VSSetConstantBuffers( StartSlot, NumBuffers, ppConstantBuffers );
    }
    if ( visibility & ConstantBufferVisibility::PS ) {
        _context->PSSetConstantBuffers( StartSlot, NumBuffers, ppConstantBuffers );
    }
    if ( visibility & ConstantBufferVisibility::DS ) {
        _context->DSSetConstantBuffers( StartSlot, NumBuffers, ppConstantBuffers );
    }
    if ( visibility & ConstantBufferVisibility::HS ) {
        _context->HSSetConstantBuffers( StartSlot, NumBuffers, ppConstantBuffers );
    }
    if ( visibility & ConstantBufferVisibility::GS ) {
        _context->GSSetConstantBuffers( StartSlot, NumBuffers, ppConstantBuffers );
    }
    if ( visibility & ConstantBufferVisibility::CS ) {
        _context->CSSetConstantBuffers( StartSlot, NumBuffers, ppConstantBuffers );
    }
}
