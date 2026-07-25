#pragma once
// Shared DDS format helpers used by every hand-rolled DDS parser in the codebase (the D3D12 texture
// loader and DDSArrayLoader). D3D11's single-texture path uses DirectXTK's CreateDDSTextureFromFile*
// and does not need this. Centralising the format tables here keeps the parsers in sync and replaces
// the magic bit-masks / FOURCC integers with named constants.
#include <dxgiformat.h>
#include <cstdint>

namespace DDS {

    // constexpr equivalent of the Win SDK MAKEFOURCC macro. Used instead of the macro itself because the
    // SDK version expands to DWORD/BYTE casts, which aren't in scope in every TU that includes this header
    // (it deliberately doesn't pull <windows.h>). Keeps the four-char codes below visibly obvious.
    constexpr uint32_t FourCC( char a, char b, char c, char d ) {
        return static_cast<uint32_t>( static_cast<uint8_t>( a ) )
            | (static_cast<uint32_t>( static_cast<uint8_t>( b ) ) << 8)
            | (static_cast<uint32_t>( static_cast<uint8_t>( c ) ) << 16)
            | (static_cast<uint32_t>( static_cast<uint8_t>( d ) ) << 24);
    }

    // NOTE: these are intentionally NOT named DDPF_*/FOURCC_*/D3DFMT_* — those are Win SDK preprocessor
    // macros (ddraw.h / d3d9types.h, both pulled in by this ddraw-wrapper project). A `constexpr` named after
    // a macro is rewritten by the preprocessor before the compiler sees it, so we use namespace-scoped
    // mixed-case names (DDS::FlagRgb, DDS::Dxt1, …) that can't collide.

    constexpr uint32_t Magic = FourCC( 'D', 'D', 'S', ' ' );   // 0x20534444

    // DDS_PIXELFORMAT.dwFlags bits (DDPF_*).
    constexpr uint32_t FlagAlphaPixels = 0x00000001;
    constexpr uint32_t FlagAlpha       = 0x00000002;
    constexpr uint32_t FlagFourCC      = 0x00000004;
    constexpr uint32_t FlagRgb         = 0x00000040;
    constexpr uint32_t FlagLuminance   = 0x00020000;
    constexpr uint32_t FlagBumpDuDv    = 0x00080000;

    // Four-character-code pixel formats (DDS_PIXELFORMAT.dwFourCC).
    constexpr uint32_t Dxt1 = FourCC( 'D', 'X', 'T', '1' );
    constexpr uint32_t Dxt2 = FourCC( 'D', 'X', 'T', '2' ); // premultiplied-alpha BC2
    constexpr uint32_t Dxt3 = FourCC( 'D', 'X', 'T', '3' );
    constexpr uint32_t Dxt4 = FourCC( 'D', 'X', 'T', '4' ); // premultiplied-alpha BC3
    constexpr uint32_t Dxt5 = FourCC( 'D', 'X', 'T', '5' );
    constexpr uint32_t Ati1 = FourCC( 'A', 'T', 'I', '1' ); // BC4 (single channel)
    constexpr uint32_t Bc4u = FourCC( 'B', 'C', '4', 'U' );
    constexpr uint32_t Bc4s = FourCC( 'B', 'C', '4', 'S' );
    constexpr uint32_t Ati2 = FourCC( 'A', 'T', 'I', '2' ); // BC5 (two channel, normal maps)
    constexpr uint32_t Bc5u = FourCC( 'B', 'C', '5', 'U' );
    constexpr uint32_t Bc5s = FourCC( 'B', 'C', '5', 'S' );
    constexpr uint32_t Dx10 = FourCC( 'D', 'X', '1', '0' ); // extended header carries a raw DXGI_FORMAT

    // Legacy D3DFMT enum values stored directly in the FOURCC dword (float / wide surfaces from older tools).
    constexpr uint32_t LegacyA16B16G16R16  = 36;
    constexpr uint32_t LegacyQ16W16V16U16  = 110;
    constexpr uint32_t LegacyR16F          = 111;
    constexpr uint32_t LegacyG16R16F       = 112;
    constexpr uint32_t LegacyA16B16G16R16F = 113;
    constexpr uint32_t LegacyR32F          = 114;
    constexpr uint32_t LegacyG32R32F       = 115;
    constexpr uint32_t LegacyA32B32G32R32F = 116;

