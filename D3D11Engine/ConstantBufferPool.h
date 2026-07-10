#pragma once
#include <wrl/client.h>
#include <d3d11_1.h>
#include <cstdint>

struct ConstantBufferAllocation {
    ID3D11Buffer* pBuffer = nullptr;
    uint32_t offsetInBytes = 0;
    uint32_t sizeInBytes = 0;

    bool operator==( const ConstantBufferAllocation& other ) const {
        return pBuffer == other.pBuffer && offsetInBytes == other.offsetInBytes && sizeInBytes == other.sizeInBytes;
    }
};

class ConstantBufferPool {
private:
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_poolBuffer;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext1> m_Context;
    uint32_t m_bufferSize;
    uint32_t m_currentOffset;
    bool m_firstMapThisFrame = true; // first Map of the frame gets DISCARD; the rest NO_OVERWRITE
    bool m_wrapWarned = false;        // warn only once if the ring wraps mid-frame

public:
    void Initialize( ID3D11Device* device, uint32_t totalSizeInBytes = 4 * 1024 * 1024 ) {
        m_bufferSize = totalSizeInBytes;
        m_currentOffset = 0;
        m_firstMapThisFrame = true;
        m_wrapWarned = false;

        D3D11_BUFFER_DESC desc = {};
        desc.ByteWidth = m_bufferSize;
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

        device->CreateBuffer( &desc, nullptr, &m_poolBuffer );
        
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
        device->GetImmediateContext(&context);
        context.As(&m_Context);
//#ifdef DEBUG_D3D11
//        SetDebugName( m_poolBuffer.Get(), std::string( "ConstantBufferPool (size:" ) + std::to_string( totalSizeInBytes ) + ")" );
//#endif
    }

    void BeginFrame();
    ConstantBufferAllocation Allocate( const void* pData, uint32_t sizeInBytes );
    void BindPS( uint32_t slot, const ConstantBufferAllocation& allocation );
    void BindVS( uint32_t slot, const ConstantBufferAllocation& allocation );
    void BindCS( uint32_t slot, const ConstantBufferAllocation& allocation );
    void BindGS( uint32_t slot, const ConstantBufferAllocation& allocation );
    void BindDS( uint32_t slot, const ConstantBufferAllocation& allocation );
    void BindHS( uint32_t slot, const ConstantBufferAllocation& allocation );
    void EndFrame();

    ID3D11Buffer* GetBuffer() const { return m_poolBuffer.Get(); }
};

