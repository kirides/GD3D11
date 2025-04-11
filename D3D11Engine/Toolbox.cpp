#pragma once
#include "pch.h"
#include "Toolbox.h"
#include "zTypes.h"

namespace Toolbox {
    /** Checks if one of a series of strings is found within the input-string */
    bool StringContainsOneOf( const std::string& string, const std::string* checkStrings, int numStrings ) {
        std::string us = string;

        // Make them uppercase
        std::transform( us.begin(), us.end(), us.begin(), ::toupper );

        for ( int i = 0; i < numStrings; i++ ) {
            std::string cu = checkStrings[i];
            std::transform( cu.begin(), cu.end(), cu.begin(), ::toupper );

            if ( us.find( cu[i] ) != std::string::npos )
                return true;
        }

        return false;
    }

    /** Checks whether a given boundingbox is inside the given frustum. The index in "cache" is tested first, if it isn't set to -1 */
    zTCam_ClipType BBox3DInFrustumCached( const zTBBox3D& bbox3D, zTPlane* frustumPlanes, uint8_t* signbits, int& cache ) {
        // This resembles gothics method for checking the frustum, but with enancements

        // Use cache first, if possible
        int tmpCache = cache;
        int	i = (cache != -1) ? cache : 5;
        int skip = -1;
        int clipFlags = CLIP_FLAGS_NO_FAR;

        // Check all planes
        do {
            // Don't test the cached plane twice
            if ( i == skip )
                continue;

            // Still not sure how these clipflags work
            if ( !(clipFlags & (1 << i)) )
                continue;

            float dist;
            const zTPlane& plane = frustumPlanes[i];
            switch ( signbits[i] ) {
            case 0:	// 000, ZYX
                dist = bbox3D.Min.x * plane.Normal.x + bbox3D.Min.y * plane.Normal.y + bbox3D.Min.z * plane.Normal.z;	if ( dist < plane.Distance ) { cache = i; return ZTCAM_CLIPTYPE_OUT; }
                dist = bbox3D.Max.x * plane.Normal.x + bbox3D.Max.y * plane.Normal.y + bbox3D.Max.z * plane.Normal.z;	if ( dist >= plane.Distance ) clipFlags &= ~(1 << i);
                break;
            case 1:	// 001
                dist = bbox3D.Max.x * plane.Normal.x + bbox3D.Min.y * plane.Normal.y + bbox3D.Min.z * plane.Normal.z;	if ( dist < plane.Distance ) { cache = i; return ZTCAM_CLIPTYPE_OUT; }
                dist = bbox3D.Min.x * plane.Normal.x + bbox3D.Max.y * plane.Normal.y + bbox3D.Max.z * plane.Normal.z;	if ( dist >= plane.Distance ) clipFlags &= ~(1 << i);
                break;
            case 2:	// 010
                dist = bbox3D.Min.x * plane.Normal.x + bbox3D.Max.y * plane.Normal.y + bbox3D.Min.z * plane.Normal.z;	if ( dist < plane.Distance ) { cache = i; return ZTCAM_CLIPTYPE_OUT; }
                dist = bbox3D.Max.x * plane.Normal.x + bbox3D.Min.y * plane.Normal.y + bbox3D.Max.z * plane.Normal.z;	if ( dist >= plane.Distance ) clipFlags &= ~(1 << i);
                break;
            case 3:	// 011
                dist = bbox3D.Max.x * plane.Normal.x + bbox3D.Max.y * plane.Normal.y + bbox3D.Min.z * plane.Normal.z;	if ( dist < plane.Distance ) { cache = i; return ZTCAM_CLIPTYPE_OUT; }
                dist = bbox3D.Min.x * plane.Normal.x + bbox3D.Min.y * plane.Normal.y + bbox3D.Max.z * plane.Normal.z;	if ( dist >= plane.Distance ) clipFlags &= ~(1 << i);
                break;
            case 4:	// 100
                dist = bbox3D.Min.x * plane.Normal.x + bbox3D.Min.y * plane.Normal.y + bbox3D.Max.z * plane.Normal.z;	if ( dist < plane.Distance ) { cache = i; return ZTCAM_CLIPTYPE_OUT; }
                dist = bbox3D.Max.x * plane.Normal.x + bbox3D.Max.y * plane.Normal.y + bbox3D.Min.z * plane.Normal.z;	if ( dist >= plane.Distance ) clipFlags &= ~(1 << i);
                break;
            case 5:	// 101
                dist = bbox3D.Max.x * plane.Normal.x + bbox3D.Min.y * plane.Normal.y + bbox3D.Max.z * plane.Normal.z;	if ( dist < plane.Distance ) { cache = i; return ZTCAM_CLIPTYPE_OUT; }
                dist = bbox3D.Min.x * plane.Normal.x + bbox3D.Max.y * plane.Normal.y + bbox3D.Min.z * plane.Normal.z;	if ( dist >= plane.Distance ) clipFlags &= ~(1 << i);
                break;
            case 6:	// 110
                dist = bbox3D.Min.x * plane.Normal.x + bbox3D.Max.y * plane.Normal.y + bbox3D.Max.z * plane.Normal.z;	if ( dist < plane.Distance ) { cache = i; return ZTCAM_CLIPTYPE_OUT; }
                dist = bbox3D.Max.x * plane.Normal.x + bbox3D.Min.y * plane.Normal.y + bbox3D.Min.z * plane.Normal.z;	if ( dist >= plane.Distance ) clipFlags &= ~(1 << i);
                break;
            case 7:	// 111
                dist = bbox3D.Max.x * plane.Normal.x + bbox3D.Max.y * plane.Normal.y + bbox3D.Max.z * plane.Normal.z;	if ( dist < plane.Distance ) { cache = i; return ZTCAM_CLIPTYPE_OUT; }
                dist = bbox3D.Min.x * plane.Normal.x + bbox3D.Min.y * plane.Normal.y + bbox3D.Min.z * plane.Normal.z;	if ( dist >= plane.Distance ) clipFlags &= ~(1 << i);
                break;
            };

            // If this was a cached check, return to normal
            if ( tmpCache != -1 ) {
                skip = tmpCache;
                tmpCache = -1;
                i = 6; // Would be 5, but we are decrementing it right after this
            }
        } while ( i-- );

        // If we got this far, the box is visible and we can reset the cache
        cache = -1;

        return (clipFlags > 0) ? ZTCAM_CLIPTYPE_CROSSING : ZTCAM_CLIPTYPE_IN;
    }

