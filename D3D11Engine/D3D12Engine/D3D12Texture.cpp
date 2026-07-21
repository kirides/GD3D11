#include "../pch.h"
#include "D3D12Texture.h"
#include "D3D12GraphicsEngine.h"
#include "../Engine.h"
#include "../Toolbox.h"
#include "../zFILE_VDFS.h"

#include <fstream>

using Microsoft::WRL::ComPtr;

namespace {
    inline D3D12GraphicsEngine* Engine12() {
        return static_cast<D3D12GraphicsEngine*>( Engine::GraphicsEngine );
    }
    inline bool IsBC( DXGI_FORMAT f ) {
        return f == DXGI_FORMAT_BC1_UNORM || f == DXGI_FORMAT_BC2_UNORM || f == DXGI_FORMAT_BC3_UNORM
            || f == DXGI_FORMAT_BC5_UNORM || f == DXGI_FORMAT_BC5_SNORM;
    }
    // 8-byte block formats (BC1); everything else BC is 16-byte (BC2/3/5). Drives the row-pitch/size math below.
    inline bool IsBC8ByteBlock( DXGI_FORMAT f ) { return f == DXGI_FORMAT_BC1_UNORM; }
    inline bool Is16Bit( DXGI_FORMAT f ) {
        return f == DXGI_FORMAT_B5G6R5_UNORM || f == DXGI_FORMAT_B5G5R5A1_UNORM || f == DXGI_FORMAT_B4G4R4A4_UNORM;
    }
}

D3D12Texture::~D3D12Texture()
{
    D3D12GraphicsEngine* engine = Engine12();
    //engine->UglySyncrhonizationWorkaroundWaitForGpuIdle();
    //engine->FreeSrvSlot( m_SrvSlot );
    m_HasSrv = false;

    if ( m_Texture && m_SrvSlot != 0xFFFFFFFFu ) {
        engine->QueueSrvResourceForRelease( m_SrvSlot, m_Texture );
    }

    m_Texture.Reset();

    /*engine->QueueSrvResourceForRelease( m_SrvSlot, m_Texture );
    engine->QueueSrvResourceForRelease( m_SrvSlot, m_Texture );
    m_Texture.Reset();*/
}

XRESULT D3D12Texture::Init( INT2 size, ETextureFormat format, unsigned int mipMapCount, const void* data, const std::string& fileName ) {
    m_Format = static_cast<DXGI_FORMAT>( format );
    m_Size = size;
    m_MipMapCount = std::max<unsigned int>( 1, mipMapCount );
    if ( !fileName.empty() ) m_DebugName = fileName;
    return CreateAndUpload( data ) ? XR_SUCCESS : XR_FAILED;
}

XRESULT D3D12Texture::Init( const uint8_t* data, size_t size, const std::string& debugFileName ) {
    if ( !debugFileName.empty() ) m_DebugName = debugFileName;
    return InitFromDDS( data, size, debugFileName );
}

XRESULT D3D12Texture::Init( const std::string& file ) {
    if ( !file.empty() ) m_DebugName = file;
    std::vector<uint8_t> bytes;

    if ( std::filesystem::path( file ).is_absolute() ) {
        std::ifstream f( file, std::ios::binary | std::ios::ate );
        if ( !f ) { LogError() << "D3D12Texture: failed to open " << file; return XR_FAILED; }
        std::streamsize sz = f.tellg();
        f.seekg( 0, std::ios::beg );
        bytes.resize( static_cast<size_t>( sz ) );
        if ( !f.read( reinterpret_cast<char*>( bytes.data() ), sz ) ) return XR_FAILED;
    } else {
        zFILE_VDFS::Ptr vdfsFile = ( !file.empty() && file[0] != '\\' )
            ? zFILE_VDFS::Create( ( "\\" + file ).c_str() )
            : zFILE_VDFS::Create( file.c_str() );
        if ( !vdfsFile || !vdfsFile->Exists() || vdfsFile->Open( false ) != zERROR_NONE ) {
            LogError() << "D3D12Texture: failed to load texture from VDFS: " << file;
            return XR_FAILED;
        }
        bytes.resize( vdfsFile->Size() );
        vdfsFile->Read( bytes.data(), bytes.size() );
        vdfsFile->Close();
    }

    return InitFromDDS( bytes.data(), bytes.size(), file );
}

void D3D12Texture::SetDebugName( const char* debugName )
{
    if ( m_Texture ) {
        m_Texture->SetPrivateData( WKPDID_D3DDebugObjectName, std::strlen( debugName ), debugName );
    }
}

/** Minimal DDS parser: enough for Gothic's textures (DXT1/3/5, DX10-extended, 32-bit uncompressed).
    Sets format/size/mips, then creates + uploads. Unknown formats fail gracefully (texture skipped). */
