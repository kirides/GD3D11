#pragma once
#include <string>
#include <sstream>
#include <Windows.h>

#include <DirectXMath.h>

using namespace DirectX;

/** Defines types used for the project */

/** Errorcodes */
enum XRESULT {
    XR_SUCCESS,
    XR_FAILED,
    XR_INVALID_ARG,
};

struct INT2 {
    INT2( int x, int y ) {
        this->x = x;
        this->y = y;
    }

    INT2( const XMFLOAT2& v ) {
        this->x = static_cast<int>(v.x + 0.5f);
        this->y = static_cast<int>(v.y + 0.5f);
    }

    INT2( const std::string& resolution ) {
        std::istringstream iss( resolution );
        char separator;

        if ( !(iss >> x >> separator >> y) || separator != 'x' ) {
            x = 800;
            y = 600;
        }
    }
    INT2() { x = 0; y = 0; }

    std::string toString() const {
        return std::to_string( x ) + "x" + std::to_string( y );
    }

    int x;
    int y;
};

struct INT4 {
    INT4( int x, int y, int z, int w ) {
        this->x = x;
        this->y = y;
        this->z = z;
        this->w = w;
    }

    INT4() { x = 0; y = 0; z = 0; w = 0; }

    int x;
    int y;
    int z;
    int w;
};

struct float4;

struct float3 {
    float3( float x, float y, float z ) {
        this->x = x;
        this->y = y;
        this->z = z;
    }

    float3( const DWORD& color ) {
        x = ((color >> 16) & 0xFF) / 255.0f;
        y = ((color >> 8) & 0xFF) / 255.0f;
        z = (color & 0xFF) / 255.0f;
    }

    float3( const float4& v ) {
        const float3* asF3 = reinterpret_cast<const float3*>(&v);
        x = asF3->x;
        y = asF3->y;
        z = asF3->z;
    }

    float3( const XMFLOAT3& v ) {
        x = v.x;
        y = v.y;
        z = v.z;
    }

    XMFLOAT3* toXMFLOAT3() const {
        return reinterpret_cast<XMFLOAT3*>(const_cast<float3*>(this));
    }
    std::string toString() const {
        return "(" + std::to_string( x ) + ", " + std::to_string( y ) + ", " + std::to_string( z ) + ")";
    }

    /** Checks if this float3 is in the range a of the given float3 */
    bool isLike( const float3& f, float a ) const {
        float3 t;
        t.x = abs( x - f.x );
        t.y = abs( y - f.y );
        t.z = abs( z - f.z );

        return t.x < a && t.y < a && t.z < a;
    }

    static float3 FromColor( unsigned char r, unsigned char g, unsigned char b ) {
        return float3( r / 255.0f, g / 255.0f, b / 255.0f );
    }

    bool operator<( const float3& rhs ) const {
        if ( (z < rhs.z) ) {
            return true;
        }
        if ( (z == rhs.z) && (y < rhs.y) ) {
            return true;
        }
        if ( (z == rhs.z) && (y == rhs.y) && (x < rhs.x) ) {
            return true;
        }
        return false;
    }

    bool operator==( const float3& b ) const {
        return isLike( b, 0.0001f );
    }

    float3() { x = 0; y = 0; z = 0; }

    float x, y, z;
};

struct float4 {
    float4( const DWORD& color ) {
        x = ((color >> 16) & 0xFF) / 255.0f;
        y = ((color >> 8) & 0xFF) / 255.0f;
        z = (color & 0xFF) / 255.0f;
        w = (color >> 24) / 255.0f;
    }

    float4( float x, float y, float z, float w ) {
        this->x = x;
        this->y = y;
        this->z = z;
        this->w = w;
    }

    float4( const float3& f ) {
        this->x = f.x;
        this->y = f.y;
        this->z = f.z;
        this->w = 1.0f;
    }

    float4( const float3& f, float a ) {
        this->x = f.x;
        this->y = f.y;
        this->z = f.z;
        this->w = a;
    }
    float4( const XMFLOAT3& v ) {
        x = v.x;
        y = v.y;
        z = v.z;
        w = 1.0f;
    }

    float4( const XMFLOAT4& v ) {
        x = v.x;
        y = v.y;
        z = v.z;
        w = v.w;
    }

    float4() { x = 0; y = 0; z = 0; w = 0; }

    XMFLOAT4* toXMFLOAT4() const {
        return reinterpret_cast<XMFLOAT4*>(const_cast<float4*>(this));
    }

    XMFLOAT3* toXMFLOAT3() const {
        return reinterpret_cast<XMFLOAT3*>(const_cast<float4*>(this));
    }
    float* toPtr() const {
        return reinterpret_cast<float*>(const_cast<float4*>(this));
    }

    DWORD ToDWORD() const {
        BYTE a = static_cast<BYTE>(w * 255.0f);
        BYTE r = static_cast<BYTE>(x * 255.0f);
        BYTE g = static_cast<BYTE>(y * 255.0f);
        BYTE b = static_cast<BYTE>(z * 255.0f);
        return static_cast<DWORD>((a << 24) | (r << 16) | (g << 8) | b);
    }

    float x, y, z, w;
};

struct float2 {
    float2( float x, float y ) {
        this->x = x;
        this->y = y;
    }

    float2( int x, int y ) {
        this->x = static_cast<float>(x);
        this->y = static_cast<float>(y);
    }

    float2( const INT2& i ) {
        this->x = static_cast<float>(i.x);
        this->y = static_cast<float>(i.y);
    }

    float2( const XMFLOAT2& v ) {
        this->x = v.x;
        this->y = v.y;
    }

    float2() { x = 0; y = 0; }

    XMFLOAT2* toXMFLOAT2() const {
        return reinterpret_cast<XMFLOAT2*>(const_cast<float2*>(this));
    }

    std::string toString() const {
        return "(" + std::to_string( x ) + ", " + std::to_string( y ) + ")";
    }

    bool operator<( const float2& rhs ) const {
        if ( (y < rhs.y) ) {
            return true;
        }
        if ( (y == rhs.y) && (x < rhs.x) ) {
            return true;
        }
        return false;
    }

    float x, y;
};
