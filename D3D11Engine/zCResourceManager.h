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
        DetourAttachTyped( &HookedFunctions::OriginalFunctions.original_zCResourceManagerPurgeCaches, hooked_PurgeCaches );
        //DetourAttachTyped( &HookedFunctions::OriginalFunctions.original_zCResourceManagerCacheOut, hooked_CacheOut  );
    }

    /*
    static void __fastcall hooked_CacheOut( void* thisptr, void* unknwn, class zCResource* res ) {
        hook_infunc

            HookedFunctions::OriginalFunctions.original_zCResourceManagerCacheOut( thisptr, res );

        hook_outfunc
    }
    */

    /** ZENGIN's PurgeCaches hangs the game thread forever if a class cache contains a resource its
        CacheOut cannot unlink. Its per-class-cache loop (G2 2.6: 0x005DCBD1-0x005DCBEF) reads:

            res = classCache->resListHead;
            while ( res ) {
                next = res->nextRes;
                if ( res->cacheOutLock ) { res = next; continue; }
                zresMan->CacheOut( res );
                res = classCache->resListHead;  // [Moos] start over, nextRes may already be freed
            }

        It restarts from the head after every CacheOut and never checks that the head actually moved.
        zCResourceManager::CacheOut (0x005DD350) only unlinks in one single case - cacheState is
        zRES_CACHED_IN *and* managedByResMan is set, because zCClassCache::RemoveRes (0x005DE3F0) has
        its entire body wrapped in `if (managedByResMan)`. In every other state CacheOut returns
        having left the class cache untouched:
            zRES_CACHED_OUT -> early-out at 0x005DD3AE, unlock and return
            zRES_QUEUED     -> removes from the cache-in queue only, not from the class cache
            zRES_CACHED_IN without managedByResMan -> RemoveRes is a no-op

        So one linked resource in any of those states pins the loop on the list head: CacheOut does
        nothing, the head is re-read, the same resource comes back, forever. That is the hang seen
        when toggling Normalmaps (which runs CacheIn traffic and then PurgeCaches): the game thread
        spins in PurgeCaches+0x1B6 -> CacheOut+0x68 -> zCCriticalSection::Unlock while the resource
        thread sits parked in ::SuspendThread - PurgeCaches asked it to suspend via goToSuspend and
        only resumes it after the loop it never leaves, so nothing can change the state that would
        break the spin. It needs a few tries to hit because producing the stale state is a race
        between the game thread and the resource thread (CacheIn writes managedByResMan/refCtr with
        no lock at all, and CacheOut clears them at 0x005DD468 *after* releasing the state lock at
        0x005DD43C).

        Fix: walk the class caches before delegating and set the cacheOutLock bit on every resource
        ZENGIN's loop would not be able to remove. It honours that bit and advances to nextRes
        (0x005DCBD8), so the loop is guaranteed to make progress. The bits are cleared again after,
        so nothing stays pinned - those resources simply survive this purge, which is what ZENGIN's
        own bookkeeping already claims about them. */
    static void __fastcall hooked_PurgeCaches( zCResourceManager* thisptr, void* unknwn, unsigned int classDef ) {
        hook_infunc

            PurgeCachesWithProgressGuarantee( thisptr, classDef );

        hook_outfunc
    }

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

