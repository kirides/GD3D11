#pragma once
#include "../GfxTexture.h"
#include "../GfxVertexBuffer.h"
#include "../BaseLineRenderer.h"
#include <vector>

// Phase-1 stand-ins while the Vulkan backend only clears and presents. They accept everything Gothic
// creates and keep nothing on the GPU; replaced by the RHI resources in Phase 3.

/** Size math only: Gothic's DDraw surface wrapper sizes its lock buffer from these, so they must match D3D12Texture. */
class VulkanNullTexture : public GfxTexture {
public:
    XRESULT Init( INT2 size, ETextureFormat format, unsigned int mipMapCount = 1, const void* data = nullptr, const std::string& fileName = "" ) override;
    XRESULT Init( const std::string& file ) override;
    XRESULT Init( const uint8_t* data, size_t size, const std::string& debugFileName ) override;
    XRESULT UpdateData( void* data, int mip = 0 ) override;
    XRESULT UpdateDataDeferred( void* data, int mip ) override;
    unsigned int GetRowPitchBytes( int mip ) override;
    unsigned int GetSizeInBytes( int mip ) override;
    bool Is16BitTexture() override;
    XRESULT BindToPixelShader( int slot ) override;
    XRESULT BindToVertexShader( int slot ) override;
    XRESULT BindToDomainShader( int slot ) override;
    XRESULT CreateThumbnail() override;
    XRESULT GenerateMipMaps() override;
    XRESULT GenerateMipMapsDeferred() override;
    uint16_t GetID() override { return 0; }

private:
    DXGI_FORMAT m_Format = DXGI_FORMAT_UNKNOWN;
    INT2 m_Size = {};
};

/** CPU memory is only allocated when something maps the buffer (dynamic Gothic vertex buffers). */
class VulkanNullVertexBuffer : public GfxVertexBuffer {
public:
    XRESULT Init( void* initData, unsigned int sizeInBytes, EBindFlags bindFlags = B_VERTEXBUFFER, EUsageFlags usage = U_DEFAULT,
        ECPUAccessFlags cpuAccess = CA_NONE, const std::string& fileName = "", unsigned int structuredByteSize = 0 ) override;
    XRESULT UpdateBuffer( void* data, unsigned int size = 0 ) override;
    XRESULT Map( int flags, void** dataPtr, unsigned int* size ) override;
    XRESULT Unmap() override;
    XRESULT OptimizeVertices( VERTEX_INDEX* indices, uint8_t* vertices, unsigned int numIndices, unsigned int numVertices,
        unsigned int stride, std::vector<VERTEX_INDEX>* outShadowIndices = nullptr, std::vector<VERTEX_INDEX>* inOutLodIndices = nullptr ) override;
    XRESULT OptimizeFaces( VERTEX_INDEX* indices, uint8_t* vertices, unsigned int numIndices, unsigned int numVertices, unsigned int stride ) override;
    unsigned int GetSizeInBytes() const override { return m_SizeInBytes; }

private:
    unsigned int m_SizeInBytes = 0;
    std::vector<uint8_t> m_Mapped;
};

class VulkanNullLineRenderer : public BaseLineRenderer {
public:
    XRESULT Flush() override { return ClearCache(); }
    XRESULT FlushScreenSpace() override { return ClearCache(); }
};
