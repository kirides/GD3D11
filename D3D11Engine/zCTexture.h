#pragma once
#include "pch.h"
#include "HookedFunctions.h"
#include "Engine.h"
#include "GothicAPI.h"
#include "zCResourceManager.h"
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
        // Publish the texture so the MyDirectDrawSurface7::Unlock calls the original loader makes can
        // find out who they belong to. Thread-local: this runs on the game thread *and* on ZENGIN's
        // resource-manager thread.
        GothicAPI::ScopedLoadingTexture loading( thisptr );

        // TODO: Figure out why some DTX1a Textures crash this
        return HookedFunctions::OriginalFunctions.ofiginal_zCTextureLoadResourceData( thisptr );
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
        std::string_view n = GetNameView();

        auto p = n.find_last_of( '.' );

        if ( p != std::string_view::npos )
            return n.substr( 0, p );

        return n;
    }

    MyDirectDrawSurface7* GetSurface() {
        return *reinterpret_cast<MyDirectDrawSurface7**>(THISPTR_OFFSET( GothicMemoryLocations::zCTexture::Offset_Surface ));
    }

    void Bind( int slot = 0 ) {
        Engine::GAPI->SetBoundTexture( slot, this );

        reinterpret_cast<void(__fastcall*)( zCTexture*, int, bool, int )>
            ( GothicMemoryLocations::zCTexture::zCTex_D3DInsertTexture )( this, 0, false, slot );
    }

    int LoadResourceData() {
        return reinterpret_cast<int( __fastcall* )( zCTexture* )>( GothicMemoryLocations::zCTexture::LoadResourceData )( this );
    }

    zTResourceCacheState GetCacheState() {
        unsigned char state = *reinterpret_cast<unsigned char*>(THISPTR_OFFSET( GothicMemoryLocations::zCTexture::Offset_CacheState ));
        return (zTResourceCacheState)(state & GothicMemoryLocations::zCTexture::Mask_CacheState);
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
            // Scoped: with the resource-manager thread running, CacheIn only *queues* the texture and
            // the load happens later on that thread. Leaving this set past the call would make the next
            // unrelated surface unlocked on this thread believe it belongs to this texture and latch its
            // name forever. hooked_LoadResourceData publishes it again on whichever thread does load it.
            GothicAPI::ScopedLoadingTexture loading( this );

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