XRESULT D3D12Texture::InitFromDDS( const uint8_t* bytes, size_t size, const std::string& /*name*/ ) {
    if ( !bytes || size < 128 ) {
        return XR_FAILED;
    }
    auto rd = [&]( size_t off ) -> uint32_t { uint32_t v; memcpy( &v, bytes + off, 4 ); return v; };

    if ( rd( 0 ) != 0x20534444u ) {
        return XR_FAILED; // 'DDS '
    }

    const uint32_t height = rd( 12 );
    const uint32_t width  = rd( 16 );
    uint32_t mips = rd( 28 );
    if ( mips == 0 ) mips = 1;

    const uint32_t pfFlags = rd( 80 ); // DDS_PIXELFORMAT.dwFlags  (file offset 4 + 72 + 4)
    const uint32_t fourCC  = rd( 84 ); // DDS_PIXELFORMAT.dwFourCC (file offset 4 + 72 + 8)

    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    size_t dataOffset = 128;

    constexpr uint32_t dxt1 = MAKEFOURCC('D', 'X', 'T', '1');
    constexpr uint32_t dxt3 = MAKEFOURCC('D', 'X', 'T', '3');
    constexpr uint32_t dxt5 = MAKEFOURCC('D', 'X', 'T', '5');
    constexpr uint32_t ati2 = MAKEFOURCC('A', 'T', 'I', '2');
    constexpr uint32_t bc5u = MAKEFOURCC('B', 'C', '5', 'U');
    constexpr uint32_t dx10 = MAKEFOURCC('D', 'X', '1', '0');
    if ( pfFlags & 0x4u /* DDPF_FOURCC */ ) {
        switch ( fourCC ) {
        case dxt1: fmt = DXGI_FORMAT_BC1_UNORM; break; // 'DXT1'
        case dxt3: fmt = DXGI_FORMAT_BC2_UNORM; break; // 'DXT3'
        case dxt5: fmt = DXGI_FORMAT_BC3_UNORM; break; // 'DXT5'
        case bc5u:
        case ati2: fmt = DXGI_FORMAT_BC5_UNORM; break; // 'ATI2' — BC5 2-channel (normal maps, XY + reconstruct Z)
        case dx10:                                     // 'DX10'
            if ( size < 148 ) return XR_FAILED;
            fmt = static_cast<DXGI_FORMAT>( rd( 128 ) );      // DDS_HEADER_DXT10.dxgiFormat
            dataOffset = 148;
            break;
        default:
            return XR_FAILED;
        }
    } else {
        const uint32_t rgbBits = rd( 88 ); // DDS_PIXELFORMAT.dwRGBBitCount (file offset 4 + 72 + 12)
        if ( rgbBits == 32 ) fmt = DXGI_FORMAT_B8G8R8A8_UNORM;
        else {
            return XR_FAILED;
        }
    }

    m_Format = fmt;
    m_Size = INT2( static_cast<int>( width ), static_cast<int>( height ) );
    m_MipMapCount = mips;

    // Clamp mip count to what the payload actually contains (avoid reading past the buffer).
    const size_t avail = size - dataOffset;
    size_t consumed = 0;
    unsigned int fit = 0;
    for ( unsigned int i = 0; i < mips; ++i ) {
        const size_t mipSize = GetSizeInBytes( static_cast<int>( i ) );
        if ( consumed + mipSize > avail ) break;
        consumed += mipSize;
        ++fit;
    }
    if ( fit == 0 ) {
        return XR_FAILED;
    }
    m_MipMapCount = fit;

    if ( dataOffset > size ) {
        return XR_FAILED;
    }
    return CreateAndUpload( const_cast<uint8_t*>( bytes + dataOffset ) ) ? XR_SUCCESS : XR_FAILED;
}

