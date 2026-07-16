#pragma once
#include <wrl/client.h>
#include <d3d12.h>
#include <vector>
#include "../GfxTexture.h"

/** D3D12 2D texture (Phase 1).

    Creates a real committed DEFAULT-heap texture and uploads pixel data synchronously through the
    engine's uploader. The CPU-side size math (GetRowPitchBytes/GetSizeInBytes/Is16BitTexture)
    mirrors D3D11Texture exactly — Gothic's DDraw surface wrapper relies on it to size the staging
    buffer it locks and writes into (a wrong/zero size there overflows before the first frame).

    Binding is still a no-op (no descriptor tables / draw path yet) — that lands with the D3D12
    descriptor + PSO work. 16-bit formats are created natively; if the GPU rejects one, creation
    fails gracefully (texture skipped, logged) rather than crashing. */
class D3D12Texture : public GfxTexture {
public:
    D3D12Texture() = default;
    ~D3D12Texture() override;

    XRESULT Init( INT2 size, ETextureFormat format, unsigned int mipMapCount = 1, void* data = nullptr, const std::string& fileName = "" ) override;
    XRESULT Init( const std::string& file ) override;
    XRESULT Init( const uint8_t* data, size_t size, const std::string& debugFileName ) override;

    XRESULT UpdateData( void* data, int mip = 0 ) override;
    XRESULT UpdateDataDeferred( void* data, int mip ) override;

    unsigned int GetRowPitchBytes( int mip ) override;
    unsigned int GetSizeInBytes( int mip ) override;
    bool Is16BitTexture() override;

    XRESULT BindToPixelShader( int /*slot*/ ) override { return XR_SUCCESS; }
    XRESULT BindToVertexShader( int /*slot*/ ) override { return XR_SUCCESS; }
    XRESULT BindToDomainShader( int /*slot*/ ) override { return XR_SUCCESS; }

    XRESULT CreateThumbnail() override { return XR_SUCCESS; }
    XRESULT GenerateMipMaps() override { return XR_SUCCESS; }
    XRESULT GenerateMipMapsDeferred() override { return XR_SUCCESS; }

    uint16_t GetID() override { return m_ID; }

    ID3D12Resource* GetResource() const { return m_Texture.Get(); }
    DXGI_FORMAT GetFormat() const { return m_Format; }
    INT2 GetSize() const { return m_Size; }
    unsigned int GetMipMapCount() const { return m_MipMapCount; }

    /** True once a shader-visible SRV has been created for the current resource (bindable in a draw). */
    bool HasSRV() const { return m_HasSrv; }
    D3D12_GPU_DESCRIPTOR_HANDLE GetSrvGpuHandle() const { return m_SrvGpu; }

    /** Backend downcast from the neutral base. Safe by construction while the D3D12 backend is active. */
    static D3D12Texture* From( GfxTexture* texture ) { return static_cast<D3D12Texture*>( texture ); }

    void SetDebugName( const char* debugName ) override;

private:
    XRESULT InitFromDDS( const uint8_t* bytes, size_t size, const std::string& name );
    bool CreateAndUpload( void* data );   // committed resource (+ synchronous upload when data != null)
    void CreateSRV();                     // (re)creates the shader-visible SRV for the current resource

    Microsoft::WRL::ComPtr<ID3D12Resource> m_Texture;
    DXGI_FORMAT  m_Format = DXGI_FORMAT_UNKNOWN;
    INT2         m_Size = {};
    unsigned int m_MipMapCount = 1;
    uint16_t     m_ID = 0;
    std::vector<uint8_t> m_Staging;       // multi-mip UpdateData accumulation

    unsigned int m_SrvSlot = 0xFFFFFFFFu;                 // persistent slot in the engine's SRV heap (UINT_MAX = none)
    D3D12_GPU_DESCRIPTOR_HANDLE m_SrvGpu = {};            // cached GPU handle for that slot
    bool m_HasSrv = false;
};