    bool CreateDirectoryRecursive( const std::string& dirName ) {
#pragma warning(push)
#pragma warning(disable: 4101)
        try {
            return std::filesystem::create_directories( dirName );
        } catch ( const std::exception& e ) {
            return false;
        }
    }

    bool FolderExists( const std::string& dirName_in ) {
#pragma warning(push)
#pragma warning(disable: 4101)
        try {
            return std::filesystem::is_directory( dirName_in );
        } catch ( const std::exception& e ) {
            return false;
        }
    }

    static std::size_t hash_value( float value ) {
        stdext::hash<float> hasher;
        return hasher( value );
    }

    static std::size_t hash_value( DWORD value ) {
        stdext::hash<DWORD> hasher;
        return hasher( value );
    }

    static void hash_combine( std::size_t& seed, float value ) {
        seed ^= hash_value( value ) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
    }

    /** Hashes the given DWORD value */
    void hash_combine( std::size_t& seed, DWORD value ) {
        seed ^= hash_value( value ) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
    }

    /** Returns true if the given position is inside the box */
    bool PositionInsideBox( const XMFLOAT3& p, const XMFLOAT3& min, const XMFLOAT3& max ) {
        if ( p.x > min.x &&
            p.y > min.y &&
            p.z > min.z &&
            p.x < max.x &&
            p.y < max.y &&
            p.z < max.z )
            return true;

        return false;
    }

