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

bool D3D12RenderTarget::CreateViews( ID3D12Device* device, UINT width, UINT height, DXGI_FORMAT format, bool needsUav ) {
    if ( m_RtvSlot == 0xFFFFFFFFu ) {
        m_RtvSlot = m_RtvHeap->Allocate();
        if ( m_RtvSlot == 0xFFFFFFFFu ) return false;
        m_Rtv = m_RtvHeap->GetCpuHandle( m_RtvSlot );
    }
    device->CreateRenderTargetView( m_Texture.Get(), nullptr, m_Rtv );

    if ( m_SrvSlot == 0xFFFFFFFFu ) {
        m_SrvSlot = m_Engine->AllocateSrvSlot();
        if ( m_SrvSlot == 0xFFFFFFFFu ) return false;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Format = format;
    srv.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView( m_Texture.Get(), &srv, m_Engine->GetSrvCpuHandle( m_SrvSlot ) );

    if ( needsUav ) {
        if ( m_UavSlot == 0xFFFFFFFFu ) {
            m_UavSlot = m_Engine->AllocateSrvSlot();
            if ( m_UavSlot == 0xFFFFFFFFu ) return false;
        }
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        uav.Format = format;
        device->CreateUnorderedAccessView( m_Texture.Get(), nullptr, &uav, m_Engine->GetSrvCpuHandle( m_UavSlot ) );
    }

    m_Width = width;
    m_Height = height;
    m_Format = format;
    State = D3D12_RESOURCE_STATE_RENDER_TARGET;
    return true;
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

    return CreateViews( device, width, height, format, needsUav );
}

bool D3D12RenderTarget::InitPlaced( ID3D12Device* device, ID3D12Resource* resource, D3D12GraphicsEngine* engine,
    D3D12PooledDescriptorHeap* rtvHeap, UINT width, UINT height, DXGI_FORMAT format, bool needsUav,
    const wchar_t* debugName ) {
    m_Engine = engine;
    m_RtvHeap = rtvHeap;
    m_Texture = resource;   // ComPtr::operator=(raw ptr) AddRefs; the arena's local ComPtr still owns its own ref
    if ( debugName ) m_Texture->SetName( debugName );

    return CreateViews( device, width, height, format, needsUav );
}

bool D3D12RenderTarget::ReplaceResource( ID3D12Device* device, ID3D12Resource* newResource, UINT width, UINT height,
    DXGI_FORMAT format, bool needsUav ) {
    // The RTV slot is reused in place (device->CreateRenderTargetView below overwrites its CPU-side
    // descriptor content). That is safe unconditionally: an OM render-target bind snapshots the
    // descriptor's CONTENTS into the command list at RECORD time (see D3D12StateCache.h's
    // InvalidateRenderTargets comment), so an earlier, not-yet-executed pass that already recorded an
    // OMSetRenderTargets against this handle is unaffected by us rewriting it now.
    //
    // The SRV/UAV slot(s) are the opposite: a bindless descriptor is read by the GPU at DRAW time, not
    // snapshotted at record time, so the OLD resource + its SRV/UAV slot(s) must survive (and their
    // slots stay unavailable for reuse) until the GPU has actually finished whatever earlier-recorded
    // draws might still read them — QueueSrvResourceForRelease defers exactly that.
    if ( m_Texture ) {
        if ( m_SrvSlot != 0xFFFFFFFFu ) m_Engine->QueueSrvResourceForRelease( m_SrvSlot, m_Texture );
        if ( m_UavSlot != 0xFFFFFFFFu ) m_Engine->QueueSrvResourceForRelease( m_UavSlot, m_Texture );
    }
    m_SrvSlot = 0xFFFFFFFFu;
    m_UavSlot = 0xFFFFFFFFu;
    m_Texture = newResource;

    return CreateViews( device, width, height, format, needsUav );
}
