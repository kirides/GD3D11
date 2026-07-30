#pragma once
#include "pch.h"
#include "GothicMemoryLocations.h"

enum zTResourceCacheState {
    zRES_FAILURE = -1,
    zRES_CACHED_OUT = 0,
    zRES_QUEUED = 1,
    zRES_LOADING = 2,
    zRES_CACHED_IN = 3
};

/** ZENGIN's zCResource - the base of every zCResourceManager-managed resource (zCTexture,
    zCSoundFX, zCModelPrototype, ...). We only wrap what the PurgeCaches fix in
    zCResourceManager.h needs; this is not a full binding.

    Layout (verified against Gothic2.exe 2.6, and consistent with zCTexture::Offset_CacheState
    which points at the very same byte):

        +0x04  refCtr                   (zCObject)
        +0x24  nextRes                  class cache / cache-in queue forward link
        +0x28  prevRes
        +0x2C  timeStamp
        +0x30  stateChangeGuard         zCCriticalSection (vtbl + CRITICAL_SECTION = 0x1C bytes)
        +0x4C  bitfield byte 0          bits 0-1 cacheState, bit 2 cacheOutLock
        +0x4D  bitfield byte 1          cacheClassIndex
        +0x4E  bitfield byte 2          bit 0 managedByResMan */
class zCResource {
public:
    zCResource* GetNextRes() {
        return *reinterpret_cast<zCResource**>(THISPTR_OFFSET( GothicMemoryLocations::zCResource::Offset_NextRes ));
    }

    zTResourceCacheState GetCacheState() {
        return static_cast<zTResourceCacheState>(Flags()[0] & Mask_CacheState);
    }

    /** Resources with this set are skipped by CacheOut, Evict and - importantly for us -
        PurgeCaches, which advances to nextRes instead (G2 2.6: 0x005DCBD1/0x005DCBD8). */
    bool GetCacheOutLock() {
        return (Flags()[0] & Bit_CacheOutLock) != 0;
    }

    void SetCacheOutLock( bool locked ) {
        if ( locked )
            Flags()[0] |= Bit_CacheOutLock;
        else
            Flags()[0] &= static_cast<unsigned char>(~Bit_CacheOutLock);
    }

    /** Set by zCResourceManager::CacheIn when the resMan takes co-ownership. zCClassCache::RemoveRes
        (G2 2.6: 0x005DE3F0) has its *entire* body wrapped in `if (managedByResMan)`, so a resource
        that is linked into a class cache without this flag can never be unlinked again. */
    bool GetManagedByResMan() {
        return (Flags()[Byte_ManagedByResMan] & Bit_ManagedByResMan) != 0;
    }

    /** True if zCResourceManager::CacheOut would actually unlink this resource from its class cache.
        Every other combination leaves the list untouched - see the comment on
        zCResourceManager::hooked_PurgeCaches for why that is fatal. */
    bool IsRemovableFromClassCache() {
        return GetCacheState() == zRES_CACHED_IN && GetManagedByResMan();
    }

    /** zCResource::LockStateChange/UnlockStateChange - the zCCriticalSection guarding every
        cacheState change. Needed for more than just ordering: cacheState and cacheOutLock share the
        byte at +0x4C, and ZENGIN writes that byte *whole* under this lock (CacheOut 0x005DD439
        `AND BL,0xFC`, ThreadProc for LOADING/CACHED_IN), so an unlocked read-modify-write on our
        side can drop a bit or clobber a state change. */
    void LockStateChange() {
        void* guard = StateChangeGuard();
        void** vtbl = *reinterpret_cast<void***>(guard);
        reinterpret_cast<int( __fastcall* )( void*, int, unsigned int )>(vtbl[VTBL_SyncObject_Lock])( guard, 0, INFINITE );
    }

    void UnlockStateChange() {
        void* guard = StateChangeGuard();
        void** vtbl = *reinterpret_cast<void***>(guard);
        reinterpret_cast<int( __fastcall* )( void*, int )>(vtbl[VTBL_SyncObject_Unlock])( guard, 0 );
    }

    class ScopedStateChangeLock {
    public:
        explicit ScopedStateChangeLock( zCResource* res ) : Res( res ) { Res->LockStateChange(); }
        ~ScopedStateChangeLock() { Res->UnlockStateChange(); }
        ScopedStateChangeLock( const ScopedStateChangeLock& ) = delete;
        ScopedStateChangeLock& operator=( const ScopedStateChangeLock& ) = delete;
    private:
        zCResource* Res;
    };

private:
    enum : unsigned char {
        Mask_CacheState = 0x3,
        Bit_CacheOutLock = 0x4,
        Bit_ManagedByResMan = 0x1,
    };

    // zCSyncObject's vtable is { ~dtor, Lock(timeOutMSec), Unlock() }, verified in CacheOut:
    // 0x005DD363 `CALL [EAX+0x4]` with -1 pushed, 0x005DD43C `CALL [EAX+0x8]`.
    enum { VTBL_SyncObject_Lock = 1, VTBL_SyncObject_Unlock = 2 };

    // Byte index of the managedByResMan bitfield byte, relative to the byte cacheState lives in.
    enum { Byte_ManagedByResMan = GothicMemoryLocations::zCResource::Offset_Flags_ManagedByResMan
                                - GothicMemoryLocations::zCResource::Offset_Flags };

    unsigned char* Flags() {
        return reinterpret_cast<unsigned char*>(THISPTR_OFFSET( GothicMemoryLocations::zCResource::Offset_Flags ));
    }

    void* StateChangeGuard() {
        return reinterpret_cast<void*>(THISPTR_OFFSET( GothicMemoryLocations::zCResource::Offset_StateChangeGuard ));
    }
};
