#include "../pch.h"
#include "VulkanNullResources.h"
#include "../DDSFormat.h"

XRESULT VulkanNullTexture::Init( INT2 size, ETextureFormat format, unsigned int, const void*, const std::string& ) {
    m_Format = static_cast<DXGI_FORMAT>( format );
    m_Size = size;
    return XR_SUCCESS;
}

XRESULT VulkanNullTexture::Init( const std::string& ) { return XR_SUCCESS; }
XRESULT VulkanNullTexture::Init( const uint8_t*, size_t, const std::string& ) { return XR_SUCCESS; }
XRESULT VulkanNullTexture::UpdateData( void*, int ) { return XR_SUCCESS; }
XRESULT VulkanNullTexture::UpdateDataDeferred( void*, int ) { return XR_SUCCESS; }

unsigned int VulkanNullTexture::GetRowPitchBytes( int mip ) {
    return DDS::RowPitch( m_Format, static_cast<uint32_t>( std::max( 1, m_Size.x >> mip ) ) );
}

unsigned int VulkanNullTexture::GetSizeInBytes( int mip ) {
    return DDS::SurfaceBytes( m_Format,
        static_cast<uint32_t>( std::max( 1, m_Size.x >> mip ) ),
        static_cast<uint32_t>( std::max( 1, m_Size.y >> mip ) ) );
}

bool VulkanNullTexture::Is16BitTexture() {
    return m_Format == DXGI_FORMAT_B5G6R5_UNORM || m_Format == DXGI_FORMAT_B5G5R5A1_UNORM || m_Format == DXGI_FORMAT_B4G4R4A4_UNORM;
}

XRESULT VulkanNullTexture::BindToPixelShader( int ) { return XR_SUCCESS; }
XRESULT VulkanNullTexture::BindToVertexShader( int ) { return XR_SUCCESS; }
XRESULT VulkanNullTexture::BindToDomainShader( int ) { return XR_SUCCESS; }
XRESULT VulkanNullTexture::CreateThumbnail() { return XR_SUCCESS; }
XRESULT VulkanNullTexture::GenerateMipMaps() { return XR_SUCCESS; }
XRESULT VulkanNullTexture::GenerateMipMapsDeferred() { return XR_SUCCESS; }

XRESULT VulkanNullVertexBuffer::Init( void*, unsigned int sizeInBytes, EBindFlags, EUsageFlags, ECPUAccessFlags,
    const std::string&, unsigned int ) {
    m_SizeInBytes = sizeInBytes;
    return XR_SUCCESS;
}

XRESULT VulkanNullVertexBuffer::UpdateBuffer( void*, unsigned int ) { return XR_SUCCESS; }

XRESULT VulkanNullVertexBuffer::Map( int, void** dataPtr, unsigned int* size ) {
    m_Mapped.resize( m_SizeInBytes );
    if ( dataPtr ) *dataPtr = m_Mapped.data();
    if ( size ) *size = m_SizeInBytes;
    return XR_SUCCESS;
}

XRESULT VulkanNullVertexBuffer::Unmap() { return XR_SUCCESS; }

XRESULT VulkanNullVertexBuffer::OptimizeVertices( VERTEX_INDEX*, uint8_t*, unsigned int, unsigned int, unsigned int,
    std::vector<VERTEX_INDEX>* outShadowIndices, std::vector<VERTEX_INDEX>* inOutLodIndices ) {
    // Nothing is drawn yet, so skip the vertex-cache work; empty shadow/LOD lists mean "use the render indices".
    if ( outShadowIndices ) outShadowIndices->clear();
    if ( inOutLodIndices ) inOutLodIndices->clear();
    return XR_SUCCESS;
}

XRESULT VulkanNullVertexBuffer::OptimizeFaces( VERTEX_INDEX*, uint8_t*, unsigned int, unsigned int, unsigned int ) {
    return XR_SUCCESS;
}
