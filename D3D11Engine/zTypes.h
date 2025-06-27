#pragma once
#include "pch.h"

enum zTCam_ClipType {
    ZTCAM_CLIPTYPE_IN,
    ZTCAM_CLIPTYPE_OUT,
    ZTCAM_CLIPTYPE_CROSSING
};

enum zTCam_ClipFlags {
    CLIP_FLAGS_FULL = 63,
    CLIP_FLAGS_NO_FAR = 15
};

enum zTAnimationMode {
    zVISUAL_ANIMODE_NONE = 0,
    zVISUAL_ANIMODE_WIND,
    zVISUAL_ANIMODE_WIND2,
};

enum zTMat_WaveMode
{
    zTMode_NONE,
    zTMode_AMBIENT_GROUND,
    zTMode_GROUND,
    zTMode_AMBIENT_WALL,
    zTMode_WALL,
    zTMode_ENV,
    zTMode_AMBIENT_WIND,
    zTMode_WIND
};

enum zTMat_WaveSpeed
{
    zTSpeed_NONE,
    zTSpeed_SLOW,
    zTSpeed_NORMAL,
    zTSpeed_FAST
};

#pragma pack (push, 1)	
struct zTBBox3D {
    XMFLOAT3	Min;
    XMFLOAT3	Max;

    enum zTPlaneClass {
        zPLANE_INFRONT,
        zPLANE_BEHIND,
        zPLANE_ONPLANE,
        zPLANE_SPANNING
    };

    int ClassifyToPlane( float planeDist, int axis ) const {
        if ( planeDist >= reinterpret_cast<const float*>(&Max)[axis] )
            return zPLANE_BEHIND;
        else if ( planeDist <= reinterpret_cast<const float*>(&Max)[axis] )
            return zPLANE_INFRONT;
        else return zPLANE_SPANNING;
    }
};

struct zTPlane {
    float Distance;
    XMFLOAT3 Normal;
};
#pragma pack (pop)

class zCWorld;
class zCVob;
class zCCamera;

struct zTRenderContext {
    int ClipFlags;
    zCVob* Vob;
    zCWorld* world;
    zCCamera* cam;
    float distVobToCam;

    // More not needed stuff here
};

struct zCRenderLight {
    int	LightType;
    XMFLOAT3	ColorDiffuse;
    XMFLOAT3	Position;
    XMFLOAT3	Direction;
    float Range;
    float RangeInv;
    XMFLOAT3 PositionLS;
    XMFLOAT3 DirectionLS;
    float Dir_approxFalloff;
};

struct zCRenderLightContainer {
    zCRenderLight			LightList[8];
    int						NumLights;

private:
    int	DoPrelight;
    int	DoSmoothPrelit;
    float PreLightDist;
    XMFLOAT4X4 MatObjToCam;
};

enum zTRnd_AlphaBlendFunc {
    zRND_ALPHA_FUNC_MAT_DEFAULT = 0,
    zRND_ALPHA_FUNC_NONE = 1,
    zRND_ALPHA_FUNC_BLEND = 2,
    zRND_ALPHA_FUNC_ADD = 3,
    zRND_ALPHA_FUNC_SUB = 4,
    zRND_ALPHA_FUNC_MUL = 5,
    zRND_ALPHA_FUNC_MUL2 = 6,
    zRND_ALPHA_FUNC_TEST = 7,
    zRND_ALPHA_FUNC_BLEND_TEST = 8
};

struct float4;
struct zColor {
    union {
        struct bgra {
            uint8_t b;
            uint8_t g;
            uint8_t r;
            uint8_t alpha;
        } bgra;
        DWORD dword;
    };
    zColor() { dword = 0xFFFFFFFF; }
    zColor( uint8_t b, uint8_t g, uint8_t r, uint8_t a = 255 ) {
        bgra.b = b;
        bgra.g = g;
        bgra.r = r;
        bgra.alpha = a;
    }
    zColor( DWORD dword ) {
        this->dword = dword;
    }
    bool IsWhite() {
        return dword == 0xFFFFFFFF;
    }
    float4 ToFloat4() {
        return float4( static_cast<float>(bgra.r) / 255.f, static_cast<float>(bgra.g) / 255.f, static_cast<float>(bgra.b) / 255.f, static_cast<float>(bgra.alpha) / 255.f );
    }
};
static zColor zCOLOR_WHITE = zColor( 255, 255, 255, 255 );

struct zTRndSimpleVertex {
    float2 pos;
    float z;
    float2 uv;
    zColor color;
};

struct zVEC2 {
public:
    union {
        float2 pos;
        float v[2];
    };
};

template <class T>
class zCList {
public:
    T* data;
    zCList* next;
};

template <class T>
class zList {
public:
    int (*cmp)(T* ele1, T* ele2);
    int count;
    T* last;
    T* root;
};

struct zTViewportData {
    int			xMin;
    int			yMin;
    int			xDim;
    int			yDim;
    float		xMinF;
    float		yMinF;
    float		xMaxF;
    float		yMaxF;
    float		xDimF;
    float		yDimF;
    float		xDimFloatMinus1;
    float		yDimFloatMinus1;
    float		xCenter;
    float		yCenter;
};

template <class T, int S> class zCMatrixStack {
    int		pos;
    T		stack[S];
public:
    zCMatrixStack() : pos( 0 ) {};
    void Push( const T& m ) { stack[pos++] = m; };
    T Pop( void ) { return stack[--pos]; };
};
