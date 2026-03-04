#pragma once

#include "pch.h"
#include <wrl/client.h>

// Templated structured buffer for GPU compute/shader access
template<typename T>
class D3D11StructuredBuffer {
public:
    D3D11StructuredBuffer() : ElementCount( 0 ), MaxElementCount( 0 ) {}

    ~D3D11StructuredBuffer() = default;

    // Initialize the buffer with a maximum capacity
    HRESULT Init( ID3D11Device* device, UINT maxElements, bool cpuWrite = true, bool gpuWrite = false ) {
        MaxElementCount = maxElements;
        ElementCount = 0;

        D3D11_BUFFER_DESC desc = {};
        desc.ByteWidth = sizeof( T ) * maxElements;
        desc.StructureByteStride = sizeof( T );
        desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;

        if ( cpuWrite ) {
            desc.Usage = D3D11_USAGE_DYNAMIC;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        } else if ( gpuWrite ) {
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.CPUAccessFlags = 0;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE 
                | (device->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_0 ? D3D11_BIND_UNORDERED_ACCESS : 0);
        } else {
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.CPUAccessFlags = 0;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        }

        HRESULT hr = device->CreateBuffer( &desc, nullptr, Buffer.GetAddressOf() );
        if ( FAILED( hr ) ) return hr;

        // Create SRV
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = DXGI_FORMAT_UNKNOWN;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        srvDesc.Buffer.FirstElement = 0;
        srvDesc.Buffer.NumElements = maxElements;

        hr = device->CreateShaderResourceView( Buffer.Get(), &srvDesc, SRV.GetAddressOf() );
        if ( FAILED( hr ) ) return hr;

        // Create UAV if GPU writable
        if ( gpuWrite ) {
            D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
            uavDesc.Format = DXGI_FORMAT_UNKNOWN;
            uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
            uavDesc.Buffer.FirstElement = 0;
            uavDesc.Buffer.NumElements = maxElements;

            hr = device->CreateUnorderedAccessView( Buffer.Get(), &uavDesc, UAV.GetAddressOf() );
            if ( FAILED( hr ) ) return hr;
        }

        return S_OK;
    }

    // Update buffer contents (for dynamic buffers)
    HRESULT UpdateBuffer( ID3D11DeviceContext* context, const T* data, UINT count ) {
        if ( count > MaxElementCount ) {
            LogError() << "StructuredBuffer overflow: " << count << " > " << MaxElementCount;
            count = MaxElementCount;
        }

        ElementCount = count;

        D3D11_MAPPED_SUBRESOURCE mapped;
        HRESULT hr = context->Map( Buffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped );
        if ( FAILED( hr ) ) return hr;

        memcpy( mapped.pData, data, sizeof( T ) * count );
        context->Unmap( Buffer.Get(), 0 );

        return S_OK;
    }

    // Update buffer contents (for default buffers)
    void UpdateBufferDefault( ID3D11DeviceContext* context, const T* data, UINT count ) {
        if ( count > MaxElementCount ) {
            LogError() << "StructuredBuffer overflow: " << count << " > " << MaxElementCount;
            count = MaxElementCount;
        }
        ElementCount = count;
        context->UpdateSubresource( Buffer.Get(), 0, nullptr, data, 0, 0 );
    }

    // Bind to vertex shader
    void BindToVertexShader( ID3D11DeviceContext* context, UINT slot ) {
        context->VSSetShaderResources( slot, 1, SRV.GetAddressOf() );
    }

    // Bind to pixel shader
    void BindToPixelShader( ID3D11DeviceContext* context, UINT slot ) {
        context->PSSetShaderResources( slot, 1, SRV.GetAddressOf() );
    }

    // Unbind from vertex shader
    void UnbindFromVertexShader( ID3D11DeviceContext* context, UINT slot ) {
        ID3D11ShaderResourceView* nullSRV = nullptr;
        context->VSSetShaderResources( slot, 1, &nullSRV );
    }

    // Bind to compute shader (SRV)
    void BindToComputeShader( ID3D11DeviceContext* context, UINT slot ) {
        context->CSSetShaderResources( slot, 1, SRV.GetAddressOf() );
    }

    // Unbind from compute shader
    void UnbindFromComputeShader( ID3D11DeviceContext* context, UINT slot ) {
        ID3D11ShaderResourceView* nullSRV = nullptr;
        context->CSSetShaderResources( slot, 1, &nullSRV );
    }

    UINT GetElementCount() const { return ElementCount; }
    UINT GetMaxElementCount() const { return MaxElementCount; }
    ID3D11Buffer* GetBuffer() const { return Buffer.Get(); }
    ID3D11ShaderResourceView* GetSRV() const { return SRV.Get(); }
    ID3D11UnorderedAccessView* GetUAV() const { return UAV.Get(); }

private:
    Microsoft::WRL::ComPtr<ID3D11Buffer> Buffer;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> SRV;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> UAV;
    UINT ElementCount;
    UINT MaxElementCount;
};
