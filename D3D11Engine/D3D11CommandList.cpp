#include "pch.h"
#include "D3D11CommandList.h"
#include "D3D11VertexBuffer.h"

void D3D11CommandList::IASetVertexBuffer( D3D11VertexBuffer* vb, UINT stride, UINT offset ) {
    m_Context->IASetVertexBuffers( 0, 1, vb->GetVertexBuffer().GetAddressOf(), &stride, &offset );
}

void D3D11CommandList::IASetIndexBuffer( D3D11VertexBuffer* ib, DXGI_FORMAT format, UINT offset ) {
    m_Context->IASetIndexBuffer( ib->GetVertexBuffer().Get(), format, offset );
}