    // Block-compressed (BC1..BC7) block byte size: 8 for BC1/BC4, 16 for BC2/3/5/6/7. Returns 0 for
    // uncompressed formats. Covers UNORM / SRGB / SNORM / TYPELESS variants.
    inline uint32_t BCBlockBytes( DXGI_FORMAT f ) {
        switch ( f ) {
        case DXGI_FORMAT_BC1_TYPELESS: case DXGI_FORMAT_BC1_UNORM: case DXGI_FORMAT_BC1_UNORM_SRGB:
        case DXGI_FORMAT_BC4_TYPELESS: case DXGI_FORMAT_BC4_UNORM: case DXGI_FORMAT_BC4_SNORM:
            return 8;
        case DXGI_FORMAT_BC2_TYPELESS: case DXGI_FORMAT_BC2_UNORM: case DXGI_FORMAT_BC2_UNORM_SRGB:
        case DXGI_FORMAT_BC3_TYPELESS: case DXGI_FORMAT_BC3_UNORM: case DXGI_FORMAT_BC3_UNORM_SRGB:
        case DXGI_FORMAT_BC5_TYPELESS: case DXGI_FORMAT_BC5_UNORM: case DXGI_FORMAT_BC5_SNORM:
        case DXGI_FORMAT_BC6H_TYPELESS: case DXGI_FORMAT_BC6H_UF16: case DXGI_FORMAT_BC6H_SF16:
        case DXGI_FORMAT_BC7_TYPELESS: case DXGI_FORMAT_BC7_UNORM: case DXGI_FORMAT_BC7_UNORM_SRGB:
            return 16;
        default: return 0;
        }
    }
    inline bool IsBC( DXGI_FORMAT f ) { return BCBlockBytes( f ) != 0; }

    // Bits-per-pixel for uncompressed formats (0 = unknown / compressed). Covers the common 8/16/32/64/128-bit
    // formats a DDS can carry; drives the row-pitch / surface-size math for non-BC textures.
    inline uint32_t BitsPerPixel( DXGI_FORMAT f ) {
        switch ( f ) {
        case DXGI_FORMAT_R32G32B32A32_TYPELESS: case DXGI_FORMAT_R32G32B32A32_FLOAT:
        case DXGI_FORMAT_R32G32B32A32_UINT:     case DXGI_FORMAT_R32G32B32A32_SINT:
            return 128;
        case DXGI_FORMAT_R32G32B32_TYPELESS: case DXGI_FORMAT_R32G32B32_FLOAT:
        case DXGI_FORMAT_R32G32B32_UINT:     case DXGI_FORMAT_R32G32B32_SINT:
            return 96;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R16G16B16A16_UNORM:    case DXGI_FORMAT_R16G16B16A16_UINT:
        case DXGI_FORMAT_R16G16B16A16_SNORM:    case DXGI_FORMAT_R16G16B16A16_SINT:
        case DXGI_FORMAT_R32G32_TYPELESS:       case DXGI_FORMAT_R32G32_FLOAT:
        case DXGI_FORMAT_R32G32_UINT:           case DXGI_FORMAT_R32G32_SINT:
            return 64;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS: case DXGI_FORMAT_R10G10B10A2_UNORM:
        case DXGI_FORMAT_R10G10B10A2_UINT:     case DXGI_FORMAT_R11G11B10_FLOAT:
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:    case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:  case DXGI_FORMAT_R8G8B8A8_UINT:
        case DXGI_FORMAT_R8G8B8A8_SNORM:       case DXGI_FORMAT_R8G8B8A8_SINT:
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:    case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:  case DXGI_FORMAT_B8G8R8X8_TYPELESS:
        case DXGI_FORMAT_B8G8R8X8_UNORM:       case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        case DXGI_FORMAT_R16G16_TYPELESS:      case DXGI_FORMAT_R16G16_FLOAT:
        case DXGI_FORMAT_R16G16_UNORM:         case DXGI_FORMAT_R16G16_UINT:
        case DXGI_FORMAT_R16G16_SNORM:         case DXGI_FORMAT_R16G16_SINT:
        case DXGI_FORMAT_R32_TYPELESS:         case DXGI_FORMAT_R32_FLOAT:
        case DXGI_FORMAT_R32_UINT:             case DXGI_FORMAT_R32_SINT:
        case DXGI_FORMAT_D32_FLOAT:
            return 32;
        case DXGI_FORMAT_R8G8_TYPELESS: case DXGI_FORMAT_R8G8_UNORM: case DXGI_FORMAT_R8G8_UINT:
        case DXGI_FORMAT_R8G8_SNORM:    case DXGI_FORMAT_R8G8_SINT:
        case DXGI_FORMAT_R16_TYPELESS:  case DXGI_FORMAT_R16_FLOAT:  case DXGI_FORMAT_R16_UNORM:
        case DXGI_FORMAT_R16_UINT:      case DXGI_FORMAT_R16_SNORM:  case DXGI_FORMAT_R16_SINT:
        case DXGI_FORMAT_D16_UNORM:
        case DXGI_FORMAT_B5G6R5_UNORM:  case DXGI_FORMAT_B5G5R5A1_UNORM: case DXGI_FORMAT_B4G4R4A4_UNORM:
            return 16;
        case DXGI_FORMAT_R8_TYPELESS: case DXGI_FORMAT_R8_UNORM: case DXGI_FORMAT_R8_UINT:
        case DXGI_FORMAT_R8_SNORM:    case DXGI_FORMAT_R8_SINT:  case DXGI_FORMAT_A8_UNORM:
            return 8;
        default: return 0;
        }
    }

