#include "../pch.h"
#include "D3D12Texture.h"
#include "D3D12GraphicsEngine.h"
#include "../Engine.h"
#include "../Toolbox.h"
#include "../zFILE_VDFS.h"
#include "../DDSFormat.h"

#include <fstream>
#include <algorithm>
#include <cstdint>

using Microsoft::WRL::ComPtr;

namespace {
    inline D3D12GraphicsEngine* Engine12() {
        return static_cast<D3D12GraphicsEngine*>( Engine::GraphicsEngine );
    }

    // The three legacy packed 16-bit formats Gothic's DDraw surface wrapper treats specially (see
    // D3D12Texture::Is16BitTexture, consumed by MyDirectDrawSurface7's lock sizing). NOT a general
    // "is 2-byte pixel" test — R8G8 / R16 are also 16 bpp but must not be flagged here. (Format decoding
    // and the pitch/size math live in the shared DDSFormat.h — see the DDS:: namespace.)
    inline bool Is16Bit( DXGI_FORMAT f ) {
        return f == DXGI_FORMAT_B5G6R5_UNORM || f == DXGI_FORMAT_B5G5R5A1_UNORM || f == DXGI_FORMAT_B4G4R4A4_UNORM;
    }
}

XRESULT D3D12Texture::BindToPixelShader( int slot )
{
    Engine12()->SetVideoTextureSlot( slot, this );
    return XR_SUCCESS;
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

    // Defer the backing D3D12MA allocation alongside the resource — releasing it here would return the
    // heap block to the allocator while the deferred ID3D12Resource is still alive (and possibly still
    // read by an in-flight frame), so the next placed resource lands on top of it.
    if ( m_Allocation ) {
        engine->QueueAllocationForRelease( std::move( m_Allocation ) );
        m_Allocation.Reset();
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

/** DDS parser covering the common resource types: BC1..BC7 (via FOURCC or the DX10-extended header),
    the legacy float/16-bit FOURCC D3DFMT codes, DX10-extended arbitrary DXGI formats, and uncompressed
    8/16/32-bit surfaces resolved from their channel masks (RGBA/BGRA/BGRX, 10:10:10:2, 5:6:5, 5:5:5:1,
    4:4:4:4, R8G8, R8/L8, A8, R16, R16G16, R32F). Sets format/size/mips, then creates + uploads. Formats
    with no direct DXGI equivalent (e.g. 24-bit RGB) fail gracefully — the texture is skipped and logged. */
XRESULT D3D12Texture::InitFromDDS( const uint8_t* bytes, size_t size, const std::string& /*name*/ ) {
    if ( !bytes || size < 128 ) {
        return XR_FAILED;
    }
    auto rd = [&]( size_t off ) -> uint32_t { uint32_t v; memcpy( &v, bytes + off, 4 ); return v; };

    if ( rd( 0 ) != DDS::Magic ) {
        return XR_FAILED; // 'DDS '
    }

    const uint32_t height = rd( 12 );
    const uint32_t width  = rd( 16 );
    uint32_t mips = rd( 28 );
    if ( mips == 0 ) mips = 1;

    const uint32_t pfFlags = rd( 80 ); // DDS_PIXELFORMAT.dwFlags       (file offset 4 + 72 + 4)
    const uint32_t fourCC  = rd( 84 ); // DDS_PIXELFORMAT.dwFourCC      (file offset 4 + 72 + 8)

    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    size_t dataOffset = 128;

    if ( pfFlags & DDS::FlagFourCC ) {
        if ( fourCC == DDS::Dx10 ) {
            if ( size < 148 ) return XR_FAILED;
            fmt = static_cast<DXGI_FORMAT>( rd( 128 ) );   // DDS_HEADER_DXT10.dxgiFormat
            dataOffset = 148;
        } else {
            fmt = DDS::FromFourCC( fourCC );               // BC1..BC5 + legacy float/wide D3DFMT codes
        }
    } else {
        // Uncompressed: resolve the DXGI format from the pixel-format flags + channel masks.
        fmt = DDS::FromPixelFormat( pfFlags, rd( 88 ), rd( 92 ), rd( 96 ), rd( 100 ), rd( 104 ) );
    }

    // Reject anything the size math can't describe (neither a known BC format nor a known bpp).
    if ( DDS::BCBlockBytes( fmt ) == 0 && DDS::BitsPerPixel( fmt ) == 0 ) {
        LogWarn() << "D3D12Texture: unsupported DDS format (fourCC=" << fourCC << " flags=" << pfFlags
                  << " bpp=" << rd( 88 ) << " -> DXGI " << static_cast<int>( fmt ) << ") — texture skipped.";
        return XR_FAILED;
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
    return DDS::RowPitch( m_Format, static_cast<uint32_t>( std::max( 1, m_Size.x >> mip ) ) );
}

unsigned int D3D12Texture::GetSizeInBytes( int mip ) {
    return DDS::SurfaceBytes( m_Format,
        static_cast<uint32_t>( std::max( 1, m_Size.x >> mip ) ),
        static_cast<uint32_t>( std::max( 1, m_Size.y >> mip ) ) );
}

bool D3D12Texture::Is16BitTexture() {
    return Is16Bit( m_Format );
}
