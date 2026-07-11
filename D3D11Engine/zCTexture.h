#pragma once
#include "pch.h"
#include "HookedFunctions.h"
#include "Engine.h"
#include "GothicAPI.h"
#include "zCResourceManager.h"
#include "zFILE_VDFS.h"
#include "zSTRING.h"
#include "D3D7\MyDirectDrawSurface7.h"

namespace zCTextureCacheHack {
    inline __declspec(selectany) unsigned int NumNotCachedTexturesInFrame;
    constexpr int MAX_NOT_CACHED_TEXTURES_IN_FRAME = 40;

    /** If true, this will force all calls to CacheIn to have -1 as parameter, which makes them an immediate cache in!
        Be very careful with this as the game will lag everytime a texture is being loaded!*/
    inline __declspec(selectany) bool ForceCacheIn;
};

class zCTexture {
public:
    /** Hooks the functions of this Class */
    static void Hook() {
        //DetourAttachTyped( &HookedFunctions::OriginalFunctions.original_zCTex_D3DXTEX_BuildSurfaces, hooked_XTEX_BuildSurfaces  );
        DetourAttachTyped( &HookedFunctions::OriginalFunctions.ofiginal_zCTextureLoadResourceData, hooked_LoadResourceData  );

        zCTextureCacheHack::NumNotCachedTexturesInFrame = 0;
        zCTextureCacheHack::ForceCacheIn = false;
    }

    static int __fastcall hooked_LoadResourceData( zCTexture* thisptr ) {
        Engine::GAPI->SetBoundTexture( 7, thisptr ); // Slot 7 is reserved for this
        // TODO: Figure out why some DTX1a Textures crash this
        int ret = HookedFunctions::OriginalFunctions.ofiginal_zCTextureLoadResourceData( thisptr );

        Engine::GAPI->SetBoundTexture( 7, nullptr ); // Slot 7 is reserved for this

        return ret;
    }

    /*
    static int __fastcall hooked_XTEX_BuildSurfaces( void* thisptr, void* unknwn, int iVal ) {
        // Notify the texture and load resources
        int ret = HookedFunctions::OriginalFunctions.original_zCTex_D3DXTEX_BuildSurfaces( thisptr, iVal );

        return ret;
    }
    */

    std::string GetName() const {
        const zSTRING& str = __GetName();
        return std::string( str.ToChar(), static_cast<size_t>( str.Length() ) );
    }

    std::string GetNameWithoutExt() const {
        std::string_view n = GetNameView();

        auto p = n.find_last_of( '.' );

        if ( p != std::string::npos )
            return std::string(n.data(), p);

        return std::string( n.data(), n.length() );
    }

    std::string_view GetNameView() const {
        const zSTRING& str = __GetName();
        std::string_view n = std::string_view( str.ToChar(), str.Length() );
        return n;
    }

    std::string_view GetNameWithoutExtView() const {
        const std::string_view n = GetNameView();

        const auto p = n.find_last_of( '.' );

        if ( p != std::string_view::npos )
            return n.substr( 0, p );

        return n;
    }

    MyDirectDrawSurface7* GetSurface() {
        return *reinterpret_cast<MyDirectDrawSurface7**>(THISPTR_OFFSET( GothicMemoryLocations::zCTexture::Offset_Surface ));
    }

    void Bind( int slot = 0 ) {
        if (auto surface = GetSurface(); surface && surface->IsSurfaceReady()) {
            surface->GetEngineTexture()->BindToPixelShader(slot);
            surface->GetEngineTexture()->BindToVertexShader(slot);
        }
        
        // Engine::GAPI->SetBoundTexture( slot, this );
        //
        // reinterpret_cast<void(__fastcall*)( zCTexture*, int, bool, int )>
        //     ( GothicMemoryLocations::zCTexture::zCTex_D3DInsertTexture )( this, 0, false, slot );
    }

    int LoadResourceData() {
        return reinterpret_cast<int( __fastcall* )( zCTexture* )>( GothicMemoryLocations::zCTexture::LoadResourceData )( this );
    }

    zTResourceCacheState GetCacheState() {
        unsigned char state = *reinterpret_cast<unsigned char*>(THISPTR_OFFSET( GothicMemoryLocations::zCTexture::Offset_CacheState ));
        return (zTResourceCacheState)(state & GothicMemoryLocations::zCTexture::Mask_CacheState);
    }
    