    // Maps a FOURCC pixel format to a DXGI format (UNKNOWN if unrecognized). Does NOT handle FOURCC_DX10 —
    // that carries a raw DXGI_FORMAT in the extended header, which the caller reads directly.
    inline DXGI_FORMAT FromFourCC( uint32_t fourCC ) {
        switch ( fourCC ) {
        case Dxt1: return DXGI_FORMAT_BC1_UNORM;
        case Dxt2: case Dxt3: return DXGI_FORMAT_BC2_UNORM;
        case Dxt4: case Dxt5: return DXGI_FORMAT_BC3_UNORM;
        case Ati1: case Bc4u: return DXGI_FORMAT_BC4_UNORM;
        case Bc4s: return DXGI_FORMAT_BC4_SNORM;
        case Ati2: case Bc5u: return DXGI_FORMAT_BC5_UNORM;
        case Bc5s: return DXGI_FORMAT_BC5_SNORM;
        case LegacyA16B16G16R16:  return DXGI_FORMAT_R16G16B16A16_UNORM;
        case LegacyQ16W16V16U16:  return DXGI_FORMAT_R16G16B16A16_SNORM;
        case LegacyR16F:          return DXGI_FORMAT_R16_FLOAT;
        case LegacyG16R16F:       return DXGI_FORMAT_R16G16_FLOAT;
        case LegacyA16B16G16R16F: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case LegacyR32F:          return DXGI_FORMAT_R32_FLOAT;
        case LegacyG32R32F:       return DXGI_FORMAT_R32G32_FLOAT;
        case LegacyA32B32G32R32F: return DXGI_FORMAT_R32G32B32A32_FLOAT;
        default: return DXGI_FORMAT_UNKNOWN;
        }
    }

