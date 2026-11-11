#include "../pch.h"
#include "D3D12VertexPullSelfTest.h"
#include "D3D12GraphicsEngine.h"
#include "D3D12RootLayout.h"
#include "D3D12ShaderBackend.h"

using Microsoft::WRL::ComPtr;

namespace {
    constexpr UINT kBaseVertex = 5, kStartInstance = 7;
    constexpr UINT kGiveUpFrames = 240;   // the readback should land within a few frames
}

void D3D12VertexPullSelfTest::Run() {
    if ( m_State == State::Pending ) { Poll(); return; }
    if ( m_State == State::Done || !E.m_FrameOpen || !E.m_CmdList ) return;
    m_State = State::Done;   // one attempt only; any failure below logs and skips

    Rhi::Device* device = E.GetRhi();
    auto fail = []( const char* what ) { Logging::Wrn( "Vertex pulling self-test skipped: {}.", what ); };

    D3D12RootLayout layout;
    layout.Reset( "VertexPullSelfTest" );
    if ( !layout.Build( device ) ) return fail( "root signature" );

    ComPtr<ID3DBlob> vs, ps;
    if ( !E.m_ShaderBackend.CompileFromFile( "VertexPullSelfTest.hlsl", "VSMain", "vs_6_6", vs.GetAddressOf() )
        || !E.m_ShaderBackend.CompileFromFile( "VertexPullSelfTest.hlsl", "PSMain", "ps_6_6", ps.GetAddressOf() ) )
        return fail( "shader compile" );

    Rhi::GraphicsPipelineStateDesc pso = {};
    pso.pRootSignature = layout.Get();
    pso.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    pso.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = DXGI_FORMAT_R32G32_UINT;
    pso.SampleDesc.Count = 1;
    pso.SampleMask = UINT_MAX;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable = TRUE;
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    ComPtr<Rhi::PipelineState> state;
    if ( FAILED( device->CreateGraphicsPipelineState( &pso, state.GetAddressOf() ) ) ) return fail( "pipeline state" );

    D3D12_RESOURCE_DESC td = {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = 2;
    td.Height = 1;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R32G32_UINT;
    td.SampleDesc.Count = 1;
    td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ComPtr<Rhi::Resource> tex;
    if ( FAILED( device->CreateResource( D3D12_HEAP_TYPE_DEFAULT, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
            tex.GetAddressOf(), Rhi::RESOURCE_FLAG_TRACK_LAYOUT ) ) ) return fail( "render target" );
    tex->SetName( L"VertexPullSelfTestRT" );

    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.NumDescriptors = 1;
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    ComPtr<Rhi::DescriptorHeap> rtvHeap;
    if ( FAILED( device->CreateDescriptorHeap( &hd, rtvHeap.GetAddressOf() ) ) ) return fail( "RTV heap" );
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
    device->CreateRenderTargetView( tex.Get(), nullptr, rtv );

    // R16 index buffer {0,1,2}, padded so the view sits well inside it.
    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = 256;
    bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<Rhi::Resource> ib;
    if ( FAILED( device->CreateResource( D3D12_HEAP_TYPE_UPLOAD, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, ib.GetAddressOf() ) ) )
        return fail( "index buffer" );
    void* mapped = nullptr;
    if ( FAILED( ib->Map( 0, nullptr, &mapped ) ) || !mapped ) return fail( "index buffer map" );
    const uint16_t indices[3] = { 0, 1, 2 };
    memcpy( mapped, indices, sizeof( indices ) );
    ib->Unmap( 0, nullptr );

    UINT numRows = 0;
    UINT64 rowSize = 0, totalBytes = 0;
    device->GetCopyableFootprints( &td, 0, 1, 0, &m_Footprint, &numRows, &rowSize, &totalBytes );
    D3D12_RESOURCE_DESC rd = bd;
    rd.Width = totalBytes;
    if ( FAILED( device->CreateResource( D3D12_HEAP_TYPE_READBACK, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, m_Readback.ReleaseAndGetAddressOf() ) ) )
        return fail( "readback buffer" );

    D3D12CmdList& cmd = E.m_CmdList;
    const FLOAT zero[4] = {};
    cmd->OMSetRenderTargets( 1, &rtv, FALSE, nullptr );
    cmd->ClearRenderTargetView( rtv, zero, 0, nullptr );
    cmd->SetGraphicsRootSignature( layout.Get() );
    cmd->SetPipelineState( state.Get() );
    cmd->IASetPrimitiveTopology( D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST );
    const D3D12_INDEX_BUFFER_VIEW ibv = { ib->GetGPUVirtualAddress(), sizeof( indices ), DXGI_FORMAT_R16_UINT };
    cmd->IASetIndexBuffer( &ibv );
    D3D12_VIEWPORT vp = { 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f };
    const D3D12_RECT sc = { 0, 0, 2, 1 };
    cmd->RSSetScissorRects( 1, &sc );
    cmd->RSSetViewports( 1, &vp );
    cmd->DrawIndexedInstanced( 3, 1, 0, static_cast<INT>( kBaseVertex ), kStartInstance );
    vp.TopLeftX = 1.0f;
    cmd->RSSetViewports( 1, &vp );
    cmd->DrawInstanced( 3, 1, kBaseVertex, kStartInstance );

    cmd->TransitionBarrier( tex.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE );
    Rhi::TextureCopyLocation dst = {}, src = {};
    dst.pResource = m_Readback.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = m_Footprint;
    src.pResource = tex.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    cmd->CopyTextureRegion( &dst, 0, 0, 0, &src, nullptr );

    // Viewport/scissor go back as OnBeginFrame left them; the caller's next pass rebinds its own RT.
    cmd->RSSetViewports( 1, &E.m_CurrentViewport );
    cmd->RSSetScissorRects( 1, &E.m_CurrentScissor );
    cmd.InvalidateRenderTargets();

    E.QueueCleanupJob( [tex = std::move( tex ), ib = std::move( ib ), rtvHeap = std::move( rtvHeap ),
        state = std::move( state ), root = layout.RootSig()]() mutable {} );
    m_FramesWaited = 0;
    m_State = State::Pending;
}

void D3D12VertexPullSelfTest::Poll() {
    // Wait out the frames in flight, then until the direct queue has drained past the copy.
    if ( ++m_FramesWaited < D3D12GraphicsEngine::kBackBufferMax + 1 ) return;
    if ( !E.GpuCaughtUp() ) {
        if ( m_FramesWaited > kGiveUpFrames ) {
            Logging::Wrn( "Vertex pulling self-test: GPU never caught up; no result." );
            m_Readback.Reset();
            m_State = State::Done;
        }
        return;
    }

    uint32_t px[4] = {};   // draw 0 (indexed) vid/iid, then draw 1 (non-indexed)
    void* mapped = nullptr;
    const D3D12_RANGE range = { 0, static_cast<SIZE_T>( m_Footprint.Offset + sizeof( px ) ) };
    if ( SUCCEEDED( m_Readback->Map( 0, &range, &mapped ) ) && mapped ) {
        memcpy( px, static_cast<const uint8_t*>( mapped ) + m_Footprint.Offset, sizeof( px ) );
        m_Readback->Unmap( 0, nullptr );
    } else {
        Logging::Wrn( "Vertex pulling self-test: readback map failed." );
    }
    m_Readback.Reset();   // the GPU is past the copy
    m_State = State::Done;

    if ( !px[0] || !px[2] ) {   // the PS stores id + 1; zero is the clear value
        Logging::Wrn( "Vertex pulling self-test: a draw did not write its pixel (indexed {}, non-indexed {}).", px[0] != 0, px[2] != 0 );
        return;
    }
    const uint32_t ivid = px[0] - 1, iiid = px[1] - 1, nvid = px[2] - 1, niid = px[3] - 1;
    Logging::Inf( "Vertex pulling self-test: indexed vid={} iid={}, non-indexed vid={} iid={} -> VertexID includes base: {}, InstanceID includes start: {}",
        ivid, iiid, nvid, niid, ivid == kBaseVertex ? "yes" : "no", iiid == kStartInstance ? "yes" : "no" );
    if ( ivid != 0 || iiid != 0 )
        Logging::Wrn( "Vertex pulling self-test: indexed draw saw vid={} iid={}, expected 0/0; the base-vertex/instance assumptions do not hold on this driver.", ivid, iiid );
}
