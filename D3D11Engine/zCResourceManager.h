#pragma once
#include "pch.h"
#include "HookedFunctions.h"
#include "Engine.h"
#include "GothicAPI.h"
#include "zCResource.h"
#include "zCClassDef.h"
#include <unordered_set>

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
        /** Diagnostics only - same field as GetResClassDef(), read as the zCClassDef* it actually is
            so we can print which resource class (zCTexture, zCSoundFX, ...) a log line is about. */
        std::string GetResClassDefName() {
            zCClassDef* def = *reinterpret_cast<zCClassDef**>(THISPTR_OFFSET( GothicMemoryLocations::zCResourceManager::Offset_ClassCache_ResClassDef ));
            if ( !def ) return "<null>";
            return std::string( def->className.ToChar(), static_cast<size_t>(def->className.Length()) );
        }
        zCResource* GetResListHead() {
            return *reinterpret_cast<zCResource**>(THISPTR_OFFSET( GothicMemoryLocations::zCResourceManager::Offset_ClassCache_ResListHead ));
        }
    };


    /** Hooks the functions of this Class */
    static void Hook() {
        DetourAttachTyped( &HookedFunctions::OriginalFunctions.original_zCResourceManagerPurgeCaches, hooked_PurgeCaches );
        //DetourAttachTyped( &HookedFunctions::OriginalFunctions.original_zCResourceManagerCacheOut, hooked_CacheOut  );
#if defined(BUILD_GOTHIC_1_08k) || defined(BUILD_GOTHIC_2_6_fix)
        DetourAttachTyped( &HookedFunctions::OriginalFunctions.original_zCClassCacheInsertRes, hooked_InsertRes );
        DetourAttachTyped( &HookedFunctions::OriginalFunctions.original_zCClassCacheTouchRes, hooked_TouchRes );
        DetourAttachTyped( &HookedFunctions::OriginalFunctions.original_zCClassCacheRemoveRes, hooked_RemoveRes );
#endif
    }

    /*
    static void __fastcall hooked_CacheOut( void* thisptr, void* unknwn, class zCResource* res ) {
        hook_infunc

            HookedFunctions::OriginalFunctions.original_zCResourceManagerCacheOut( thisptr, res );

        hook_outfunc
    }
    */

#if defined(BUILD_GOTHIC_1_08k) || defined(BUILD_GOTHIC_2_6_fix)
    /** zCClassCache::InsertRes/TouchRes/RemoveRes all mutate the SAME unlocked
        resListHead/resListTail/prevRes/nextRes for a class cache, and vanilla ZENGIN never
        synchronizes them against each other - confirmed live: InsertRes (fired on the resource-loader
        thread, from ThreadProc's inlined LoadResource) raced TouchRes (fired on another thread via
        zCResource::TouchTimeStamp, called from CacheIn() on an already-CACHED_IN resource) for the
        SAME class cache but different resources - no overlap on the resource itself needed, since
        both write the shared head/tail pointers. That interleaving is what corrupted the list into a
        cycle, which zCResourceManager::PurgeCaches' hooked_PurgeCaches below routes around as a
        second line of defense.

        Fix: one mutex serializing all three, covering the whole call (not just the pointer surgery) so
        no partial state is ever observable. Recursive because InsertRes legitimately calls back into
        TouchTimeStamp/TouchRes on the same thread before returning - a plain std::mutex would deadlock
        (or throw, on MSVC's checked STL) on that self-reentry; recursive_mutex allows it while still
        fully blocking any OTHER thread until the outermost call releases it. */
    static std::recursive_mutex& GetClassCacheListMutex() {
        static std::recursive_mutex mutex;
        return mutex;
    }

    static void __fastcall hooked_InsertRes( zCClassCache* thisptr, void* unknwn, zCResource* res ) {
        std::scoped_lock lock( GetClassCacheListMutex() );
        HookedFunctions::OriginalFunctions.original_zCClassCacheInsertRes( thisptr, res );
    }

    static void __fastcall hooked_TouchRes( zCClassCache* thisptr, void* unknwn, zCResource* res ) {
        std::scoped_lock lock( GetClassCacheListMutex() );
        HookedFunctions::OriginalFunctions.original_zCClassCacheTouchRes( thisptr, res );
    }

    static void __fastcall hooked_RemoveRes( zCClassCache* thisptr, void* unknwn, zCResource* res ) {
        std::scoped_lock lock( GetClassCacheListMutex() );
        HookedFunctions::OriginalFunctions.original_zCClassCacheRemoveRes( thisptr, res );
    }
