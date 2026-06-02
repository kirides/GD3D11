#pragma once
#include "pch.h"
#include "GothicMemoryLocations.h"

class zSTRING {
public:
    zSTRING() {
        reinterpret_cast<void( __fastcall* )( zSTRING* )>( GothicMemoryLocations::zSTRING::ConstructorEmptyPtr )( this );
    }
    zSTRING( const char* str ) {
        reinterpret_cast<void( __fastcall* )(zSTRING*, int, const char*)>(GothicMemoryLocations::zSTRING::ConstructorCharPtr)(this, 0, str);
    }

    ~zSTRING() {
        reinterpret_cast<void( __fastcall* )(zSTRING*)>(GothicMemoryLocations::zSTRING::DestructorCharPtr)(this);
    }

    static void* operator new(std::size_t count) {
        return malloc( std::max( count, sizeof( zSTRING ) ) );
    }

    static void operator delete(void* ptr) {
        free(ptr);
    }

    void Delete() {
        reinterpret_cast<void( __fastcall* )( zSTRING* )>( GothicMemoryLocations::zSTRING::DestructorCharPtr )( this );
    }

    const char* ToChar() const {
        return _dataPtr ? _dataPtr : "";
    }

    size_t Length() const
    {
        return length;
    }

private:
    void* _vtblString;
    void* _unknwn;
    //---
    char* _dataPtr;
    size_t length;
    size_t reserved;
};
