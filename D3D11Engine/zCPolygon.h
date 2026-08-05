#pragma once
#include "pch.h"
#include "HookedFunctions.h"
#include "zTypes.h"
#include "Logger.h"

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

    int TransformedIndex;
    int MyIndex;
};

class zCVertFeature {
public:
    float3 normal;
    DWORD lightStatic;
    DWORD lightDynamic;
    float2 texCoord;
};


/** ZenGin's "fast 32-bit float evaluations" (zTools.h). These are raw sign-bit tests, so -0.0f
    counts as negative and +0.0f counts as "greater zero" - the ray/poly tests below depend on
    exactly that, so don't replace them with the arithmetic comparisons they look like. */
inline bool zIsNegative( float f ) {
    return std::bit_cast<unsigned int>( f ) >= 0x80000000u;
}

inline bool zIsInRange01( float f ) {
    const unsigned int bits = std::bit_cast<unsigned int>( f );
    return bits < 0x80000000u && bits <= 0x3F800000u;
}

class zCTexture;
class zCMaterial;
class zCLightmap;
class zCPolygon {
public:
    /** Hooks the functions of this Class */
    static void Hook() {
#ifndef BUILD_SPACER
        // ZenGin built both of these from the same source for G1 1.08k and G2 2.6, down to
        // identical instructions, and the ports below were written against exactly that code.
        // Check the prologue before attaching so we quietly keep the original implementation on
        // a binary we haven't verified (G1 1.12f), or when a SystemPack/Union plugin has already
        // detoured them, instead of replacing something we didn't port.
        static const unsigned char sigOneSided[] = { 0x83, 0xEC, 0x20, 0xD9, 0x41, 0x10, 0x53, 0x8B, 0x5C, 0x24, 0x28 };
        static const unsigned char sigTwoSided[] = { 0x83, 0xEC, 0x0C, 0x53, 0x56, 0x8B, 0xF1, 0x8B, 0x4C, 0x24, 0x18 };

        if ( PrologueMatches( GothicMemoryLocations::zCPolygon::CheckRayPolyIntersection, sigOneSided, sizeof( sigOneSided ) ) ) {
            DetourAttachTyped( &HookedFunctions::OriginalFunctions.original_zCPolygonCheckRayPolyIntersection, hooked_CheckRayPolyIntersection );
        } else {
            LogWarn() << "zCPolygon::CheckRayPolyIntersection has an unexpected prologue - keeping ZenGin's implementation";
        }

        if ( PrologueMatches( GothicMemoryLocations::zCPolygon::CheckRayPolyIntersection2Sided, sigTwoSided, sizeof( sigTwoSided ) ) ) {
            DetourAttachTyped( &HookedFunctions::OriginalFunctions.original_zCPolygonCheckRayPolyIntersection2Sided, hooked_CheckRayPolyIntersection2Sided );
        } else {
            LogWarn() << "zCPolygon::CheckRayPolyIntersection2Sided has an unexpected prologue - keeping ZenGin's implementation";
        }
#endif
    }

#ifndef BUILD_SPACER
    /** Replacements for ZenGin's two ray/poly tests. These sit at the leaf of every
        zCBspBase::CheckRayAgainstPolys* loop - NPC movement, AI line-of-sight, camera and
        projectile collision - and the shipped VC6 code runs the whole thing on the x87 stack,
        which is where the per-frame cost comes from.

        Same algorithm, same operand order and the same sign-bit comparisons as the original, so
        hit/miss decisions match; only the arithmetic changes, because the compiler emits scalar
        SSE2 here instead of x87. Intermediates are therefore 32-bit rather than 80-bit, so a ray
        that grazes a polygon edge to within ~1e-7 can decide differently than it used to. */
    static int __fastcall hooked_CheckRayPolyIntersection( zCPolygon* thisptr, const XMFLOAT3& rayOrigin,
        const XMFLOAT3& ray, XMFLOAT3& inters, float& alpha ) {
        const zTPlane& plane = thisptr->GetPolyPlane();
        const XMFLOAT3& n = plane.Normal;

        // 1) Ray/plane intersection with backface culling. dn is negative from here on.
        const float dn = ray.x * n.x + ray.y * n.y + ray.z * n.z;
        if ( !zIsNegative( dn ) ) return FALSE; // parallel or backfacing?

        alpha = plane.Distance - (n.x * rayOrigin.x + n.y * rayOrigin.y + n.z * rayOrigin.z);
        if ( !zIsNegative( alpha ) || alpha < dn ) return FALSE; // in front of / behind the ray?

        alpha *= (1.0f / dn); // both are negative, so this lands in 0..1
        inters.x = rayOrigin.x + alpha * ray.x;
        inters.y = rayOrigin.y + alpha * ray.y;
        inters.z = rayOrigin.z + alpha * ray.z;

        // 2) Project onto the plane the normal points away from the least, then 3) point-in-poly
        int vx, vy;
        SelectProjectionAxes( n, vx, vy );
        return PointInPoly( thisptr, inters, vx, vy );
    }

