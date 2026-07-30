#pragma once
#include "pch.h"
#include "HookedFunctions.h"
#include "Engine.h"
#include "GothicAPI.h"
#include "zCResource.h"

class zCTexture;

class zCResourceManager {
public:
    /** zCResourceManager::zCClassCache - one per zCLASS_FLAG_RESOURCE class, held in the
        classCacheList zCArray that follows the zCThread base. Only the two fields the PurgeCaches
        fix needs are wrapped. */
    class zCClassCache {
    public:
        unsigned int GetResClassDef() {
            return *reinterpret_cast<unsigned int*>(THISPTR_OFFSET( GothicMemoryLocations::zCResourceManager::Offset_ClassCache_ResClassDef ));
        }
        zCResource* GetResListHead() {
            return *reinterpret_cast<zCResource**>(THISPTR_OFFSET( GothicMemoryLocations::zCResourceManager::Offset_ClassCache_ResListHead ));
        }
    };


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

            thisptr->PurgeCachesWithProgressGuarantee( classDef );

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

    zCClassCache* GetClassCache( int index ) {
        auto caches = *reinterpret_cast<unsigned char**>(THISPTR_OFFSET( GothicMemoryLocations::zCResourceManager::Offset_ClassCacheList ));
        if ( !caches ) return nullptr;
        return reinterpret_cast<zCClassCache*>(caches + index * GothicMemoryLocations::zCResourceManager::SizeOf_ClassCache);
    }

    int GetNumClassCaches() {
        return *reinterpret_cast<int*>(THISPTR_OFFSET( GothicMemoryLocations::zCResourceManager::Offset_ClassCacheCount ));
    }

    /** volatile zBOOL goToSuspend - asks the resource thread to park itself at the top of its
        ThreadProc loop. PurgeCaches and the immediate-CacheIn path both use this. */
    void SetGoToSuspend( bool suspend ) {
        *reinterpret_cast<volatile int*>(THISPTR_OFFSET( GothicMemoryLocations::zCResourceManager::Offset_GoToSuspend )) = suspend ? 1 : 0;
    }

    /** zCThread::IsSuspended() - values greater than 0 indicate suspended mode. */
    int GetSuspendCount() {
        return *reinterpret_cast<volatile int*>(THISPTR_OFFSET( GothicMemoryLocations::zCThread::Offset_SuspendCount ));
    }

    bool IsThreadRunning() {
        void** vtbl = *reinterpret_cast<void***>(this);
        return reinterpret_cast<int( __fastcall* )( zCResourceManager*, int )>(
            vtbl[GothicMemoryLocations::zCResourceManager::VTBL_IsThreadRunning / sizeof( void* )] )( this, 0 ) != 0;
    }

private:
    /** Park ZENGIN's resource thread the same way PurgeCaches itself does, so the class cache lists
        are not being mutated by LoadResource()/InsertRes() while we walk them. PurgeCaches redoes
        this handshake immediately afterwards (and clears goToSuspend at the end), so leaving the
        flag set here is exactly the state it expects. Bounded, unlike ZENGIN's `while(...);` - if
        the thread does not park we skip the marking pass and behave like vanilla. */
    bool ParkResourceThread() {
        SetGoToSuspend( true );

        // ~2s is far longer than any single LoadResourceData, but still terminates.
        for ( int i = 0; i < 2000; ++i ) {
            if ( !IsThreadRunning() || GetSuspendCount() != 0 )
                return true;
            Sleep( 1 );
        }

        LogWarn() << "zCResourceManager::PurgeCaches: resource thread did not suspend - skipping the "
            "cache-list sanity pass and running ZENGIN's PurgeCaches unguarded";
        return false;
    }

    /** Sets cacheOutLock on every resource ZENGIN's CacheOut could not unlink, so its
        restart-from-head loop is forced to advance. Returns the resources we touched, so the caller
        can undo it once the purge is over. */
    std::vector<zCResource*> LockOutUnremovableResources( unsigned int classDef ) {
        // A corrupt (cyclic) list must not hang us either - this is generous, the texture cache
        // holds a few thousand entries at most.
        constexpr size_t MAX_RESOURCES_PER_CACHE = 1u << 20;

        std::vector<zCResource*> lockedByUs;
        const int numCaches = GetNumClassCaches();

        for ( int i = 0; i < numCaches; ++i ) {
            zCClassCache* cache = GetClassCache( i );
            if ( !cache ) break;

            // Same filter PurgeCaches applies: classDef 0 purges every class cache.
            if ( classDef != 0 && classDef != cache->GetResClassDef() )
                continue;

            size_t visited = 0;
            zCResource* res = cache->GetResListHead();
            while ( res && ++visited <= MAX_RESOURCES_PER_CACHE ) {
                zCResource* next = res->GetNextRes();

                zCResource::ScopedStateChangeLock lock( res );
                // Resources ZENGIN already skips are fine - it advances to nextRes for those - and
                // we must not restore a bit we did not set.
                if ( !res->GetCacheOutLock() && !res->IsRemovableFromClassCache() ) {
                    res->SetCacheOutLock( true );
                    lockedByUs.push_back( res );
                }

                res = next;
            }

            if ( visited > MAX_RESOURCES_PER_CACHE ) {
                LogWarn() << "zCResourceManager::PurgeCaches: class cache " << i << " list did not terminate "
                    "after " << MAX_RESOURCES_PER_CACHE << " entries - it is cyclic, aborting the walk";
            }
        }

        if ( !lockedByUs.empty() ) {
            LogWarn() << "zCResourceManager::PurgeCaches: " << lockedByUs.size() << " resource(s) are linked "
                "into a class cache but not in a state ZENGIN's CacheOut can unlink (stale "
                "cacheState/managedByResMan). Skipping them - purging them would spin PurgeCaches' "
                "restart-from-head loop forever.";
        }

        return lockedByUs;
    }

    void PurgeCachesWithProgressGuarantee( unsigned int classDef ) {
        std::vector<zCResource*> lockedByUs;
        if ( ParkResourceThread() )
            lockedByUs = LockOutUnremovableResources( classDef );

        HookedFunctions::OriginalFunctions.original_zCResourceManagerPurgeCaches( this, classDef );

        for ( zCResource* res : lockedByUs ) {
            zCResource::ScopedStateChangeLock lock( res );
            res->SetCacheOutLock( false );
        }
    }

public:

    static void RefreshTexMaxSize(int textureMaxSize) {
        reinterpret_cast<void( __cdecl* )(int)>(GothicMemoryLocations::zCResourceManager::RefreshTexMaxSize)(textureMaxSize);
    }
};
