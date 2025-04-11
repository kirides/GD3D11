#pragma once
#include "pch.h"
#include "HookedFunctions.h"

#pragma pack (push, 1)	
#ifdef BUILD_GOTHIC_2_6_fix
struct PolyFlags {
    unsigned char PortalPoly : 2;
    unsigned char Occluder : 1;
    unsigned char SectorPoly : 1;
    unsigned char MustRelight : 1;
    unsigned char PortalIndoorOutdoor : 1;
    unsigned char GhostOccluder : 1;
    unsigned char NoDynLightNear : 1;
    VERTEX_INDEX SectorIndex : 16;
};

#elif defined(BUILD_GOTHIC_1_08k)
struct PolyFlags {
    unsigned char PortalPoly : 2;
    unsigned char Occluder : 1;
    unsigned char SectorPoly : 1;
    unsigned char LodFlag : 1;
    unsigned char PortalIndoorOutdoor : 1;
    unsigned char GhostOccluder : 1;
    unsigned char NormalMainAxis : 2;
    VERTEX_INDEX SectorIndex : 16;
};
#endif
#pragma pack (pop)

class zCVertex {
public:
    /*#ifdef BUILD_GOTHIC_1_08k
        int id;
    #endif*/

    float3 Position;

    int TransformedIndex = 0;
    int MyIndex = 0;
};

class zCVertFeature {
public:
    float3 normal;
    DWORD lightStatic;
    DWORD lightDynamic;
    float2 texCoord;
};


class zCTexture;
class zCMaterial;
class zCLightmap;
class zCPolygon {
public:
    ~zCPolygon() {
        // Clean our vertices
        for ( int i = 0; i < GetNumPolyVertices(); i++ ) {
            delete getVertices()[i];
            getVertices()[i] = nullptr;
        }

        Destructor();
    }

    void Destructor() {
#ifndef BUILD_GOTHIC_2_6_fix
        reinterpret_cast<void( __fastcall* )( zCPolygon* )>( GothicMemoryLocations::zCPolygon::Destructor )( this );
#endif
    }

    void Constructor() {
#ifndef BUILD_GOTHIC_2_6_fix
        reinterpret_cast<void( __fastcall* )( zCPolygon* )>( GothicMemoryLocations::zCPolygon::Constructor )( this );
#endif
    }

    void AllocVertPointers( int num ) {
#ifndef BUILD_GOTHIC_2_6_fix
        reinterpret_cast<void( __fastcall* )( zCPolygon*, int, int )>( GothicMemoryLocations::zCPolygon::AllocVerts )( this, 0, num );
#endif
    }

    void CalcNormal() {
#ifndef BUILD_GOTHIC_2_6_fix
        reinterpret_cast<void( __fastcall* )( zCPolygon* )>( GothicMemoryLocations::zCPolygon::CalcNormal )( this );
#endif
    }

    void AllocVertData() {
        for ( int i = 0; i < GetNumPolyVertices(); i++ ) {
            getVertices()[i] = new zCVertex;
        }
    }

    zCVertex** getVertices() const {
        return *reinterpret_cast<zCVertex***>(THISPTR_OFFSET( GothicMemoryLocations::zCPolygon::Offset_VerticesArray ));
    }

    zCVertFeature** getFeatures() const {
        return *reinterpret_cast<zCVertFeature***>(THISPTR_OFFSET( GothicMemoryLocations::zCPolygon::Offset_FeaturesArray ));
    }

    unsigned char GetNumPolyVertices() const {
        return *reinterpret_cast<unsigned char*>(THISPTR_OFFSET( GothicMemoryLocations::zCPolygon::Offset_NumPolyVertices ));
    }

    PolyFlags* GetPolyFlags() const {
        return reinterpret_cast<PolyFlags*>(THISPTR_OFFSET( GothicMemoryLocations::zCPolygon::Offset_PolyFlags ));
    }

    zCMaterial* GetMaterial() const {
        return *reinterpret_cast<zCMaterial**>(THISPTR_OFFSET( GothicMemoryLocations::zCPolygon::Offset_Material ));
    }

    void SetMaterial( zCMaterial* material ) {
        *reinterpret_cast<zCMaterial**>(THISPTR_OFFSET( GothicMemoryLocations::zCPolygon::Offset_Material )) = material;
    }

    float3 GetLightStatAtPos(float3& position) {
        float3 colorStat;
        reinterpret_cast<void( __fastcall* )( zCPolygon*, DWORD, float3&, float3& )>( GothicMemoryLocations::zCPolygon::GetLightStatAtPos )( this, 0, colorStat, position );
        return colorStat;
    }

    zCLightmap* GetLightmap() const {
        return *reinterpret_cast<zCLightmap**>(THISPTR_OFFSET( GothicMemoryLocations::zCPolygon::Offset_Lightmap ));
    }

    void SetLightmap( zCLightmap* lightmap ) {
        *reinterpret_cast<zCLightmap**>(THISPTR_OFFSET( GothicMemoryLocations::zCPolygon::Offset_Lightmap )) = lightmap;
    }

    char data[56];
};
