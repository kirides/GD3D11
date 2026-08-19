#include "../pch.h"
#include "D3D12RenderTarget.h"
#include "D3D12GraphicsEngine.h"
#include "D3D12ResourceCreate.h"
#include "D3D12PooledDescriptorHeap.h"
#include "../Logger.h"

D3D12RenderTarget::~D3D12RenderTarget() {
    if ( m_Engine ) {
        if ( m_SrvSlot != 0xFFFFFFFFu ) m_Engine->FreeSrvSlot( m_SrvSlot );
        if ( m_UavSlot != 0xFFFFFFFFu ) m_Engine->FreeSrvSlot( m_UavSlot );
    }
    if ( m_RtvHeap && m_RtvSlot != 0xFFFFFFFFu ) m_RtvHeap->Free( m_RtvSlot );
}

bool D3D12RenderTarget::Init( ID3D12Device* device, D3D12MA::Allocator* allocator, D3D12GraphicsEngine* engine,
    D3D12PooledDescriptorHeap* rtvHeap, UINT width, UINT height, DXGI_FORMAT format, bool needsUav,
    const wchar_t* debugName ) {
    m_Engine = engine;
    m_RtvHeap = rtvHeap;

    D3D12_RESOURCE_DESC dd = {};
    dd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    dd.Width = width;
    dd.Height = height;
    dd.DepthOrArraySize = 1;
    dd.MipLevels = 1;
    dd.Format = format;
    dd.SampleDesc.Count = 1;
    dd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    dd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if ( needsUav ) dd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    D3D12_CLEAR_VALUE clear = {};
    clear.Format = format;

    D3D12MA::ALLOCATION_DESC allocDesc = {};
    allocDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;

    if ( FAILED( D3D12ResourceCreate::CreateTexture( allocator, allocDesc, dd, D3D12_RESOURCE_STATE_RENDER_TARGET,
        &clear, m_Allocation.ReleaseAndGetAddressOf(), IID_PPV_ARGS( m_Texture.ReleaseAndGetAddressOf() ) ) ) ) {
        LogWarn() << "D3D12TexturePool: failed to create a pooled render target (" << width << "x" << height << ").";
        return false;
    }
    if ( debugName ) m_Texture->SetName( debugName );

    m_RtvSlot = rtvHeap->Allocate();
    if ( m_RtvSlot == 0xFFFFFFFFu ) return false;
    m_Rtv = rtvHeap->GetCpuHandle( m_RtvSlot );
    device->CreateRenderTargetView( m_Texture.Get(), nullptr, m_Rtv );

    m_SrvSlot = engine->AllocateSrvSlot();
    if ( m_SrvSlot == 0xFFFFFFFFu ) return false;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Format = format;
    srv.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView( m_Texture.Get(), &srv, engine->GetSrvCpuHandle( m_SrvSlot ) );

    if ( needsUav ) {
        m_UavSlot = engine->AllocateSrvSlot();
        if ( m_UavSlot == 0xFFFFFFFFu ) return false;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        uav.Format = format;
        device->CreateUnorderedAccessView( m_Texture.Get(), nullptr, &uav, engine->GetSrvCpuHandle( m_UavSlot ) );
    }

    m_Width = width;
    m_Height = height;
    m_Format = format;
    State = D3D12_RESOURCE_STATE_RENDER_TARGET;
    return true;
}
