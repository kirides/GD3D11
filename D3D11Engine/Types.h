#pragma once
#include <string>
#include <sstream>
#include <Windows.h>
#include <DirectXMath.h>

using namespace DirectX;

/** Defines types used for the project */

/** Errorcodes */
enum XRESULT : int {
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

    INT2() { x = 0; y = 0; }

    INT2( const std::string& resolution ) {
        std::istringstream iss( resolution );
        char separator;

        if ( !(iss >> x >> separator >> y) || separator != 'x' ) {
            x = 800;
            y = 600;
        }
    }

    std::string toString() const {
        return "(" + std::to_string( x ) + ", " + std::to_string( y ) + ")";
    }

    bool operator==( const INT2& rhs ) const { return x == rhs.x && y == rhs.y; }
    bool operator!=( const INT2& rhs ) const { return !(x == rhs.x && y == rhs.y); }

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

struct float3 : XMFLOAT3 {
    float3(const float _x, const float _y, const float _z) : XMFLOAT3(_x, _y, _z) {}
    float3() : XMFLOAT3(0.0f, 0.0f, 0.0f) {}

    float3( const DWORD& color )
    : XMFLOAT3(static_cast<float>((color >> 16) & 0xFF) / 255.0f, 
    static_cast<float>((color >> 8) & 0xFF) / 255.0f,
    static_cast<float>(color & 0xFF) / 255.0f)
    {
    }

    float3( const XMFLOAT3& v ) : XMFLOAT3(v) {}

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
};

struct float4 : XMFLOAT4 {
    float4( const DWORD& color ) : XMFLOAT4(
    static_cast<float>((color >> 16) & 0xFF) / 255.0f,
        static_cast<float>((color >> 8) & 0xFF) / 255.0f,
        static_cast<float>(color & 0xFF) / 255.0f,
        static_cast<float>(color >> 24) / 255.0f) {
    }
    float4() : XMFLOAT4(0,0,0,0) { }

    float4(const float x, const float y, const float z, const float w ) : XMFLOAT4(x,y,z,w) { }

    float4( const float3& f ) : XMFLOAT4(f.x,f.y,f.z,1.0f) { }

    float4( const float3& f, float a ) : XMFLOAT4(f.x,f.y,f.z, a) { }
    
    float4( const XMFLOAT3& v ) : XMFLOAT4(v.x,v.y,v.z,1.0f) { }

    float4( const XMFLOAT4& v ) : XMFLOAT4(v) {}

    const float* toPtr() const { return &x; }
    float* toPtr() { return &x; }

    DWORD ToDWORD() const {
        BYTE a = static_cast<BYTE>(w * 255.0f);
        BYTE r = static_cast<BYTE>(x * 255.0f);
        BYTE g = static_cast<BYTE>(y * 255.0f);
        BYTE b = static_cast<BYTE>(z * 255.0f);
        return static_cast<DWORD>((a << 24) | (r << 16) | (g << 8) | b);
    }

    bool operator==( const float4& b ) const noexcept {
        return x == b.x 
            && y == b.y
            && z == b.z
            && w == b.w;
    }
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

struct DisplayModeInfo {
    int Width;
    int Height;
    unsigned int refreshRateNumerator;
    unsigned int refreshRateDenominator;

    DisplayModeInfo( int w, int h, unsigned int num = 0, unsigned int den = 0 )
        : Width( w ), Height( h ), refreshRateNumerator( num ), refreshRateDenominator( den ) {
    }
};
