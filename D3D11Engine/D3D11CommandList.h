#pragma once
#include "pch.h"
#include "D3D11PipelineStateObject.h"

class D3D11VertexBuffer;

/**
 * Slim command-list wrapper around an ID3D11DeviceContext1 and the
 * D3D11PipelineStateCache.
 *
 * Provides SetPipelineState() plus the commonly used Draw / IA / OM
 * helpers so that call-sites read like a modern graphics API without
 * touching the raw context or the engine's global render-state machine.
 *
 * The object is intentionally cheap to construct (two pointers) and
 * does not own any resources.
 */
struct D3D11CommandList {

    D3D11CommandList() = default;
    D3D11CommandList( ID3D11DeviceContext* context, D3D11PipelineStateCache* cache )
        : m_Context( context ), m_Cache( cache ) {}

    // --- Pipeline state ------------------------------------------------------

    void SetPipelineState( const D3D11PipelineStateObject& pso ) {
        m_Cache->SetPipelineState( pso );
    }

    /** Force the cache to re-bind everything on next SetPipelineState. */
    void InvalidatePipelineState() {
        m_Cache->Invalidate();
    }

    // --- Input assembly ------------------------------------------------------

    void IASetVertexBuffer( D3D11VertexBuffer* vb, UINT stride, UINT offset = 0 );

    void IASetVertexBuffers( UINT startSlot,
                             UINT numBuffers,
                             ID3D11Buffer* const* buffers,
                             const UINT* strides,
                             const UINT* offsets ) {
        m_Context->IASetVertexBuffers( startSlot, numBuffers, buffers, strides, offsets );
    }

    void IASetIndexBuffer( ID3D11Buffer* buffer, DXGI_FORMAT format, UINT offset = 0 ) {
        m_Context->IASetIndexBuffer( buffer, format, offset );
    }

    void IASetIndexBuffer( D3D11VertexBuffer* ib, DXGI_FORMAT format, UINT offset = 0 );

    // --- Draw calls ----------------------------------------------------------

    void Draw( UINT vertexCount, UINT startVertexLocation = 0 ) {
        m_Context->Draw( vertexCount, startVertexLocation );
        m_DrawnTriangles += vertexCount / 3;
    }

    void DrawIndexed( UINT indexCount,
                      UINT startIndexLocation = 0,
                      INT  baseVertexLocation  = 0 ) {
        m_Context->DrawIndexed( indexCount, startIndexLocation, baseVertexLocation );
        m_DrawnTriangles += indexCount / 3;
    }

    void DrawInstanced( UINT vertexCountPerInstance,
                        UINT instanceCount,
                        UINT startVertexLocation    = 0,
                        UINT startInstanceLocation  = 0 ) {
        m_Context->DrawInstanced( vertexCountPerInstance, instanceCount,
                                  startVertexLocation, startInstanceLocation );
        m_DrawnTriangles += ( vertexCountPerInstance / 3 ) * instanceCount;
    }

    void DrawIndexedInstanced( UINT indexCountPerInstance,
                               UINT instanceCount,
                               UINT startIndexLocation   = 0,
                               INT  baseVertexLocation    = 0,
                               UINT startInstanceLocation = 0 ) {
        m_Context->DrawIndexedInstanced( indexCountPerInstance, instanceCount,
                                         startIndexLocation, baseVertexLocation,
                                         startInstanceLocation );
        m_DrawnTriangles += ( indexCountPerInstance / 3 ) * instanceCount;
    }

    void DrawIndexedInstancedIndirect( ID3D11Buffer* argsBuffer,
                                       UINT alignedByteOffsetForArgs ) {
        m_Context->DrawIndexedInstancedIndirect( argsBuffer, alignedByteOffsetForArgs );
        // Triangle count unknown for indirect draws
    }

    // --- Render target / viewport helpers ------------------------------------

    void OMSetRenderTargets( UINT numViews,
                             ID3D11RenderTargetView* const* rtvs,
                             ID3D11DepthStencilView* dsv ) {
        m_Context->OMSetRenderTargets( numViews, rtvs, dsv );
    }

    void RSSetViewports( UINT numViewports, const D3D11_VIEWPORT* viewports ) {
        m_Context->RSSetViewports( numViewports, viewports );
    }

    void RSGetViewports( UINT* numViewports, D3D11_VIEWPORT* viewports ) {
        m_Context->RSGetViewports( numViewports, viewports );
    }

    void ClearDepthStencilView( ID3D11DepthStencilView* dsv,
                                UINT clearFlags,
                                float depth,
                                UINT8 stencil ) {
        m_Context->ClearDepthStencilView( dsv, clearFlags, depth, stencil );
    }

    void ClearRenderTargetView( ID3D11RenderTargetView* rtv, const float color[4] ) {
        m_Context->ClearRenderTargetView( rtv, color );
    }

    // --- Stats ---------------------------------------------------------------

    /** Return triangles drawn since last ResetStats() and reset counter. */
    UINT FlushDrawnTriangles() {
        UINT t = m_DrawnTriangles;
        m_DrawnTriangles = 0;
        return t;
    }

    UINT GetDrawnTriangles() const { return m_DrawnTriangles; }

    // --- Raw access (escape hatch) -------------------------------------------

    ID3D11DeviceContext*    GetContext() const { return m_Context; }
    D3D11PipelineStateCache* GetPSOCache()   const { return m_Cache; }

private:
    ID3D11DeviceContext*    m_Context        = nullptr;
    D3D11PipelineStateCache* m_Cache          = nullptr;
    UINT                     m_DrawnTriangles = 0;
};
