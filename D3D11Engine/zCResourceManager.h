#pragma once
#include "pch.h"
#include "HookedFunctions.h"
#include "Engine.h"
#include "GothicAPI.h"

class zCTexture;

enum zTResourceCacheState {
    zRES_FAILURE = -1,
    zRES_CACHED_OUT = 0,
    zRES_QUEUED = 1,
    zRES_LOADING = 2,
    zRES_CACHED_IN = 3
};

class zCResourceManager {
public:

    /** Hooks the functions of this Class */
    static void Hook() {
        //DetourAttachTyped( &HookedFunctions::OriginalFunctions.original_zCResourceManagerCacheOut, hooked_CacheOut  );
    }

    /*
    static void __fastcall hooked_CacheOut( void* thisptr, void* unknwn, class zCResource* res ) {
        hook_infunc

            HookedFunctions::OriginalFunctions.original_zCResourceManagerCacheOut( thisptr, res );

        hook_outfunc
    }
    */

    zTResourceCacheState CacheIn( zCTexture* res, float priority ) {
        return reinterpret_cast<zTResourceCacheState( __fastcall* )( zCResourceManager*, int, zCTexture*, float )>
            ( GothicMemoryLocations::zCResourceManager::CacheIn )( this, 0, res, priority );
    }

    static std::mutex& GetResourceManagerMutex() {
        static std::mutex mutex;
        return mutex;
    }

    void PurgeCaches( unsigned int classDef ) {
        reinterpret_cast<void( __fastcall* )( zCResourceManager*, int, unsigned int )>
            ( GothicMemoryLocations::zCResourceManager::PurgeCaches )( this, 0, classDef );
    }

    void SetThreadingEnabled( bool enabled ) {
        reinterpret_cast<void( __fastcall* )( zCResourceManager*, int, bool )>
            ( GothicMemoryLocations::zCResourceManager::SetThreadingEnabled )( this, 0, enabled );
    }

    static zCResourceManager* GetResourceManager() { return *reinterpret_cast<zCResourceManager**>(GothicMemoryLocations::GlobalObjects::zCResourceManager); }
    
    static void RefreshTexMaxSize(int textureMaxSize) {
        reinterpret_cast<void( __cdecl* )(int)>(GothicMemoryLocations::zCResourceManager::RefreshTexMaxSize)(textureMaxSize);
    }
};