    /** Computes the Distance of a point to an AABB */
    float ComputePointAABBDistance( const XMFLOAT3& p, const XMFLOAT3& min, const XMFLOAT3& max ) {
        float dx = std::max( std::max( min.x - p.x, 0.0f ), p.x - max.x );
        float dy = std::max( std::max( min.y - p.y, 0.0f ), p.y - max.y );
        float dz = std::max( std::max( min.z - p.z, 0.0f ), p.z - max.z );
        //return sqrtf( dx * dx + dz * dz );
        return _mm_cvtss_f32( _mm_rcp_ss( _mm_rsqrt_ss( _mm_set_ss( dx * dx + dz * dz ) ) ) );
    }

    /** Computes the Normal of a triangle */
    FXMVECTOR ComputeNormal( const XMFLOAT3& v0, const XMFLOAT3& v1, const XMFLOAT3& v2 ) {
        FXMVECTOR Normal = XMVector3Normalize( XMVector3Cross( (XMLoadFloat3( &v1 ) - XMLoadFloat3( &v0 )), (XMLoadFloat3( &v2 ) - XMLoadFloat3( &v0 )) ) );

        return Normal;
    }

    /** Does a ray vs aabb test */
    bool IntersectTri( const XMFLOAT3& v0, const XMFLOAT3& v1, const XMFLOAT3& v2, const XMFLOAT3& origin, const XMFLOAT3& direction, float& u, float& v, float& t ) {
        const float EPSILON = 0.00001f;
        FXMVECTOR edge1 = XMLoadFloat3( &v1 ) - XMLoadFloat3( &v0 );
        FXMVECTOR edge2 = XMLoadFloat3( &v2 ) - XMLoadFloat3( &v0 );
        FXMVECTOR pvec = XMVector3Cross( XMLoadFloat3( &direction ), edge2 );
        float det;
        XMStoreFloat( &det, XMVector3Dot( edge1, pvec ) );
        if ( det > -EPSILON && det < EPSILON ) return false;

        float invDet = 1 / det;
        FXMVECTOR tvec = XMLoadFloat3( &origin ) - XMLoadFloat3( &v0 );
        XMStoreFloat( &u, XMVector3Dot( tvec, pvec ) * invDet );
        if ( u < 0 || u > 1 ) return false;
        FXMVECTOR qvec = XMVector3Cross( tvec, edge1 );

        XMStoreFloat( &v, XMVector3Dot( XMLoadFloat3( &direction ), qvec ) * invDet );
        if ( v < 0 || u + v > 1 ) return false;
        XMStoreFloat( &t, XMVector3Dot( edge2, qvec ) * invDet );

        return true;
    }

    /** Does a ray vs aabb test */
    bool IntersectBox( const XMFLOAT3& min, const XMFLOAT3& max, const XMFLOAT3& origin, const XMFLOAT3& direction, float& t ) {
        XMFLOAT3 dirfrac;

        // r.dir is unit direction vector of ray
        dirfrac.x = 1.0f / direction.x;
        dirfrac.y = 1.0f / direction.y;
        dirfrac.z = 1.0f / direction.z;
        // lb is the corner of AABB with minimal coordinates - left bottom, rt is maximal corner
        // r.org is origin of ray
        float t1 = (min.x - origin.x) * dirfrac.x;
        float t2 = (max.x - origin.x) * dirfrac.x;
        float t3 = (min.y - origin.y) * dirfrac.y;
        float t4 = (max.y - origin.y) * dirfrac.y;
        float t5 = (min.z - origin.z) * dirfrac.z;
        float t6 = (max.z - origin.z) * dirfrac.z;

        float tmin = std::max( std::max( std::min( t1, t2 ), std::min( t3, t4 ) ), std::min( t5, t6 ) );
        float tmax = std::min( std::min( std::max( t1, t2 ), std::max( t3, t4 ) ), std::max( t5, t6 ) );

        // if tmax < 0, ray (line) is intersecting AABB, but whole AABB is behind us
        if ( tmax < 0 ) {
            t = tmax;
            return false;
        }

        // if tmin > tmax, ray doesn't intersect AABB
        if ( tmin > tmax ) {
            t = tmax;
            return false;
        }

        t = tmin;
        return true;
    }