    static int __fastcall hooked_CheckRayPolyIntersection2Sided( zCPolygon* thisptr, const XMFLOAT3& rayOrigin,
        const XMFLOAT3& ray, XMFLOAT3& inters, float& alpha ) {
        const zTPlane& plane = thisptr->GetPolyPlane();
        const XMFLOAT3& n = plane.Normal;

        // 1) Ray/plane intersection, no backface culling (used for bbox checks and diagonals)
        const float dn = ray.x * n.x + ray.y * n.y + ray.z * n.z;
        if ( dn == 0 ) return FALSE; // parallel?

        alpha = (plane.Distance - (n.x * rayOrigin.x + n.y * rayOrigin.y + n.z * rayOrigin.z)) / dn;
        if ( !zIsInRange01( alpha ) ) return FALSE;

        inters.x = rayOrigin.x + alpha * ray.x;
        inters.y = rayOrigin.y + alpha * ray.y;
        inters.z = rayOrigin.z + alpha * ray.z;

        int vx, vy;
        SelectProjectionAxes( n, vx, vy );
        if ( thisptr->GetNumPolyVertices() == 3 ) {
            return TriangleContains( thisptr, inters, vx, vy, /*div0Safe=*/false );
        }

        // ZenGin uses a different (and cheaper) crossing test in the 2-sided n-gon case
        zCVertex** vertex = thisptr->getVertices();
        const int numVert = thisptr->GetNumPolyVertices();
        bool inside = false;
        for ( int i = 0, j = numVert - 1; i < numVert; j = i++ ) {
            const float* u = &vertex[i]->Position.x;
            const float* v = &vertex[j]->Position.x;

            if ( (u[vy] >= (&inters.x)[vy]) != (v[vy] >= (&inters.x)[vy]) ) {
                bool right = u[vx] >= (&inters.x)[vx];
                if ( right != (v[vx] >= (&inters.x)[vx]) ) {
                    float t = (u[vy] - (&inters.x)[vy]) / (u[vy] - v[vy]);
                    if ( u[vx] + (v[vx] - u[vx]) * t >= (&inters.x)[vx] ) inside = !inside;
                } else if ( right ) {
                    inside = !inside;
                }
            }
        }
        return inside ? TRUE : FALSE;
    }

private:
    static bool PrologueMatches( unsigned int address, const unsigned char* signature, size_t size ) {
        return std::memcmp( reinterpret_cast<const void*>( address ), signature, size ) == 0;
    }

    /** Picks the axis pair to project onto: drop the component the normal is largest in.
        zAbsApprox() is a plain bitwise abs and zIsSmallerPositive() an integer compare of two
        non-negative floats, so both collapse to ordinary float math here. */
    static void SelectProjectionAxes( const XMFLOAT3& normal, int& vx, int& vy ) {
        const float ax = std::fabs( normal.x );
        const float ay = std::fabs( normal.y );
        const float az = std::fabs( normal.z );

        int vz = (ax < ay) ? 1 : 0;
        if ( ((vz == 1) ? ay : ax) < az ) vz = 2;

        vx = vz + 1; if ( vx > 2 ) vx = 0;
        vy = vx + 1; if ( vy > 2 ) vy = 0;
    }

    /** Barycentric point-in-triangle on the projected axes (src: brown.edu/people/scd/facts.html) */
    static int TriangleContains( const zCPolygon* poly, const XMFLOAT3& inters, int vx, int vy, bool div0Safe ) {
        zCVertex** vertex = poly->getVertices();
        const float* v0 = &vertex[0]->Position.x;
        const float* v1 = &vertex[1]->Position.x;
        const float* v2 = &vertex[2]->Position.x;

        const float a_c0 = v0[vx] - v2[vx];
        const float a_c1 = v0[vy] - v2[vy];
        const float b_c0 = v1[vx] - v2[vx];
        const float b_c1 = v1[vy] - v2[vy];
        const float p_c0 = (&inters.x)[vx] - v2[vx];
        const float p_c1 = (&inters.x)[vy] - v2[vy];

        const float denom = a_c0 * b_c1 - a_c1 * b_c0;
        if ( div0Safe && denom == 0 ) return FALSE;

        const float denomInv = 1.0f / denom;
        const float u = (p_c0 * b_c1 - p_c1 * b_c0) * denomInv;
        const float v = (a_c0 * p_c1 - a_c1 * p_c0) * denomInv;

        return (u + v < 1) && (u < 1) && (v < 1) && !zIsNegative( u ) && !zIsNegative( v ) ? TRUE : FALSE;
    }

    /** Triangles are ~90% of the calls, so they get the special case; everything else falls back
        to the even-odd crossing test. */
    static int PointInPoly( const zCPolygon* poly, const XMFLOAT3& inters, int vx, int vy ) {
        const int numVert = poly->GetNumPolyVertices();
        if ( numVert == 3 ) {
            return TriangleContains( poly, inters, vx, vy, /*div0Safe=*/true );
        }

        zCVertex** vertex = poly->getVertices();
        bool inside = false;
        for ( int i = 0, j = numVert - 1; i < numVert; j = i++ ) {
            const float* u = &vertex[i]->Position.x;
            const float* v = &vertex[j]->Position.x;
            if ( (u[vy] <= (&inters.x)[vy] && (&inters.x)[vy] < v[vy] && (v[vy] - u[vy]) * ((&inters.x)[vx] - u[vx]) < (v[vx] - u[vx]) * ((&inters.x)[vy] - u[vy]))
                || (v[vy] <= (&inters.x)[vy] && (&inters.x)[vy] < u[vy] && (v[vy] - u[vy]) * ((&inters.x)[vx] - u[vx]) > (v[vx] - u[vx]) * ((&inters.x)[vy] - u[vy])) ) {
                inside = !inside;
            }
        }
        return inside ? TRUE : FALSE;
    }

public:
#endif

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

    /** Plane this polygon lies in. Front side is dot(Normal, p) > Distance. */
    const zTPlane& GetPolyPlane() const {
        return *reinterpret_cast<zTPlane*>(THISPTR_OFFSET( GothicMemoryLocations::zCPolygon::Offset_PolyPlane ));
    }

    /** True if this poly is one of the sector/portal polys the world was compiled with */
    bool IsPortal() const {
        return GetPolyFlags()->PortalPoly != 0;
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