    // Maps an uncompressed (non-FOURCC) DDS_PIXELFORMAT to a DXGI format from its channel masks. Returns
    // DXGI_FORMAT_UNKNOWN for layouts with no direct DXGI equivalent (e.g. 24-bit RGB, which needs expansion).
    inline DXGI_FORMAT FromPixelFormat( uint32_t flags, uint32_t bits, uint32_t r, uint32_t g, uint32_t b, uint32_t a ) {
        auto m = [&]( uint32_t rr, uint32_t gg, uint32_t bb, uint32_t aa ) {
            return r == rr && g == gg && b == bb && a == aa;
            };
        if ( flags & (FlagRgb | FlagBumpDuDv) ) {
            switch ( bits ) {
            case 32:
                if ( m( 0x000000ff, 0x0000ff00, 0x00ff0000, 0xff000000 ) ) return DXGI_FORMAT_R8G8B8A8_UNORM;
                if ( m( 0x00ff0000, 0x0000ff00, 0x000000ff, 0xff000000 ) ) return DXGI_FORMAT_B8G8R8A8_UNORM;
                if ( m( 0x00ff0000, 0x0000ff00, 0x000000ff, 0x00000000 ) ) return DXGI_FORMAT_B8G8R8X8_UNORM;
                // 10:10:10:2 — both channel orderings map to R10G10B10A2 (A2R10G10B10 is a common authoring mistake).
                if ( m( 0x3ff00000, 0x000ffc00, 0x000003ff, 0xc0000000 ) ) return DXGI_FORMAT_R10G10B10A2_UNORM;
                if ( m( 0x000003ff, 0x000ffc00, 0x3ff00000, 0xc0000000 ) ) return DXGI_FORMAT_R10G10B10A2_UNORM;
                if ( m( 0x0000ffff, 0xffff0000, 0x00000000, 0x00000000 ) ) return DXGI_FORMAT_R16G16_UNORM;
                if ( m( 0xffffffff, 0x00000000, 0x00000000, 0x00000000 ) ) return DXGI_FORMAT_R32_FLOAT;
                break;
            case 16:
                if ( m( 0xf800, 0x07e0, 0x001f, 0x0000 ) ) return DXGI_FORMAT_B5G6R5_UNORM;
                if ( m( 0x7c00, 0x03e0, 0x001f, 0x8000 ) ) return DXGI_FORMAT_B5G5R5A1_UNORM;
                if ( m( 0x0f00, 0x00f0, 0x000f, 0xf000 ) ) return DXGI_FORMAT_B4G4R4A4_UNORM;
                if ( m( 0x00ff, 0xff00, 0x0000, 0x0000 ) ) return DXGI_FORMAT_R8G8_UNORM;
                if ( m( 0xffff, 0x0000, 0x0000, 0x0000 ) ) return DXGI_FORMAT_R16_UNORM;
                break;
            case 8:
                if ( m( 0xff, 0, 0, 0 ) ) return DXGI_FORMAT_R8_UNORM;
                break;
            }
        }
        if ( flags & FlagLuminance ) {
            if ( bits == 8 && m( 0xff, 0, 0, 0 ) ) return DXGI_FORMAT_R8_UNORM;
            if ( bits == 16 && m( 0xffff, 0, 0, 0 ) ) return DXGI_FORMAT_R16_UNORM;
            // L8A8 has no direct DXGI equivalent — carry it as R8G8 (a consumer shader swizzles .r/.g as L/A).
            if ( bits == 16 && r == 0x00ff && a == 0xff00 ) return DXGI_FORMAT_R8G8_UNORM;
        }
        if ( (flags & FlagAlpha) && bits == 8 ) return DXGI_FORMAT_A8_UNORM;
        return DXGI_FORMAT_UNKNOWN;
    }

    // Row pitch (bytes) of one mip row, block-aware. width should be the mip's width (>=1).
    inline uint32_t RowPitch( DXGI_FORMAT f, uint32_t width ) {
        const uint32_t w = width ? width : 1u;
        if ( const uint32_t block = BCBlockBytes( f ) ) {
            const uint32_t blocksWide = (w + 3) / 4 > 0 ? (w + 3) / 4 : 1u;
            return blocksWide * block;
        }
        uint32_t bpp = BitsPerPixel( f );
        if ( bpp == 0 ) bpp = 32;   // unknown: assume 32-bit (callers reject unknown formats upstream)
        return (w * bpp + 7) / 8;
    }

    // Total bytes of one mip surface, block-aware. width/height should be the mip's dims (>=1).
    inline uint32_t SurfaceBytes( DXGI_FORMAT f, uint32_t width, uint32_t height ) {
        const uint32_t h = height ? height : 1u;
        if ( BCBlockBytes( f ) ) {
            const uint32_t blocksHigh = (h + 3) / 4 > 0 ? (h + 3) / 4 : 1u;
            return RowPitch( f, width ) * blocksHigh;
        }
        return RowPitch( f, width ) * h;
    }

}   // namespace DDS