bool D3D12Texture::CreateAndUpload( const void* data ) {
    D3D12GraphicsEngine* engine = Engine12();
    ID3D12Device* device = engine ? engine->GetD3DDevice() : nullptr;
    D3D12MA::Allocator* allocator = engine ? engine->GetAllocator() : nullptr;
    if ( !device || !allocator || m_Size.x <= 0 || m_Size.y <= 0 || m_Format == DXGI_FORMAT_UNKNOWN ) {
        return false;
    }

    D3D12_RESOURCE_DESC td = {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = static_cast<UINT64>( m_Size.x );
    td.Height = static_cast<UINT>( m_Size.y );
    td.DepthOrArraySize = 1;
    td.MipLevels = static_cast<UINT16>( m_MipMapCount );
    td.Format = m_Format;
    td.SampleDesc.Count = 1;
    td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    td.Flags = D3D12_RESOURCE_FLAG_NONE;

    if ( m_Allocation ) {
        engine->QueueAllocationForRelease( m_Allocation );
        m_Allocation.Reset();
        m_Texture.Reset();
    }
    
    D3D12MA::ALLOCATION_DESC allocDesc = {};
    allocDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;

    const D3D12_RESOURCE_STATES initState = D3D12_RESOURCE_STATE_COMMON;
    
    if ( FAILED( allocator->CreateResource(
        &allocDesc,
        &td,
        initState,
        nullptr,
        m_Allocation.ReleaseAndGetAddressOf(),
        IID_PPV_ARGS( m_Texture.ReleaseAndGetAddressOf() ) ) ) ) {
        LogWarn() << "D3D12Texture: D3D12MA::CreateResource failed (format " << static_cast<int>( m_Format )
                  << ", " << m_Size.x << "x" << m_Size.y << ", mips " << m_MipMapCount << ").";
        return false;
    }

    {
        const std::string debugName = "Tex:" + ( m_DebugName.empty() ? std::string( "unnamed" ) : m_DebugName );
        m_Allocation->SetName( std::wstring( debugName.begin(), debugName.end() ).c_str() );
        m_Texture->SetPrivateData( WKPDID_D3DDebugObjectName, static_cast<UINT>( debugName.size() ), debugName.c_str() );
    }

    if ( !data ) {
        CreateSRV();
        return true;
    }

    std::vector<D3D12_SUBRESOURCE_DATA> subs( m_MipMapCount );
    const uint8_t* src = reinterpret_cast<const uint8_t*>( data );
    size_t offset = 0;
    for ( unsigned int i = 0; i < m_MipMapCount; ++i ) {
        subs[i].pData = src + offset;
        subs[i].RowPitch = GetRowPitchBytes( static_cast<int>( i ) );
        subs[i].SlicePitch = GetSizeInBytes( static_cast<int>( i ) );
        offset += GetSizeInBytes( static_cast<int>( i ) );
    }

    // Fully async upload submission
    if ( !engine->UploadTextureSubresources( m_Texture.Get(), subs.data(), m_MipMapCount ) )
        return false;

    // Immediately create SRV; GPU sync is queued on the direct command queue asynchronously
    CreateSRV();
    return true;
}

/** (Re)creates the shader-visible SRV for the current resource. The heap slot is allocated once and
    reused across resource recreations (UpdateData), so a texture's GPU descriptor handle is stable. */
void D3D12Texture::CreateSRV() {
    D3D12GraphicsEngine* engine = Engine12();
    ID3D12Device* device = engine ? engine->GetD3DDevice() : nullptr;
    if ( !device || !m_Texture ) return;

    if ( m_SrvSlot == 0xFFFFFFFFu ) {
        m_SrvSlot = engine->AllocateSrvSlot();
        if ( m_SrvSlot == 0xFFFFFFFFu ) return; // heap exhausted — stays unbindable (white fallback used)
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC srvd = {};
    srvd.Format = m_Format;
    srvd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvd.Texture2D.MipLevels = m_MipMapCount;
    device->CreateShaderResourceView( m_Texture.Get(), &srvd, engine->GetSrvCpuHandle( m_SrvSlot ) );
    m_SrvGpu = engine->GetSrvGpuHandle( m_SrvSlot );
    m_HasSrv = true;
}

XRESULT D3D12Texture::UpdateData( void* data, int mip ) {
    if ( m_MipMapCount == 1 ) {
        // Single-mip surfaces (fonts / dynamic UI): recreate immutable + upload (mirrors D3D11).
        return CreateAndUpload( data ) ? XR_SUCCESS : XR_FAILED;
    }

    // Multi-mip: accumulate the concatenated chain, build the texture once the last mip arrives.
    if ( mip == 0 ) {
        size_t total = 0;
        for ( unsigned int i = 0; i < m_MipMapCount; ++i ) total += GetSizeInBytes( static_cast<int>( i ) );
        m_Staging.resize( total );
    }
    size_t off = 0;
    for ( int i = 0; i < mip; ++i ) off += GetSizeInBytes( i );
    memcpy( m_Staging.data() + off, data, GetSizeInBytes( mip ) );

    if ( mip + 1 == static_cast<int>( m_MipMapCount ) ) {
        return CreateAndUpload( m_Staging.data() ) ? XR_SUCCESS : XR_FAILED;
    }
    return XR_SUCCESS;
}

XRESULT D3D12Texture::UpdateDataDeferred( void* data, int mip ) {
    // Synchronous for now (safe with the default single-threaded resource manager). A true async
    // copy-queue path lands with the D3D12 upload-ring / copy-queue work.
    return UpdateData( data, mip );
}

unsigned int D3D12Texture::GetRowPitchBytes( int mip ) {
    const int px = m_Size.x >> mip;
    if ( m_Format == DXGI_FORMAT_R8_UNORM ) return px;
    if ( Is16Bit( m_Format ) ) return px * 2;
    if ( IsBC( m_Format ) ) return Toolbox::GetDDSRowPitchSize( px, m_Format == DXGI_FORMAT_BC1_UNORM );
    return px * 4; // B8G8R8A8 and friends
}

unsigned int D3D12Texture::GetSizeInBytes( int mip ) {
    const int px = m_Size.x >> mip;
    const int py = m_Size.y >> mip;
    if ( m_Format == DXGI_FORMAT_R8_UNORM ) return px * py;
    if ( Is16Bit( m_Format ) ) return px * py * 2;
    if ( IsBC( m_Format ) ) return Toolbox::GetDDSStorageRequirements( px, py, m_Format == DXGI_FORMAT_BC1_UNORM );
    return px * py * 4;
}

bool D3D12Texture::Is16BitTexture() {
    return Is16Bit( m_Format );
}