    enum zTexFormat : DWORD
    {
        ARGB_8888 = 0,  // 32-bit ARGB pixel format with alpha, using 8 bits per channel
        ABGR_8888 = 1,  // 32-bit ARGB pixel format with alpha, using 8 bits per channel
        RGBA_8888 = 2,  // 32-bit ARGB pixel format with alpha, using 8 bits per channel
        BGRA_8888 = 3,  // 32-bit ARGB pixel format with alpha, using 8 bits per channel
        RGB_888 = 4,  // 24-bit RGB pixel format with 8 bits per channel
        BGR_888 = 5,  // 24-bit RGB pixel format with 8 bits per channel
        ARGB_4444 = 6,  // 16-bit ARGB pixel format with 4 bits for each channel
        ARGB_1555 = 7,  // 16-bit pixel format where 5 bits are reserved for each color and 1 bit is reserved for alpha
        RGB_565 = 8,  // 16-bit RGB pixel format with 5 bits for red, 6 bits for green and 5 bits for blue
        PAL_8 = 9,  // 8-bit color indexed
        DXT1 = 10,  // DXT1 compression texture format
        DXT2 = 11,  // DXT2 compression texture format
        DXT3 = 12,  // DXT3 compression texture format
        DXT4 = 13,  // DXT4 compression texture format
        DXT5 = 14,  // DXT5 compression texture format
    };
    
    struct zTexHeader {
        DWORD magicHeaderZTEX;
        DWORD version;
        zTexFormat format;
        DWORD width;
        DWORD height;
        DWORD mipmap_count;
        DWORD refwidth;
        DWORD refheight;
        DWORD average_color;
    };
    
    DXGI_FORMAT mapFormat(zTexFormat format)
    {
        switch (format)
        {
        case ARGB_8888:
        case ABGR_8888:
        case RGB_888:
        case BGR_888:
            return DXGI_FORMAT::DXGI_FORMAT_UNKNOWN;
        case RGBA_8888:
            return DXGI_FORMAT::DXGI_FORMAT_R8G8B8A8_UNORM;
        case BGRA_8888:
            return DXGI_FORMAT::DXGI_FORMAT_B8G8R8A8_UNORM;
        case ARGB_4444:
            return DXGI_FORMAT::DXGI_FORMAT_B4G4R4A4_UNORM;
        case ARGB_1555:
            return DXGI_FORMAT::DXGI_FORMAT_B5G5R5A1_UNORM;
        case RGB_565:
            return DXGI_FORMAT::DXGI_FORMAT_B5G6R5_UNORM;
        case PAL_8:
            return DXGI_FORMAT::DXGI_FORMAT_P8;
        case DXT1:
            return DXGI_FORMAT::DXGI_FORMAT_BC1_UNORM;
        case DXT2:
        case DXT3:
            return DXGI_FORMAT::DXGI_FORMAT_BC2_UNORM;
        case DXT4:
        case DXT5:
            return DXGI_FORMAT::DXGI_FORMAT_BC3_UNORM;
        }
        
        return DXGI_FORMAT::DXGI_FORMAT_UNKNOWN;
    }
    
    struct texFlags {					
        uint8_t					cacheState		: 2;
        uint8_t					cacheOutLock	: 1;
        uint8_t					cacheClassIndex	: 8;
        uint8_t					managedByResMan	: 1;
        uint16_t				cacheInPriority	: 16;
        uint8_t					canBeCachedOut	: 1;
    };
    
    struct texFlags0 {
        //		zUINT8				inMemory			: 1;				// .. or on hard-disk 
        uint8_t				hasAlpha			: 1;
        uint8_t				isAnimated			: 1;				// texture ani => frame Animation
        uint8_t				changingRealtime	: 1;				// is changing realtime ? (procedural texture)
        uint8_t				isTextureTile		: 1;				// a 'texture-tile', these textures are sliced into separate textures if they are bigger than the maxSize the renderer permits
    };
    
