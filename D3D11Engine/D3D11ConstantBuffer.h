#pragma once

class D3D11ConstantBuffer {
public:
    D3D11ConstantBuffer( int size, void* data );
    ~D3D11ConstantBuffer() = default;

    /** Updates the buffer */
    D3D11ConstantBuffer* UpdateBuffer( const void* data );
    D3D11ConstantBuffer* UpdateBuffer( const void* data, UINT size );

    /** Binds the buffer */
    D3D11ConstantBuffer* BindToVertexShader( int slot );
    D3D11ConstantBuffer* BindToPixelShader( int slot );
    D3D11ConstantBuffer* BindToDomainShader( int slot );
    D3D11ConstantBuffer* BindToHullShader( int slot );
    D3D11ConstantBuffer* BindToGeometryShader( int slot );
    D3D11ConstantBuffer* BindToComputeShader( int slot );

    /** Binds the constantbuffer */
    Microsoft::WRL::ComPtr<ID3D11Buffer>& Get() { return Buffer; }

    /** Returns whether this buffer has been updated since the last bind */
    bool IsDirty();

private:
    Microsoft::WRL::ComPtr<ID3D11Buffer> Buffer;
    int OriginalSize; // Buffersize must be a multiple of 16
    bool BufferDirty;
};
