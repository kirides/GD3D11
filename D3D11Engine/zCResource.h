#pragma once


template<typename T>
concept zCResourceHasGetStaticClassDef = requires {
    { T::GetStaticClassDef() } -> std::same_as<const zCClassDef*>;
};

class zCCriticalSection {
public:
    virtual ~zCCriticalSection()      = 0;
    virtual int Lock( unsigned long = 0xFFFFFFFF ) = 0;
    virtual int Unlock()              = 0;
};

class zCResource
{
public:
    zCCriticalSection& GetCriticalSection() {
        return *reinterpret_cast<zCCriticalSection*>(THISPTR_OFFSET( GothicMemoryLocations::zCResource::Offset_zCCriticalSection ));
    }

    struct texCacheFlags {
        uint8_t					cacheState : 2;
        uint8_t					cacheOutLock : 1;
        uint8_t					cacheClassIndex : 8;
        uint8_t					managedByResMan : 1;
        uint16_t				cacheInPriority : 16;
        uint8_t					canBeCachedOut : 1;
    };

    texCacheFlags& GetCacheFlags() {
        return *reinterpret_cast<texCacheFlags*>(THISPTR_OFFSET( GothicMemoryLocations::zCResource::Offset_CacheStateFlags )); // same offset in G1
    }

    template<zCResourceHasGetStaticClassDef T>
    T* As() {
        const zCObject* obj = reinterpret_cast<zCObject*>(this);
        zCClassDef* classDef = obj->_GetClassDef();
        if ( obj->CheckInheritance( classDef, T::GetStaticClassDef() ) ) {
            return reinterpret_cast<T*>(this);
        }
        return nullptr;
    }
};