    /** Returns whether two AABBs are intersecting or not */
    bool AABBsOverlapping( const XMFLOAT3& minA, const XMFLOAT3& maxA, const XMFLOAT3& minB, const XMFLOAT3& maxB ) {
        //Check if Box1's max is greater than Box2's min and Box1's min is less than Box2's max
        return(maxA.x > minB.x &&
            minA.x < maxB.x &&
            maxA.y > minB.y &&
            minA.y < maxB.y &&
            maxA.z > minB.z &&
            minA.z < maxB.z);
    }

    /** Converts a multi-byte-string to wide-char */
    std::wstring ToWideChar( const std::string& str ) {
        return std::wstring( str.begin(), str.end() );
    }

    /** Converts a wide-char-string to  multi-byte*/
    std::string ToMultiByte( const std::wstring& str ) {
        return std::string( str.begin(), str.end() );
    }

    /** Returns a random number between 0 and 1 */
    float frand() {
        return static_cast<float>(rand()) / RAND_MAX;
    }

    /** Linear interpolation */
    float lerp( float a, float b, float w ) {
#if defined(_XM_FMA3_INTRINSICS_)
        __m128 v0 = _mm_set_ss( a );
        __m128 v1 = _mm_set_ss( b );
        __m128 t  = _mm_set_ss( w );
        return _mm_cvtss_f32( _mm_fmadd_ss( t, v1, _mm_fnmadd_ss( t, v0, v0 ) ) );
#else
        return (1.0f - w) * a + w * b;
#endif
    }

    /** Converts an errorcode into a string */
    std::string MakeErrorString( XRESULT code ) {
        switch ( code ) {
        case XRESULT::XR_FAILED:
            return "General fail";

        case XRESULT::XR_SUCCESS:
            return "Success";

        case XRESULT::XR_INVALID_ARG:
            return "Invalid argument";

        default:
            return "Other error";
        }
    }

    /** Returns the number of bits inside a bitmask */
    WORD GetNumberOfBits( DWORD dwMask ) {
        WORD wBits = 0;
        while ( dwMask ) {
            dwMask = dwMask & (dwMask - 1);
            wBits++;
        }
        return wBits;
    };

    /** Returns the size of a DDS-Image in bytes */
    unsigned int GetDDSStorageRequirements( unsigned int width, unsigned int height, bool dxt1 ) {
        // compute the storage requirements
        int blockcount = ((width + 3) / 4) * ((height + 3) / 4);
        int blocksize = dxt1 ? 8 : 16;
        return blockcount * blocksize;
    }

    /** Returns the RowPitch-Size of a DDS-Image */
    unsigned int GetDDSRowPitchSize( unsigned int width, bool dxt1 ) {
        if ( dxt1 )
            return std::max<unsigned int>(1, ((width + 3) / 4) ) * 8;
        else
            return std::max<unsigned int>(1, ((width + 3) / 4) ) * 16;
    }

    /** Returns whether the given file exists */
    bool FileExists( const std::string& file ) {
#pragma warning(push)
#pragma warning(disable: 4101)
        try {
            return std::filesystem::exists( file );
        } catch ( const std::exception& e ) {
            return false;
        }
    }

    /** Saves a std::string to a FILE* */
    void SaveStringToFILE( FILE* f, const std::string& str ) {
        unsigned int numChars = str.size();
        fwrite( &numChars, sizeof( numChars ), 1, f );

        fwrite( str.data(), numChars, 1, f );
    }

    /** Loads a std::string from a FILE* */
    std::string LoadStringFromFILE( FILE* f ) {
        unsigned int numChars;
        fread( &numChars, sizeof( numChars ), 1, f );

        char* c = new char[numChars + 1];
        memset( c, 0, numChars + 1 );
        fread( c, numChars, 1, f );

        std::string str = c;

        delete[] c;

        return str;
    }

    DWORD timeSinceStartMs() {
        static std::chrono::steady_clock::time_point s_startPoint = std::chrono::steady_clock::now();

        // We dont expect anyone to play for 49 Days straight!
        return static_cast<DWORD>( std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - s_startPoint).count() );
    }
}