    bool LoadTexture()
    {
        std::string b(255, ' ');
        b.clear();
        b.append(R"(/_work/Data/Textures/_Compiled/)");
        b.append(GetNameWithoutExtView());
        b.append("-C.TEX");
        
        if (auto file = zFILE_VDFS::Create(b.c_str())) {
            if (!file->Exists()) {
                return false;
            }
            if (file->Open(false) != 0) {
             return false;
            }
            long len = file->Size();
            
            zTexHeader header;
            // careful: needs to be run on little endian machine!
            if (file->Read(&header, sizeof(header)) != 36) {
                file->Close();
                return false;
            }
            
            auto mappedFormat = mapFormat(header.format);
            if (mappedFormat == DXGI_FORMAT::DXGI_FORMAT_UNKNOWN) {
                file->Close();
                return false;
            }
            
            thread_local std::vector<uint8_t> imgBuf;
            imgBuf.reserve(len*2);
            
            imgBuf.resize(len-36);
            if (file->Read(imgBuf.data(), imgBuf.size()) != imgBuf.size()) {
                file->Close();
                return false;
            }
            file->Close();

            // .TEX stores the mip chain reversed (smallest mip first, mip 0 last).
            // D3D11 expects mip 0 (largest) first, so rebuild the chain in that order.
            auto mipSizeBytes = [&]( UINT mip ) -> size_t {
                UINT px = header.width >> mip;
                UINT py = header.height >> mip;
                switch ( mappedFormat ) {
                case DXGI_FORMAT::DXGI_FORMAT_R8_UNORM:
                    return static_cast<size_t>(px) * py;
                case DXGI_FORMAT::DXGI_FORMAT_B5G6R5_UNORM:
                case DXGI_FORMAT::DXGI_FORMAT_B5G5R5A1_UNORM:
                case DXGI_FORMAT::DXGI_FORMAT_B4G4R4A4_UNORM:
                    return static_cast<size_t>(px) * py * 2;
                case DXGI_FORMAT::DXGI_FORMAT_BC1_UNORM:
                case DXGI_FORMAT::DXGI_FORMAT_BC2_UNORM:
                case DXGI_FORMAT::DXGI_FORMAT_BC3_UNORM:
                    return Toolbox::GetDDSStorageRequirements( px, py, mappedFormat == DXGI_FORMAT::DXGI_FORMAT_BC1_UNORM );
                default: // 32-bit RGBA/BGRA
                    return static_cast<size_t>(px) * py * 4;
                }
            };

            thread_local std::vector<uint8_t> reordered;
            reordered.reserve(imgBuf.capacity());
            reordered.resize( imgBuf.size() );
            size_t srcOffset = imgBuf.size();
            size_t dstOffset = 0;
            for ( UINT mip = 0; mip < header.mipmap_count; ++mip ) {
                size_t mipSize = mipSizeBytes( mip );
                srcOffset -= mipSize;
                memcpy( reordered.data() + dstOffset, imgBuf.data() + srcOffset, mipSize );
                dstOffset += mipSize;
            }

            std::unique_ptr<D3D11Texture> tex;
            Engine::GraphicsEngine->CreateTexture(tex);
            if (XR_SUCCESS != tex->Init(INT2(header.width, header.height),
                static_cast<D3D11Texture::ETextureFormat>(mappedFormat),
                header.mipmap_count,
                reordered.data(),
                GetName()
            ))
            {
                tex.reset();
                return  false;
            }
            texFlags0* selfFlags = reinterpret_cast<texFlags0*>(THISPTR_OFFSET( GothicMemoryLocations::zCTexture::Offset_Flags ));
            selfFlags->hasAlpha = mappedFormat == DXGI_FORMAT::DXGI_FORMAT_BC2_UNORM;

            auto surface = new MyDirectDrawSurface7();
            IDirectDrawSurface7** ppSurface = reinterpret_cast<IDirectDrawSurface7**>(THISPTR_OFFSET(0xd4));
            if (*ppSurface)
            {
                (*ppSurface)->Release();
            }
            *ppSurface = surface;
            surface->AttachEngineTexture(this, std::move(tex));
            
            texFlags* flags = reinterpret_cast<texFlags*>(THISPTR_OFFSET(0x4c));
            flags->cacheState = 3;
            return true;
        }
        return false;
    }

    static constexpr int zTEX_MAX_ANIS = 3;
    