#endif

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
        are not being mutated by LoadResource()/InsertRes() while we walk them. Vanilla PurgeCaches
        only ever clears goToSuspend at the very end of its own body (0x005DCC29) - it never checks
        who else might have set it. We call it conditionally (skipping corrupted class caches, or not
        at all if parking itself failed), so WE own clearing goToSuspend again; ClearGoToSuspendGuard
        below is what actually guarantees that, not this function. Bounded, unlike ZENGIN's
        `while(...);` - if the thread does not park in time we report that but still leave goToSuspend
        set, same as vanilla would while it is mid-wait; the guard clears it either way. */
    bool ParkResourceThread() {
        SetGoToSuspend( true );

        // ~2s is far longer than any single LoadResourceData, but still terminates.
        for ( int i = 0; i < 2000; ++i ) {
            if ( !IsThreadRunning() || GetSuspendCount() != 0 )
                return true;
            Sleep( 1 );
        }

        LogWarn() << "zCResourceManager::PurgeCaches: resource thread did not suspend in time";
        return false;
    }

    /** ParkResourceThread() sets goToSuspend=true; only vanilla PurgeCaches' own body ever sets it
        back to false. We do not always end up calling vanilla PurgeCaches (corrupted-class-cache
        skip, or ParkResourceThread() itself timing out) - if goToSuspend is left stuck true in those
        cases the loader thread parks and can never leave zCThread::SuspendThread again (its ThreadProc
        rechecks goToSuspend before touching the next resource and re-suspends immediately), so every
        future blocking CacheIn()/WaitForCacheIn() spins forever waiting on a worker that will never
        run. This guard makes sure we always put goToSuspend back, on every exit path. */
    struct ClearGoToSuspendGuard {
        explicit ClearGoToSuspendGuard( zCResourceManager* mgr ) : Mgr( mgr ) {}
        ~ClearGoToSuspendGuard() { Mgr->SetGoToSuspend( false ); }
        ClearGoToSuspendGuard( const ClearGoToSuspendGuard& ) = delete;
        ClearGoToSuspendGuard& operator=( const ClearGoToSuspendGuard& ) = delete;
    private:
        zCResourceManager* Mgr;
    };

    struct ScanResult {
        std::vector<zCResource*> lockedByUs;
        // Indices into classCacheList whose resListHead is cyclic/did not terminate. ZENGIN's own
        // PurgeCaches loop has no bound at all, so calling it on one of these hangs for real - the
        // caller must skip these class caches entirely rather than delegate to it.
        std::vector<int> corruptedClassCacheIndices;
    };

    /** Sets cacheOutLock on every resource ZENGIN's CacheOut could not unlink, so its
        restart-from-head loop is forced to advance. Returns the resources we touched, so the caller
        can undo it once the purge is over, plus any class cache whose list turned out to be cyclic
        (a visited-set, not just a counter, so a legitimately large class cache is never mistaken for
        a corrupt one). */
    ScanResult LockOutUnremovableResources( unsigned int classDef ) {
        // Generous cap - the texture cache holds a few thousand entries at most - so a corrupt
        // (cyclic) list can't hang us even if it never repeats a node we've already tracked.
        constexpr size_t MAX_RESOURCES_PER_CACHE = 1u << 20;

        ScanResult result;
        const int numCaches = GetNumClassCaches();

        for ( int i = 0; i < numCaches; ++i ) {
            zCClassCache* cache = GetClassCache( i );
            if ( !cache ) break;

            // Same filter PurgeCaches applies: classDef 0 purges every class cache.
            if ( classDef != 0 && classDef != cache->GetResClassDef() )
                continue;

            std::unordered_set<zCResource*> seen;
            zCResource* res = cache->GetResListHead();
            bool corrupted = false;

            while ( res ) {
                if ( !seen.insert( res ).second ) {
                    corrupted = true;
                    break;
                }
                if ( seen.size() > MAX_RESOURCES_PER_CACHE ) {
                    corrupted = true;
                    break;
                }

                zCResource* next = res->GetNextRes();

                zCResource::ScopedStateChangeLock lock( res );
                // ZENGIN's own PurgeCaches/CacheOut/Evict never write this bit - the only writer is
                // this hook. So a resource that is removable but still shows cacheOutLock==true is a
                // stale bit left over from an earlier call rather than something we must leave alone;
                // left alone it would exempt the resource from being purged forever, so self-heal it
                // here instead of only ever adding bits.
                if ( res->IsRemovableFromClassCache() ) {
                    if ( res->GetCacheOutLock() )
                        res->SetCacheOutLock( false );
                } else if ( !res->GetCacheOutLock() ) {
                    res->SetCacheOutLock( true );
                    result.lockedByUs.push_back( res );
                }

                res = next;
            }

            if ( corrupted ) {
                LogWarn() << "zCResourceManager::PurgeCaches: class cache " << i << " (\""
                    << cache->GetResClassDefName() << "\") has a corrupted resource list - excluding "
                    "it from this purge.";
                result.corruptedClassCacheIndices.push_back( i );
            }
        }

        return result;
    }

    /** If the resource thread will not park, the class-cache lists are not ours to walk safely (the
        loader thread mutates nextRes/resListHead via InsertRes/RemoveRes with no lock we can take on
        the list itself - only ParkResourceThread's OS-level suspend actually stops that). Calling
        ZENGIN's PurgeCaches unguarded in that state reopens the exact livelock this hook exists to
        prevent, so we skip the purge entirely rather than gamble on the timing. It is safe to skip:
        this is a manual/best-effort reclaim (texture quality change, Reload Textures) and
        zCResourceManager::Evict() already reclaims over-budget caches every frame regardless. */
    void PurgeCachesWithProgressGuarantee( unsigned int classDef ) {
        bool parked = ParkResourceThread();
        // Must be constructed unconditionally, after ParkResourceThread() (which sets goToSuspend
        // true regardless of whether it times out) and before any early return - see the class
        // comment for why leaving goToSuspend stuck true hangs every future blocking CacheIn().
        ClearGoToSuspendGuard clearGoToSuspend( this );

        if ( !parked ) {
            LogWarn() << "zCResourceManager::PurgeCaches: skipping this purge entirely - the class "
                "cache lists cannot be scanned safely while the resource thread is still running";
            return;
        }

        ScanResult scan = LockOutUnremovableResources( classDef );

        if ( scan.corruptedClassCacheIndices.empty() ) {
            HookedFunctions::OriginalFunctions.original_zCResourceManagerPurgeCaches( this, classDef );
        } else {
            // At least one class cache in scope is cyclic. ZENGIN's PurgeCaches takes a single
            // classDef filter with no way to exclude one class cache from an otherwise-wide purge,
            // so delegate per class instead of once, skipping the corrupted ones - that still purges
            // everything else PurgeCaches(classDef) was asked to purge instead of giving up entirely.
            const int numCaches = GetNumClassCaches();
            for ( int i = 0; i < numCaches; ++i ) {
                zCClassCache* cache = GetClassCache( i );
                if ( !cache ) break;

                unsigned int thisClassDef = cache->GetResClassDef();
                if ( classDef != 0 && classDef != thisClassDef )
                    continue;

                if ( std::find( scan.corruptedClassCacheIndices.begin(), scan.corruptedClassCacheIndices.end(), i )
                    != scan.corruptedClassCacheIndices.end() ) {
                    continue;
                }

                HookedFunctions::OriginalFunctions.original_zCResourceManagerPurgeCaches( this, thisClassDef );
            }
        }

        for ( zCResource* res : scan.lockedByUs ) {
            zCResource::ScopedStateChangeLock lock( res );
            res->SetCacheOutLock( false );
        }
    }

public:

    static void RefreshTexMaxSize(int textureMaxSize) {
        reinterpret_cast<void( __cdecl* )(int)>(GothicMemoryLocations::zCResourceManager::RefreshTexMaxSize)(textureMaxSize);
    }
};
