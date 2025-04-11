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

class zCTexture;
class zCSkyLayer;
class zCSkyState;
class GMesh;
class D3D11Texture;

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
    void SetCustomSkyWavelengths( float X, float Y, float Z );

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

    /** Returns the current sun color */
    float3 GetSunColor();

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
    std::vector<D3D11Texture*> SkyTextures;

    std::unique_ptr<D3D11Texture> CloudTexture;
    std::unique_ptr<D3D11Texture> NightTexture;

    zCTexture* CloudTexture_Zen = nullptr;
    zCTexture* NightTexture_Zen = nullptr;

    std::unique_ptr<D3D11VertexBuffer> SkyPlaneVertexBuffer;
    ExVertexStruct SkyPlaneVertices[6];

    /** Atmospheric variables */
    AtmosphereConstantBuffer AtmosphereCB;
    AtmosphereSettings Atmosphere;
};