    // offset into zTEX_MAX_ANIS-count of animation frames
    int* GetNumAniFrames() {
        return *reinterpret_cast<int**>(THISPTR_OFFSET(GothicMemoryLocations::zCTexture::Offset_AniFrames));
    }
    
    int* GetActAniFrame() {
        return *reinterpret_cast<int**>(THISPTR_OFFSET(GothicMemoryLocations::zCTexture::Offset_ActAniFrame));
    }
    zCTexture** GetNextAni() {
        return *reinterpret_cast<zCTexture***>(THISPTR_OFFSET(GothicMemoryLocations::zCTexture::Offset_NextFrame));
    }
    
    zCTexture* GetAniTexture() {
        const int* numAniFrames = GetNumAniFrames();
        const int* actAniFrame = GetActAniFrame();

        zCTexture* tex = this;
        for ( int i = 0; i < zTEX_MAX_ANIS; ++i ) {
            if (numAniFrames[i] == 0) continue;
            
            for (int j = 0; j < actAniFrame[i]; ++j) {
                const auto nextAni = tex->GetNextAni();
                if (!nextAni[i]) {
                    break;
                }
                tex = nextAni[i];
            }
        }
        return tex;
    }
    
    zTResourceCacheState CacheIn( float priority ) {
        zTResourceCacheState cacheState = GetCacheState();
        if ( cacheState == zRES_CACHED_IN ) {
            TouchTimeStamp();
        } else/* if ( cacheState == zRES_CACHED_OUT || zCTextureCacheHack::ForceCacheIn )*/ {
            if (LoadTexture()) {
                return zRES_CACHED_IN;
            } else if (GetSurface() && GetSurface()->IsSurfaceReady()) {
                return zRES_CACHED_IN;
            }
            TouchTimeStampLocal();
            /*zCTextureCacheHack::NumNotCachedTexturesInFrame++;

            if (zCTextureCacheHack::NumNotCachedTexturesInFrame >= zCTextureCacheHack::MAX_NOT_CACHED_TEXTURES_IN_FRAME)
            {
                // Don't let the renderer cache in all textures at once!
                return zRES_CACHED_OUT;
            }*/

#ifndef PUBLIC_RELEASE
            if ( 1 == 0 ) // Small debugger-only section to get the name of currently cachedin texture
            {
                LogInfo() << "CacheIn on Texture: " << GetNameView();
            }
#endif
            Engine::GAPI->SetBoundTexture( 7, this ); // Index 7 is reserved for cacheIn

            // Cache the texture, overwrite priority if wanted.
            zCResourceManager::GetResourceManager()->CacheIn( this, zCTextureCacheHack::ForceCacheIn ? -1 : priority );
        }

        MyDirectDrawSurface7* surface = GetSurface();
        if ( !surface || !surface->IsSurfaceReady() ) {
            if ( zCTextureCacheHack::ForceCacheIn )
                zCResourceManager::GetResourceManager()->CacheIn( this, -1 );
            else
                return zRES_CACHED_OUT;
        }

        return GetCacheState();
    }

    void PrecacheTexAniFrames( float priority ) {
        reinterpret_cast<void( __fastcall* )( zCTexture*, int, float )>
            ( GothicMemoryLocations::zCTexture::PrecacheTexAniFrames )( this, 0, priority );
    }

    void TouchTimeStamp() {
        reinterpret_cast<void( __fastcall* )( zCTexture* )>( GothicMemoryLocations::zCTexture::zCResourceTouchTimeStamp )( this );
    }

    void TouchTimeStampLocal() {
        reinterpret_cast<void( __fastcall* )( zCTexture* )>( GothicMemoryLocations::zCTexture::zCResourceTouchTimeStampLocal )( this );
    }

    bool HasAlphaChannel() {
        unsigned char flags = *reinterpret_cast<unsigned char*>(THISPTR_OFFSET( GothicMemoryLocations::zCTexture::Offset_Flags ));
        return (flags & GothicMemoryLocations::zCTexture::Mask_FlagHasAlpha) != 0;
    }

    const zSTRING& __GetName() const {
        return reinterpret_cast<zSTRING&(__fastcall*)( const zCTexture* )>( GothicMemoryLocations::zCObject::GetObjectName )( this );
    }
};
