#pragma once
#include "HookedFunctions.h"

#if (defined(BUILD_GOTHIC_2_6_fix) || defined(BUILD_GOTHIC_1_08k))
#if !defined(BUILD_SPACER) && !defined(BUILD_1_12F)
#define zCParserSupported
#endif
#endif


enum zPAR_TYPE : int {
    zPAR_TYPE_VOID, zPAR_TYPE_FLOAT, zPAR_TYPE_INT,
    zPAR_TYPE_STRING, zPAR_TYPE_CLASS, zPAR_TYPE_FUNC,
    zPAR_TYPE_PROTOTYPE, zPAR_TYPE_INSTANCE
};

class zCParser {
public:
#ifndef zCParserSupported
    static zCParser* GetParser() { return nullptr; }
#else
    static zCParser* GetParser() { return reinterpret_cast<zCParser*>(GothicMemoryLocations::GlobalObjects::zCParser); }
#endif


    void GetParameter( int& value ) {
#ifndef zCParserSupported
#else
        reinterpret_cast<void( __thiscall* )(zCParser*, int*)>(GothicMemoryLocations::zCParser::GetParameter_Int)(this, &value);
#endif
    }
    void GetParameter( float& value ) {
#ifndef zCParserSupported
#else
        reinterpret_cast<void( __thiscall* )(zCParser*, float*)>(GothicMemoryLocations::zCParser::GetParameter_Float)(this, &value);
#endif
    }
    void GetParameter( zSTRING& value ) {
#ifndef zCParserSupported
#else
        reinterpret_cast<void( __thiscall* )(zCParser*, zSTRING*)>(GothicMemoryLocations::zCParser::GetParameter_String)(this, &value);
#endif
    }

    void SetReturn( int value ) {
#ifndef zCParserSupported
#else
        reinterpret_cast<void( __thiscall* )(zCParser*, int)>(GothicMemoryLocations::zCParser::SetReturn_Int)(this, value);
#endif
    }
    void SetReturn( float value ) {
#ifndef zCParserSupported
#else
        reinterpret_cast<void( __thiscall* )(zCParser*, float)>(GothicMemoryLocations::zCParser::SetReturn_Float)(this, value);
#endif
    }
    void SetReturn( void* value ) {
#ifndef zCParserSupported
#else
        reinterpret_cast<void( __thiscall* )(zCParser*, void*)>(GothicMemoryLocations::zCParser::SetReturn_Instance)(this, value);
#endif
    }

    void SetReturn( zSTRING& value ) {
#ifndef zCParserSupported
#else
        reinterpret_cast<void( __thiscall* )(zCParser*, zSTRING)>(GothicMemoryLocations::zCParser::SetReturn_String)(this, value);
#endif
    }

    int GetIndex( const zSTRING& name )
    {
#ifndef zCParserSupported
        return -1;
#else
        return reinterpret_cast<int( __thiscall* )(void*, const zSTRING*)>(GothicMemoryLocations::zCParser::GetIndex)(this, &name);
#endif
    }

    template <typename... Args>
    void DefineExternal( zSTRING name, int( __cdecl* fn )(void), zPAR_TYPE returnType, Args... args ) {
#ifndef zCParserSupported
#else
        auto zCParser_DefineExternal = reinterpret_cast<void( __cdecl* )(zCParser*, zSTRING&, int( __cdecl * fn )(void), int returnType, int paramType ...)>(GothicMemoryLocations::zCParser::DefineExternal);
        zCParser_DefineExternal( this, name, fn, (int)returnType, args... );
#endif
    }

    template <typename... Args>
    void CallFunc( int symbolId, Args... args ) {
#ifndef zCParserSupported
#else
        const auto zCParser_CallFunc = reinterpret_cast<void( __cdecl* )(zCParser*, int, ...)>(GothicMemoryLocations::zCParser::CallFunc);
        zCParser_CallFunc( this, symbolId, args... );
#endif
    }

    void CallFunc( const zSTRING& name ) {
#ifndef zCParserSupported
#else
        const int symbolId = GetIndex( name );

        if ( symbolId != -1 ) {
            CallFunc( symbolId );
        }
#endif
    }
};