private:
    enum : unsigned char {
        kCacheStateMask = 0x3,
        kCacheOutLockBit = 0x4,
        kManagedByResManBit = 0x1,
    };

    static unsigned char* ResFlags( void* res ) {
        return reinterpret_cast<unsigned char*>(PTR_OFFSET( res, GothicMemoryLocations::zCResource::Offset_Flags ));
    }

    static void* ResNext( void* res ) {
        return *reinterpret_cast<void**>(PTR_OFFSET( res, GothicMemoryLocations::zCResource::Offset_NextRes ));
    }

    /** True if ZENGIN's CacheOut would actually unlink this resource from its class cache. Anything
        else leaves the list unchanged and would spin PurgeCaches' restart-from-head loop. */
    static bool CanZenGinUnlink( void* res ) {
        const unsigned char* flags = ResFlags( res );
        const bool cachedIn = (flags[0] & kCacheStateMask) == zRES_CACHED_IN;
        const bool managedByResMan = (flags[GothicMemoryLocations::zCResource::Offset_Flags_ManagedByResMan
            - GothicMemoryLocations::zCResource::Offset_Flags] & kManagedByResManBit) != 0;
        return cachedIn && managedByResMan;
    }

    /** Park ZENGIN's resource thread the same way PurgeCaches itself does, so the class cache lists
        are not being mutated by LoadResource()/InsertRes() while we walk them. PurgeCaches redoes
        this handshake immediately afterwards (and clears goToSuspend at the end), so leaving the
        flag set here is exactly the state it expects. Bounded, unlike ZENGIN's `while(...);` - if
        the thread does not park we simply skip the marking pass and behave like vanilla. */
    static bool ParkResourceThread( zCResourceManager* rsm ) {
        *reinterpret_cast<volatile int*>(PTR_OFFSET( rsm, GothicMemoryLocations::zCResourceManager::Offset_GoToSuspend )) = 1;

        void** vtbl = *reinterpret_cast<void***>(rsm);
        auto isThreadRunning = reinterpret_cast<int( __fastcall* )(zCResourceManager*, int)>(
            vtbl[GothicMemoryLocations::zCResourceManager::VTBL_IsThreadRunning / sizeof( void* )] );
        auto suspendCount = reinterpret_cast<volatile int*>(PTR_OFFSET( rsm, GothicMemoryLocations::zCThread::Offset_SuspendCount ));

        // ~2s is far longer than any single LoadResourceData, but still terminates.
        for ( int i = 0; i < 2000; ++i ) {
            if ( !isThreadRunning( rsm, 0 ) || *suspendCount != 0 )
                return true;
            Sleep( 1 );
        }
        LogWarn() << "zCResourceManager::PurgeCaches: resource thread did not suspend - skipping the "
            "cache-list sanity pass and running ZENGIN's PurgeCaches unguarded";
        return false;
    }

    static void PurgeCachesWithProgressGuarantee( zCResourceManager* rsm, unsigned int classDef ) {
        // A corrupt (cyclic) list must not hang us either - this is generous, the texture cache holds
        // a few thousand entries at most.
        constexpr size_t MAX_RESOURCES_PER_CACHE = 1u << 20;

        std::vector<void*> lockedByUs;

        if ( ParkResourceThread( rsm ) ) {
            unsigned char* caches = *reinterpret_cast<unsigned char**>(
                PTR_OFFSET( rsm, GothicMemoryLocations::zCResourceManager::Offset_ClassCacheList ));
            const int numCaches = *reinterpret_cast<int*>(
                PTR_OFFSET( rsm, GothicMemoryLocations::zCResourceManager::Offset_ClassCacheCount ));

            for ( int i = 0; caches && i < numCaches; ++i ) {
                unsigned char* cache = caches + i * GothicMemoryLocations::zCResourceManager::SizeOf_ClassCache;

                // Same filter PurgeCaches applies: classDef 0 purges everything.
                const unsigned int cacheClassDef = *reinterpret_cast<unsigned int*>(
                    cache + GothicMemoryLocations::zCResourceManager::Offset_ClassCache_ResClassDef );
                if ( classDef != 0 && classDef != cacheClassDef )
                    continue;

                void* res = *reinterpret_cast<void**>(
                    cache + GothicMemoryLocations::zCResourceManager::Offset_ClassCache_ResListHead );

                size_t visited = 0;
                while ( res && ++visited <= MAX_RESOURCES_PER_CACHE ) {
                    void* next = ResNext( res );
                    unsigned char* flags = ResFlags( res );

                    // Resources ZENGIN already skips are fine - it advances to nextRes for those.
                    if ( !(flags[0] & kCacheOutLockBit) && !CanZenGinUnlink( res ) ) {
                        flags[0] |= kCacheOutLockBit;
                        lockedByUs.push_back( res );
                    }
                    res = next;
                }

                if ( visited > MAX_RESOURCES_PER_CACHE ) {
                    LogWarn() << "zCResourceManager::PurgeCaches: class cache " << i << " list did not "
                        "terminate after " << MAX_RESOURCES_PER_CACHE << " entries - it is cyclic, aborting the walk";
                }
            }

            if ( !lockedByUs.empty() ) {
                LogWarn() << "zCResourceManager::PurgeCaches: " << lockedByUs.size() << " resource(s) are "
                    "linked into a class cache but not in a state ZENGIN's CacheOut can unlink "
                    "(stale cacheState/managedByResMan). Skipping them - purging them would spin "
                    "PurgeCaches' restart-from-head loop forever.";
            }
        }

        HookedFunctions::OriginalFunctions.original_zCResourceManagerPurgeCaches( rsm, classDef );

        for ( void* res : lockedByUs )
            ResFlags( res )[0] &= static_cast<unsigned char>(~kCacheOutLockBit);
    }

public:
    
    static void RefreshTexMaxSize(int textureMaxSize) {
        reinterpret_cast<void( __cdecl* )(int)>(GothicMemoryLocations::zCResourceManager::RefreshTexMaxSize)(textureMaxSize);
    }
};
