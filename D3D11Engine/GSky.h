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

/** The moon as a light source. Direction is world space, toward the moon; zero when there is no outdoor sky. */
struct MoonLightInfo {
    XMFLOAT3 Direction = { 0.0f, 0.0f, 0.0f };
    float AboveHorizonFade = 0.0f;              // 0 below the horizon, 1 from ~14 degrees up
    XMFLOAT3 LightColor = { 0.0f, 0.0f, 0.0f }; // gamma-space direct moonlight, 0 unless the moon is the main light
    float GlintVisibility = 0.0f;               // strength of the moon's reflection on water
    float DiscVisibility = 0.0f;                // how visible the moon disc is in the sky, day or night
    XMFLOAT3 NightFill = { 0.0f, 0.0f, 0.0f };  // gamma-space indirect night light for outdoor worlds
    XMFLOAT3 Tint = { 0.42f, 0.56f, 1.0f };     // LightColor = Tint * Intensity
    float Intensity = 0.0f;
    bool IsMainLight = false;                   // the moon drives the shadow cascades instead of the sun
};

/** Dynamic-weather internals for the dev ImGui column. See GSky::UpdateWeather. */
struct WeatherDebugInfo {
    bool Active = false;        // model running (setting on, outdoor scattering world)
    float DayCounter = 0.0f;    // Gothic's day number (own wrap count where the offset is unknown)
    int K = 0;                  // round( dayCounter + masterTime ): the calendar day
    float SeedTarget = 0.0f;    // seed target blended across the night
    float TextureBias = 0.0f;   // from the sky layer textures (+ WeatherBias)
    float RainRamp = 0.0f;
    float Raw = 0.0f;           // overcast before smoothing
    float Overcast = 0.0f;      // final, smoothed
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

    /** Moon direction plus the light and water glint it gives at night. */
    MoonLightInfo GetMoonLight();

    /** Direction toward the shadow-casting light: the sun by day, the moon at night. */
    XMFLOAT3 GetMainLightDirection();

    /** True while the sun is above the horizon or the moon has taken over as the main light. */
    bool IsMainLightUp();

    /** Whether the low cloud passes run this frame (setting, outdoor world, not drowned by rain or fog). */
    bool AreLowCloudsVisible();

    /** Constants for Shaders/include/LowClouds.hlsl, shared by both backends. */
    void FillLowCloudConstants( LowCloudConstantBuffer& cb );

    /** Per-frame dynamic-weather step (called at the start of RenderSky). Overcast stays 0 with scattering
        off, indoors or WeatherEnabled false. */
    void UpdateWeather();

    /** 0 = clear, 1 = fully overcast; smoothed over real time. */
    float GetOvercast() const { return Overcast; }

    /** Subtle cloud tint, (1,1,1) = neutral, at most +-10% per channel. */
    XMFLOAT3 GetWeatherTint() const { return WeatherTint; }

    /** Weight for the RainSunLightStrength lerps: max( min( 1, rain * 2 ), 0.7 * overcast ). */
    float GetSunDimWeight( float rainWeight ) const;

    /** fogColorMod desaturated toward its luminance and darkened (up to 35%) by the overcast. */
    XMFLOAT3 GetWeatherFogColorMod( const XMFLOAT3& fogColorMod ) const;

    /** Weight (0.3 * overcast) for lerping the fog density toward the rain fog density. */
    float GetWeatherFogDensityWeight() const { return 0.3f * Overcast; }

    WeatherDebugInfo GetWeatherDebug() const { return WeatherDebug; }
    bool IsWeatherActive() const { return WeatherDebug.Active; }

    /** Tunables; the settings layer wires them up. Override < 0 = off, otherwise it replaces the
        seed+bias overcast (rain still raises it). */
    void SetWeatherEnabled( bool enabled ) { WeatherEnabled = enabled; }
    void SetWeatherVariety( float variety ) { WeatherVariety = std::clamp( variety, 0.0f, 1.0f ); }
    void SetWeatherBias( float bias ) { WeatherBias = std::clamp( bias, -1.0f, 1.0f ); }
    void SetWeatherOverride( float overcast ) { WeatherOverride = overcast; }

    bool WeatherEnabled = true;
    float WeatherVariety = 1.0f;
    float WeatherBias = 0.0f;
    float WeatherOverride = -1.0f;

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

    /** Dynamic weather state */
    float Overcast = 0.0f;
    XMFLOAT3 WeatherTint = XMFLOAT3( 1, 1, 1 );
    WeatherDebugInfo WeatherDebug;
    bool WeatherSmoothingValid = false;
    int64_t WeatherLastTickNs = 0;
    float WeatherLastMaster = 0.0f;
    int WeatherLocalDays = 0;           // own wrap count for builds without a dayCounter offset
    float WeatherTexBias = 0.0f;        // last valid bias from the sky textures
    XMFLOAT3 WeatherTexTint = XMFLOAT3( 1, 1, 1 );
    const zCTexture* WeatherKeyTex[2] = {};
    int WeatherKeyAlpha = -1;
    int WeatherRetryFrames = 0;
    const zCTexture* WeatherCacheInTried[4] = {};
};
