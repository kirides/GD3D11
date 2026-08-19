#include "../pch.h"
#include "D3D12TexturePool.h"
#include "D3D12GraphicsEngine.h"
#include "../Logger.h"
#include <algorithm>

bool D3D12TexturePool::Attach( D3D12GraphicsEngine& engine ) {
    m_Engine = &engine;
    m_Device = engine.GetD3DDevice();
    m_Allocator = engine.GetAllocator();
    if ( !m_Device || !m_Allocator ) return false;
    return m_RtvHeap.Init( m_Device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kMaxPooledTargets, L"D3D12TexturePool_RTV" );
}

D3D12TexturePool::Handle D3D12TexturePool::Acquire( const Description& desc ) {
    PooledTexture* found = nullptr;
    for ( auto& entry : m_Pool ) {
        if ( !entry->InUse && entry->Desc == desc ) {
            found = entry.get();
            break;
        }
    }

    if ( !found ) {
        auto tex = std::make_unique<D3D12RenderTarget>();
        wchar_t name[64];
        swprintf_s( name, L"PooledRT_%zu", m_Pool.size() );
        if ( !tex->Init( m_Device, m_Allocator, m_Engine, &m_RtvHeap, desc.Width, desc.Height, desc.Format, desc.NeedsUav, name ) ) {
            LogWarn() << "D3D12TexturePool: could not create a pooled render target (" << desc.Width << "x" << desc.Height << ").";
            return Handle( nullptr );
        }
        m_Pool.push_back( std::make_unique<PooledTexture>( PooledTexture{ std::move( tex ), desc, m_CurrentFrame, true } ) );
        found = m_Pool.back().get();
    }

    found->InUse = true;
    found->LastFrameUsed = m_CurrentFrame;
    return Handle( found );
}

void D3D12TexturePool::GiveTick() {
    m_CurrentFrame++;
    m_Pool.erase( std::remove_if( m_Pool.begin(), m_Pool.end(), [this]( const auto& entry ) {
        return !entry->InUse && ( m_CurrentFrame - entry->LastFrameUsed > kMaxUnusedFrames );
        } ), m_Pool.end() );
}

void D3D12TexturePool::Clear() {
    m_Pool.clear();
}

size_t D3D12TexturePool::GetActiveCount() const {
    size_t count = 0;
    for ( const auto& e : m_Pool ) if ( e->InUse ) count++;
    return count;
}
