#pragma once
#include "pch.h"
#include "GothicMemoryLocations.h"
#include "zAllocator.h"

#pragma pack(push, 1)

class zSTRING {
public:
    zSTRING() {
        reinterpret_cast<void( __fastcall* )( zSTRING* )>( GothicMemoryLocations::zSTRING::ConstructorEmptyPtr )( this );
    }
    zSTRING( const char* str ) {
        reinterpret_cast<void( __fastcall* )(zSTRING*, int, const char*)>(GothicMemoryLocations::zSTRING::ConstructorCharPtr)(this, 0, str);
    }

    zSTRING(const zSTRING& other)
        : zSTRING(other.ToChar()) {}

    zSTRING& operator=(const zSTRING& other) = delete;

    ~zSTRING() {
        reinterpret_cast<void( __fastcall* )(zSTRING*)>(GothicMemoryLocations::zSTRING::DestructorCharPtr)(this);
    }

    static void* operator new(std::size_t count) {
        return zAllocator::zNew( std::max( count, sizeof( zSTRING ) ) );
    }

    static void operator delete(void* ptr) {
        zAllocator::zFree(ptr);
    }

    void Delete() {
        // no-op, as we have a proper destructor now.
        // reinterpret_cast<void( __fastcall* )( zSTRING* )>( GothicMemoryLocations::zSTRING::DestructorCharPtr )( this );
    }

    const char* ToChar() const {
        return _dataPtr ? _dataPtr : "";
    }

    std::string ToString() const {
        return std::string(ToChar(), length);
    }

    size_t Length() const
    {
        return length;
    }
    
    std::string_view ToView() const {
        return std::string_view(ToChar(), length);
    }

private:
    void* _vtblString;
    void* _allocator;
    //---
    const char* _dataPtr;
    size_t length;
    size_t reserved;
};
static_assert(sizeof(zSTRING) == 20, "Must be exactly 20");
#pragma pack(pop)
