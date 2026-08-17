#pragma once
#include "pch.h"
#include "ConstantBufferStructs.h"
#include "WorldConverter.h"

struct AtmosphereSettings {
    float G;
    float Kr;
    float Km;
    float ESun;
    float InnerRadius;
    float OuterRadius;
    int Samples;
    float RayleightScaleDepth;
    float3 WaveLengths;
    XMFLOAT3 SpherePosition;
    float SphereOffsetY;
    XMFLOAT3 LightDirection;
    float SkyTimeScale;
};

enum ESkyTexture {
    ST_NewWorld,
    ST_OldWorld
};

/** Screen-space placement of the moon sprite (planets[1]), as the fixed-function sky would have drawn it.
    Backend-neutral: the caller resolves Texture to its own SRV/descriptor. Texture == nullptr means the moon
    is not visible this frame and nothing should be composited. See GSky::ResolveMoonSprite. */
struct MoonSpriteInfo {
    zCTexture* Texture = nullptr;
    float CenterPx[2] = { 0.0f, 0.0f };
    float HalfSizePx[2] = { 0.0f, 0.0f };
    float Color[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

    /** Radians, clockwise from screen-up - the moon's parallactic tilt. 0 at the zenith (tilt undefined). */
    float RotationAngle = 0.0f;
};

class zCTexture;
class zCSkyLayer;
class zCSkyState;
class GMesh;
class D3D11Texture;
class GfxTexture;

class GSky {
public:
    GSky();
    ~GSky();

    /** Creates needed resources by the sky */
    XRESULT InitSky();

    /** Renders the sky */
    XRESULT RenderSky();

    /** Returns the sky-texture for the passed daytime (0..1) */
    void GetTextureOfDaytime( float time, D3D11Texture** t1, D3D11Texture** t2, float* factor );

    /** Returns the loaded sky-Dome */
    GMesh* GetSkyDome();

    /** Sets the current sky texture */
    void SetSkyTexture( ESkyTexture texture );

    void SetCustomCloudAndNightTexture( int idx, bool isNightTexture, bool isOldWorld );
    void SetCustomSkyTexture_ZenGin( bool isNightTexture, zCTexture* texture, bool isOldWorld );
    void SetCustomSkyWavelengths(float X, float Y, float Z);
    
    /** Returns the skyplane */
    MeshInfo* GetSkyPlane();

    /** Returns the cloud meshes */
    std::vector<GMesh*>& GetCloudMeshes();

    /** Returns the atmospheric parameters */
    AtmosphereConstantBuffer& GetAtmosphereCB() { return AtmosphereCB; }

    /** returns atmosphere settings */
    AtmosphereSettings& GetAtmoshpereSettings() { return Atmosphere; }

    /** Returns the current sky-light color */
    float4 GetSkylightColor();

    /** Returns the cloud texture */
    D3D11Texture* GetCloudTexture();

    /** Returns the night texture */
    D3D11Texture* GetNightTexture();

    /** Backend-neutral form of the two accessors above — same resolution (a ZenGin override texture if a
        script set one and it is cached in, else our own loaded DDS), without the D3D11 downcast. The D3D12
        sky dome binds these and downcasts with D3D12Texture::From itself. */
    GfxTexture* GetCloudTextureGfx();
    GfxTexture* GetNightTextureGfx();

    /** Returns the current sun color */
    float3 GetSunColor();

    /** Where the moon should be composited this frame, in pixels of a `resolution`-sized backbuffer.
        Port of the planets[1] half of zCSkyControler_Outdoor::RenderPlanets - see GSky.cpp. Used by both
        backends' atmospheric-scattering sky, which draws no planets of its own. */
    MoonSpriteInfo ResolveMoonSprite( const INT2& resolution );

protected:
    /** Loads the sky textures */
    XRESULT LoadSkyResources();

    /** Adds a sky texture. Sky textures must be in order to make the daytime work */
    XRESULT AddSkyTexture( const std::string& file );

    /** Sky mesh */
    std::unique_ptr<GMesh> SkyDome;

    /** Skyplane */
    std::unique_ptr<MeshInfo> SkyPlane;

    /** Sky textures by time:
        1=0	0.25	0.5	  0.75	  1=0
        Day	Evening	Night Morning Day
    */
    std::vector<GfxTexture*> SkyTextures;

    std::unique_ptr<GfxTexture> CloudTexture;
    std::unique_ptr<GfxTexture> NightTexture;

    zCTexture* CloudTexture_Zen = nullptr;
    zCTexture* NightTexture_Zen = nullptr;

    std::unique_ptr<D3D11VertexBuffer> SkyPlaneVertexBuffer;
    ExVertexStruct SkyPlaneVertices[6];

    /** Atmospheric variables */
    AtmosphereConstantBuffer AtmosphereCB;
    AtmosphereSettings Atmosphere;
};
