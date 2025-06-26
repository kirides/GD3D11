#pragma once

class D3D11ConstantBuffer {
public:
    D3D11ConstantBuffer( int size, void* data );
    ~D3D11ConstantBuffer();

    /** Updates the buffer */
    void UpdateBuffer( const void* data );
    void UpdateBuffer( const void* data, UINT size );

    /** Binds the buffer */
    void BindToVertexShader( int slot );
    void BindToPixelShader( int slot );
    void BindToDomainShader( int slot );
    void BindToHullShader( int slot );
    void BindToGeometryShader( int slot );
    void BindToComputeShader( int slot );

    /** Binds the constantbuffer */
    Microsoft::WRL::ComPtr<ID3D11Buffer>& Get() { return Buffer; }

    /** Returns whether this buffer has been updated since the last bind */
    bool IsDirty();

private:
    Microsoft::WRL::ComPtr<ID3D11Buffer> Buffer;
    int OriginalSize; // Buffersize must be a multiple of 16
    bool BufferDirty;
};
