#include <Windows.h>
#include <algorithm>
#include <string>
#include <sstream>
#include "pch.h"
#include "GothicAPI.h"
#include "Engine.h"
#include "BaseGraphicsEngine.h"
#include "zCPolygon.h"
#include "WorldConverter.h"
#include "HookedFunctions.h"
#include "zCMaterial.h"
#include "zCTexture.h"
#include "zCVisual.h"
#include "zCVob.h"
#include "zCClassDef.h"
#include "zCProgMeshProto.h"
#include "zCCamera.h"
#include "oCGame.h"
#include "zCModel.h"
#include "zCMorphMesh.h"
#include "zCParticleFX.h"
#include "GSky.h"
#include "GInventory.h"

#define DIRECTINPUT_VERSION 0x0700
#include <charconv>
#include <numeric>
#include <dinput.h>
#include "ImGuiShim.h"
#include "zCInput.h"
#include "zCBspTree.h"
#include "BaseLineRenderer.h"
#include "D3D11PShader.h"
#include "D3D11VShader.h"
#include "D3D7\MyDirect3DDevice7.h"
#include "GVegetationBox.h"
#include "oCNPC.h"
#include "oCVisFX.h"
#include "zCMeshSoftSkin.h"
#include "zCVobLight.h"
#include "zCQuadMark.h"
#include "zCFlash.h"
#include "zCOption.h"
#include "zCRndD3D.h"
#include "win32ClipboardWrapper.h"
#include "zCSoundSystem.h"
#include "zCView.h"

// TODO: REMOVE THIS!
#include <ranges>

#include "D3D11GraphicsEngine.h"
#include "D3D11PipelineStateCache.h"
#include "MeshManager.h"
#include "SharedVisualRegistry.h"
#include "AsyncVisualExtractor.h"
#include "ThreadPool.h"
#include "zFILE.h"
#include "zFILE_VDFS.h"

#ifndef PUBLIC_RELEASE
#define OPT_DBG_NOINLINE __declspec(noinline)
#else
#define OPT_DBG_NOINLINE
#endif

// Duration how long the scene will stay wet, in MS
const DWORD SCENE_WETNESS_DURATION_MS = 20 * 1000;

// Draw ghost from back to front of our camera

extern float vobAnimation_WindStrength;

/** Writes this info to a file */
void MaterialInfo::WriteToFile( const std::string_view name ) {
    thread_local std::string infoPath{};
    infoPath.reserve( 255 );
    infoPath.clear();

    infoPath.append(R"(system\GD3D11\textures\infos\)");
    infoPath.append(name);
    infoPath.append(".mi");
    FILE* f = fopen( infoPath.c_str(), "wb" );

    if ( !f ) {
        LogError() << "Failed to open file '" << infoPath << "' for writing! Make sure the game runs in Admin mode "
            " to get the rights to write to that directory!";

        return;
    }

    // Write the version first
    fwrite( &MATERIALINFO_VERSION, sizeof( MATERIALINFO_VERSION ), 1, f );

    // Then the data
    fwrite( &buffer, sizeof( MaterialInfo::Buffer ), 1, f );
    fclose( f );
}

/** Loads this info from a file */
void MaterialInfo::LoadFromFile( const std::string_view name ) {
    
    bool foundFile = false;
    char ReadBuffer[sizeof( int ) + sizeof( MaterialInfo::Buffer )];
    
    thread_local std::string filePath{};
    filePath.reserve( 255 );
    filePath.clear();

    filePath.append(R"(\system\GD3D11\textures\infos\)");
    filePath.append( name.data(), name.size() );
    filePath.append( ".mi" );
    {
        auto vdfsFile = zFILE_VDFS::Create(filePath.c_str());
        if ( vdfsFile->Exists()
            && vdfsFile->Open(false) == zERROR_NONE )
        {
            vdfsFile->Read(ReadBuffer, sizeof(ReadBuffer));
            vdfsFile->Close();
            foundFile = true;
        }
    }
    
    if (!foundFile) {
        return;
    }
    // Write the version first
    int version;
    memcpy( &version, ReadBuffer, sizeof( int ) );
    if (version < 6) {
        buffer.SetDefault();
        return;
    }
    
    // Then the data
    ZeroMemory( &buffer, sizeof( MaterialInfo::Buffer ) );
    memcpy( &buffer, ReadBuffer + sizeof( int ), sizeof( MaterialInfo::Buffer ) );

    if ( version < 2 ) {
        if ( buffer.DisplacementFactor == 0.0f ) {
            buffer.DisplacementFactor = 0.7f;
        }
    }

    buffer.Color = float4( 1, 1, 1, 1 );
}

GothicAPI::GothicAPI() {
    OriginalGothicWndProc = 0;

    TextureTestBindMode = false;

    ZeroMemory( BoundTextures, sizeof( BoundTextures ) );

    CameraReplacementPtr = nullptr;
    WrappedWorldMesh = nullptr;
    CurrentCamera = nullptr;

    MainThreadID = GetCurrentThreadId();

    _canRain = false;
    _canClearVobsByVisual = false;

    SkeletalMeshVobs.reserve(300);
    AnimatedSkeletalVobs.reserve(300);
    DynamicallyAddedVobs.reserve(100);
}

GothicAPI::~GothicAPI() {
    //ResetWorld(); // Just let it leak for now. // TODO: Do this properly
    SAFE_DELETE( WrappedWorldMesh );
}

namespace
{
    constexpr uint32_t WORLD_SECTION_BVH_LEAF_SIZE = 8;

    struct WorldSectionBVHBuildPrimitive {
        WorldMeshSectionInfo* Section = nullptr;
        DirectX::BoundingBox Bounds = {};
        XMFLOAT3 Center = {};
    };

    bool IsValidSectionBounds( const zTBBox3D& box ) {
        return box.Min.x <= box.Max.x
            && box.Min.y <= box.Max.y
            && box.Min.z <= box.Max.z;
    }

    float GetAxisValue( const XMFLOAT3& value, int axis ) {
        switch ( axis ) {
        default:
        case 0: return value.x;
        case 1: return value.y;
        case 2: return value.z;
        }
    }

    DirectX::BoundingBox MergeBoundingBoxes( const DirectX::BoundingBox& a, const DirectX::BoundingBox& b ) {
        DirectX::BoundingBox merged;
        DirectX::BoundingBox::CreateMerged( merged, a, b );
        return merged;
    }

    OPT_DBG_NOINLINE float GetPrivateProfileFloatA(
        const LPCSTR lpAppName,
        const LPCSTR lpKeyName,
        const float nDefault,
        const std::string& lpFileName
    ) {
        constexpr int float_str_max = 30;
        TCHAR nFloat[float_str_max];
        if ( auto count = ::GetPrivateProfileStringA( lpAppName, lpKeyName, nullptr, nFloat, float_str_max, lpFileName.c_str() ) ) {
            float flt;
            auto dataPtr = &nFloat[0];
            auto [_, ec] = std::from_chars(dataPtr, dataPtr+count, flt);

            if (ec == std::errc{}) {
                return flt;
            }
            return nDefault;
        }
        return nDefault;
    }
    
    // Win32's GetPrivateProfileInt clamps negative values to 0, which kills any "-1 = auto" sentinel.
    // Parse the raw string ourselves instead.
    OPT_DBG_NOINLINE int GetPrivateProfileSignedIntA(
        const LPCSTR lpAppName,
        const LPCSTR lpKeyName,
        const int nDefault,
        const std::string& lpFileName
    ) {
        constexpr int int_str_max = 30;
        TCHAR nInt[int_str_max];
        if ( auto count = ::GetPrivateProfileStringA( lpAppName, lpKeyName, nullptr, nInt, int_str_max, lpFileName.c_str() ) ) {
            int value;
            auto dataPtr = &nInt[0];
            auto [_, ec] = std::from_chars( dataPtr, dataPtr + count, value );
            if ( ec == std::errc{} ) {
                return value;
            }
        }
        return nDefault;
    }

    template<typename T>
    std::string to_string_locale_independent(const T value) {
        std::array<char, 255> buffer;
        auto [ptr, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);

        if (ec == std::errc{}) {
            return std::string(buffer.data(), ptr);
        }
        return "";
    }

    // Helper function to trim leading/trailing whitespace
    std::string_view trim(std::string_view sv) {
        auto first = sv.find_first_not_of(" \t\n\r\f\v");
        if (std::string_view::npos == first) {
            return sv.substr(0, 0); // Return an empty view if all characters are whitespace
        }
        auto last = sv.find_last_not_of(" \t\n\r\f\v");
        return sv.substr(first, (last - first + 1));
    }

    template <typename T>
    OPT_DBG_NOINLINE bool parse_segment_from_chars( std::string_view sv, T* out ) {
        sv = trim( sv ); // Trim whitespace

        // Check if the type is supported by std::from_chars
        if constexpr ( !std::is_integral_v<T> && !std::is_floating_point_v<T> ) {
            // This static_assert will fire at compile time if you try to use
            // a type that from_chars doesn't support.
            static_assert(std::is_integral_v<T> || std::is_floating_point_v<T>,
                          "parse_segment_from_chars: Unsupported type T. Only integral and floating point types are supported.");
            // Need a runtime throw for compilers that might not hard-error on static_assert(false) in unreachable code
            throw std::runtime_error( "Internal error: Unsupported type T reached runtime parse logic." ); // Should not happen
        }

        if ( sv.empty() ) {
            // from_chars cannot parse empty strings
            return false;
        }

        T value;
        auto result = std::from_chars( sv.data(), sv.data() + sv.size(), value );
        if ( result.ec == std::errc() && out ) {
            *out = value;
            return true;
        }
        return false;
    }

    template <typename T>
    OPT_DBG_NOINLINE size_t parse_delimited_list_to_array( std::string_view input, T* output_array, size_t max_size, char delimiter = ',' ) {
        if ( !output_array || max_size == 0 ) {
            return 0; // Nothing to do
        }

        size_t count = 0;
        size_t start = 0;

        while ( start < input.size() ) {
            // Check if we have space BEFORE parsing
            if ( count >= max_size ) {
                // We found more segments than the array can hold
                return max_size;
            }

            size_t end = input.find( delimiter, start );
            std::string_view segment;

            if ( end == std::string_view::npos ) {
                segment = input.substr( start );
                start = input.size(); // Process the rest and exit loop
            } else {
                segment = input.substr( start, end - start );
                start = end + 1;
            }

            std::string_view trimmed_segment = trim( segment );

            // Skip empty segments after trimming
            if ( trimmed_segment.empty() ) {
                continue;
            }

            if ( T value; parse_segment_from_chars( trimmed_segment, &value ) ) {
                output_array[count++] = value; // Store and increment count
            }
        }

        return count; // Return the number of elements successfully parsed and stored
    }

    template<typename T>
    OPT_DBG_NOINLINE void GetPrivateProfileArray(
        const LPCSTR lpAppName,
        const LPCSTR lpKeyName,
        T* values,
        const size_t count,
        const T* defaults,
        const std::string& lpFileName
    ) {
        const int buf_max = 512;
        TCHAR buffer[buf_max];

        // Get the full string value
        if ( auto len = ::GetPrivateProfileStringA( lpAppName, lpKeyName, nullptr, buffer, buf_max, lpFileName.c_str() ) ) {
            std::string_view str( buffer, len );

            // parse and fill all remaining values with defaults if key not found
            for ( size_t i = parse_delimited_list_to_array( str, values, count ); i < count; ++i ) {
                values[i] = defaults[i];
            }
        }
    }

    template<typename T>
    OPT_DBG_NOINLINE void GetPrivateProfileRGB(
        const LPCSTR lpAppName,
        const LPCSTR lpKeyName,
        T& values,
        const std::string& lpFileName
    ) {
        const int defaults[3] = {
            static_cast<int>(values.x * 255.0f),
            static_cast<int>(values.y * 255.0f),
            static_cast<int>(values.z * 255.0f),
        };
        int color[3] = {
            static_cast<int>(values.x * 255.0f),
            static_cast<int>(values.y * 255.0f),
            static_cast<int>(values.z * 255.0f),
        };
        GetPrivateProfileArray(lpAppName, lpKeyName, color, 3, defaults, lpFileName);
        values.x = static_cast<float>(color[0]) / 255.0f;
        values.y = static_cast<float>(color[1]) / 255.0f;
        values.z = static_cast<float>(color[2]) / 255.0f;
    }

    template<typename T>
    OPT_DBG_NOINLINE void WritePrivateProfileArray(
        const LPCSTR lpAppName,
        const LPCSTR lpKeyName, 
        T* values,
        const size_t count,
        const std::string& lpFileName
    ) {
        std::stringstream ss;
        ss.imbue(std::locale::classic());

        for (size_t i = 0; i < count; i++)
        {
            ss << values[i];
            if (i < count - 1) {
                ss << ",";
            }
        }
        WritePrivateProfileStringA(lpAppName, lpKeyName, ss.str().c_str(), lpFileName.c_str());
    }

    OPT_DBG_NOINLINE void WritePrivateProfileRGB(
        const LPCSTR lpAppName,
        const LPCSTR lpKeyName, 
        float3 values,
        const std::string& lpFileName
    ) {
        int color[3] = {
            static_cast<int>(values.x * 255.0f),
            static_cast<int>(values.y * 255.0f),
            static_cast<int>(values.z * 255.0f),
        };
        WritePrivateProfileArray(lpAppName, lpKeyName, color, 3, lpFileName.c_str());
    }

    OPT_DBG_NOINLINE std::string GetPrivateProfileStringA(
        const LPCSTR lpAppName,
        const LPCSTR lpKeyName,
        const std::string& lpcstrDefault,
        const std::string& lpFileName ) {
        char buffer[MAX_PATH];
        auto count = ::GetPrivateProfileStringA( lpAppName, lpKeyName, lpcstrDefault.c_str(), buffer, MAX_PATH, lpFileName.c_str() );
        return std::string( buffer, count );
    }

    OPT_DBG_NOINLINE bool GetPrivateProfileBoolA(
        const LPCSTR lpAppName,
        const LPCSTR lpKeyName,
        const bool nDefault,
        const std::string& lpFileName ) {
        return GetPrivateProfileIntA( lpAppName, lpKeyName, nDefault, lpFileName.c_str() ) ? true : false;
    }
    
    static std::string float_to_string(const float val, int precision = 6)
    {
        std::stringstream ss;
        ss.imbue(std::locale::classic());

        ss << std::fixed << std::setprecision(precision) << val;
        return ss.str();
    }
    
    zCCamera* GetSceneCamera() {
        if ( !oCGame::GetGame()->_zCSession_camVob )
            return zCCamera::GetCamera();
        
        if (auto cam = static_cast<zCCamera*>(oCGame::GetGame()->_zCSession_camera); cam) {
            return cam;
        }
        return zCCamera::GetCamera();
    }
}

void GothicAPI::ProcessVobAnimation( zCVob* vob, zTAnimationMode aniMode, VobInstanceInfo& vobInstance ) {
    if ( Engine::GAPI->GetRendererState().RendererSettings.WindQuality == GothicRendererSettings::EWindQuality::WIND_QUALITY_ADVANCED ) {
        vobInstance.windStrenth = std::max<float>( 0.1f, vob->GetVisualAniModeStrength() ) * vobAnimation_WindStrength;
    }
}

/** Called when the game starts */
void GothicAPI::OnGameStart() {
    // Get threadid of main thread here because DllMain can be called from different thread
    MainThreadID = GetCurrentThreadId();

    LoadMenuSettings( MENU_SETTINGS_FILE );

    LogInfo() << "Running with Commandline: " << zCOption::GetOptions()->GetCommandline();

    // Get forced resolution from commandline
    std::string res = zCOption::GetOptions()->ParameterValue( "ZRES" );
    if ( !res.empty() ) {
        std::string x = res.substr( 0, res.find_first_of( ',' ) );
        std::string y = res.substr( res.find_first_of( ',' ) + 1 );
        RendererState.RendererSettings.LoadedResolution.x = std::stoi( x );
        RendererState.RendererSettings.LoadedResolution.y = std::stoi( y );

        LogInfo() << "Forcing resolution via zRes-Commandline to: " << RendererState.RendererSettings.LoadedResolution.toString();
    }

#ifdef PUBLIC_RELEASE
#ifndef BUILD_GOTHIC_1_08k
    // See if the user correctly installed the normalmaps
    CheckNormalmapFilesOld();
#endif
#endif

    LoadedWorldInfo = std::make_unique<WorldInfo>();
    LoadedWorldInfo->HighestVertex = 2;
    LoadedWorldInfo->LowestVertex = 3;
    LoadedWorldInfo->MidPoint = XMFLOAT2( 4, 5 );

    // Get start directory
    char dir[MAX_PATH];
    GetCurrentDirectoryA( MAX_PATH, dir );
    StartDirectory = dir;

    InitializeCriticalSection( &ResourceCriticalSection );

    SkyRenderer = std::make_unique<GSky>();
    SkyRenderer->InitSky();

    Inventory = std::make_unique<GInventory>();

    UpdateMTResourceManager();

#if defined(BUILD_GOTHIC_1_CLASSIC)
    HookedFunctions::OriginalFunctions.InitAnimatedInventoryHooks();
#endif
    void RegisterBinkPlayerHooks();
    RegisterBinkPlayerHooks();
}

/** Called to update the multi thread resource manager state */
void GothicAPI::UpdateMTResourceManager() {
    // Show memory profiller
/*#ifndef PUBLIC_RELEASE
    #ifdef BUILD_GOTHIC_1_08k
    PatchAddr( 0x005B61C0, "\x75" );
    #endif
    #ifdef BUILD_GOTHIC_2_6_fix
    PatchAddr( 0x005DD560, "\x75" );
    #endif
#endif*/

    // ZENGIN itself ships with this enabled (zEngine: SetThreadingEnabled(!zoptions->Parm("ZNORESTHREAD"))),
    // so the loader thread is the vanilla configuration and we treat it as sound. Our side of it is what
    // had to be fixed: the texture being cached in is thread-local now (GothicAPI::ScopedLoadingTexture),
    // because a process-global slot let the loader thread and the game thread clobber each other and a
    // surface would latch the wrong texture name permanently.
    // Only ever set this once, here at init: SetThreadingEnabled gates zCResourceManager's own
    // Lock/UnlockCacheInQueue, so flipping it at runtime can leave that critical section locked forever.
    if ( zCResourceManager* rsm = zCResourceManager::GetResourceManager() ) {
        rsm->SetThreadingEnabled( RendererState.RendererSettings.MTResoureceManager );
    } else {
        LogWarn() << "zCResourceManager not created yet - MultiThreadResourceManager setting not applied, "
            "ZENGIN keeps its own default (threading enabled)";
    }
}

/** Called to update the texture quality */
void GothicAPI::UpdateTextureMaxSize() {
    if ( zCResourceManager* rsm = zCResourceManager::GetResourceManager() ) {
        if ( oCGame::GetGame() ) {
            zCResourceManager::RefreshTexMaxSize(RendererState.RendererSettings.textureMaxSize);
        }
        rsm->PurgeCaches( GothicMemoryLocations::zCClassDef::zCTexture );
    }
}

/** Called to update the world, before rendering */
void GothicAPI::OnWorldUpdate() {
    ZoneScopedN( "GothicAPI::OnWorldUpdate" );
    ++FrameNumber;
#if BUILD_SPACER
    zCBspBase* rootBsp = oCGame::GetGame()->_zCSession_world->GetBspTree()->GetRootNode();
    BspInfo* root = &BspLeafVobLists[rootBsp];

    if ( !root->OriginalNode )
        Engine::GAPI->OnWorldLoaded();
#endif
#ifdef BUILD_SPACER_NET
    if ( RendererState.RendererSettings.RunInSpacerNet ) {
        zCBspBase* rootBsp = oCGame::GetGame()->_zCSession_world->GetBspTree()->GetRootNode();
        BspInfo* root = &BspLeafVobLists[rootBsp];

        if ( !root->OriginalNode )
            Engine::GAPI->OnWorldLoaded();
    }
#endif

    // Retire background extractions that finished on their own and hand the references they held back
    // to ZENGIN. Has to happen at a top-level point like this one: a release can run a visual's
    // destructor, which re-enters us via OnVisualDeleted.
    s_AsyncVisualExtractor->DrainFinished();
    WorldConverter::PruneFinishedNodeVisuals();
    s_AsyncVisualExtractor->FlushReleases();

    RendererState.RendererInfo.Reset();
    RendererState.RendererInfo.FPS = GetFramesPerSecond();
    RendererState.GraphicsState.FF_Time = GetTimeSeconds();

    if ( zCCamera* camera = GetSceneCamera() ) {
        RendererState.RendererInfo.FarPlane = camera->GetFarPlane();
        RendererState.RendererInfo.NearPlane = camera->GetNearPlane();

        //GetSceneCamera()->Activate();
        SetViewTransform( camera->GetTransformDX( zCCamera::ETransformType::TT_VIEW ), false );
    }

    // Apply the hints for the sound system to fix voices in indoor locations being quiet
    // This was originally done in zCBspTree::Render
    zCWorld* world = oCGame::GetGame()->_zCSession_world;
    if ( !GMPModeActive ) {
        if ( IsCameraIndoor() ) {
            // Set mode to 2, which means we are indoors, but can see the outside
            if ( zCSoundSystem* sndSystem = zCSoundSystem::GetSoundSystem() )
                sndSystem->SetGlobalReverbPreset( 2, 0.6f );

            if ( world && world->GetSkyControllerOutdoor() )
                world->GetSkyControllerOutdoor()->SetCameraLocationHint( 1 );
        } else {
            // Set mode to 0, which is the default
            if ( zCSoundSystem* sndSystem = zCSoundSystem::GetSoundSystem() )
                sndSystem->SetGlobalReverbPreset( 0, 0.0f );

            if ( world && world->GetSkyControllerOutdoor() )
                world->GetSkyControllerOutdoor()->SetCameraLocationHint( 0 );
        }
    }

    // Do rain-effects
    zCSkyController_Outdoor* skyController;

    if ( world && (skyController = world->GetSkyControllerOutdoor()) != nullptr && _canRain ) {
        bool outdoor = (LoadedWorldInfo->BspTree->GetBspTreeMode() == zBSP_MODE_OUTDOOR);
        if ( RendererState.RendererSettings.AtmosphericScattering && outdoor ) {
            float lastMasterTime = skyController->GetLastMasterTime();
            float masterTime = skyController->GetMasterTime();
            if ( (lastMasterTime - masterTime) > 0.95f && masterTime < 0.02f ) {
#ifndef BUILD_GOTHIC_1_08k
                float timeStartRain = std::min<float>( float( rand() ) / float( RAND_MAX ), 0.958f );
                float timeStopRain = std::min<float>( timeStartRain + 0.042f + ( float( rand() ) / float( RAND_MAX ) * 0.06f ), 1.0f );
#else
                float timeStartRain = std::min<float>( float( rand() ) / float( RAND_MAX ), 0.96f );
                float timeStopRain = std::min<float>( timeStartRain + 0.04f + ( float( rand() ) / float( RAND_MAX ) * 0.04f ), 1.0f );
#endif
                int renderLightning = 0;
                if ( skyController->GetRainingCounter() > 3 && ( float( rand() ) / float( RAND_MAX ) ) > 0.6f )
                    renderLightning = 1;

                skyController->SetTimeStartRain( timeStartRain );
                skyController->SetTimeStopRain( timeStopRain );
                skyController->SetRenderLighting( renderLightning );
            }

            skyController->SetLastMasterTime( masterTime );
        }

#ifdef OPT_MANAGE_SKY_EFFECTS_SUPPORTED // see zCSkyController
        int enableSkyEffect = !RendererState.RendererSettings.EnableRain || !outdoor
            ? 0
            : 1;
        int skyEffects = zCSkyController::GetSkyEffectsEnabled();
        zCSkyController::SetSkyEffectsEnabled(enableSkyEffect);
        skyController->ProcessRainFX();
        zCSkyController::SetSkyEffectsEnabled(skyEffects);
#endif
    }

    if ( !_canRain ) {
        srand( time( nullptr ) );
        _canRain = true;
    }

    // Clean futures so we don't have an ever growing array of them
    CleanFutures();
}

/** Returns gothics fps-counter */
int GothicAPI::GetFramesPerSecond() {
    return ((vidGetFPSRate)GothicMemoryLocations::Functions::vidGetFPSRate)();
}

/** Returns wether the camera is indoor or not */
bool GothicAPI::IsCameraIndoor() {
    oCGame* ogame = oCGame::GetGame();
    if ( !ogame || !ogame->_zCSession_camVob || !ogame->_zCSession_camVob->GetGroundPoly() )
        return false;

    return ogame->_zCSession_camVob->GetGroundPoly()->GetLightmap() != nullptr;
}

/** Alpha of ZenGin's env-map overlay stage: envMapStrength * skyFogIntensity
    (zRenderManager.cpp:701-703). ZenGin's bInSector is per-polygon; the camera stands in for it, which
    differs only while straddling a portal. */
float GothicAPI::GetEnvMapStageAlpha( zCMaterial* mat ) {
    if ( !mat ) return 0.0f;
    return std::clamp( mat->GetEnvMapStrength() * GetSkyLightIntensity(), 0.0f, 1.0f );
}

/** Sky-fog intensity 0..1 (0.299r+0.587g+0.114b, zTypes3D.h:128), pinned to zCOLOR(100,100,100)
    indoors as ZenGin does. Peaks well below 1.0 even at noon, so it is NOT a brightness multiplier -
    used as one it darkens surfaces in broad daylight. GetSkyDayFactor is that. */
float GothicAPI::GetSkyLightIntensity() {
    float lumaFog = 100.0f * (0.299f + 0.587f + 0.114f);   // the in-sector zCOLOR(100,100,100)

    if ( !IsCameraIndoor() ) {
        oCGame* ogame = oCGame::GetGame();
        zCSkyController_Outdoor* sc = ogame && ogame->_zCSession_world
            ? ogame->_zCSession_world->GetSkyControllerOutdoor() : nullptr;
        if ( sc ) {
            zColor fog = sc->GetBackgroundColor();
            lumaFog = 0.299f * fog.bgra.r + 0.587f * fog.bgra.g + 0.114f * fog.bgra.b;
        }
    }

    return std::clamp( lumaFog * (1.0f / 255.0f), 0.0f, 1.0f );
}

/** Day/night brightness for the alpha-blended world surfaces that never receive lighting
    (D3D11ForwardPlusRenderer::BindShaderForTexture routes every BLEND/ADD material to the unlit
    fallbacks) over a vertex color baked at full daylight - without it ice and foam stay noon-bright at
    midnight. ZenGin needs no equivalent: its lightDyn already carries the time of day. CHOSEN, not
    ported - exactly 1.0 while the sun is up so daylight is unchanged. Both constants are look tuning. */
float GothicAPI::GetSkyDayFactor() {
    constexpr float kNightFactor = 0.35f;   // brightness after dusk
    constexpr float kDuskSharpness = 4.0f;  // how fast it crosses over around the horizon

    GSky* sky = GetSky();
    if ( !sky ) return 1.0f;

    // AC_LightPos.y is the sun height, -1 (midnight) .. 1 (noon)
    const float sunHeight = sky->GetAtmosphereCB().AC_LightPos.y;
    const float day = std::clamp( sunHeight * kDuskSharpness, 0.0f, 1.0f );
    return std::lerp( kNightFactor, 1.0f, day );
}

/** Returns whether the loaded world itself is an indoor level (mines, dungeons, ...) */
bool GothicAPI::IsIndoorWorld() const {
    if ( !LoadedWorldInfo || !LoadedWorldInfo->BspTree )
        return false;

    return LoadedWorldInfo->BspTree->GetBspTreeMode() == zBSP_MODE_INDOOR;
}

/** Returns total time */
float GothicAPI::GetTotalTime() {
    if ( zCTimer* timer = zCTimer::GetTimer() )
        return timer->totalTimeFloat;

    return 0.0f;
}

/** Returns total time DWORD */
DWORD GothicAPI::GetTotalTimeDW() {
    if ( zCTimer* timer = zCTimer::GetTimer() )
        return timer->totalTime;

    return 0;
}

/** Returns global time */
float GothicAPI::GetTimeSeconds() {
#ifdef BUILD_GOTHIC_1_08k
    if ( zCTimer* timer = zCTimer::GetTimer() )
        return timer->totalTimeFloat / 1000.0f; // Gothic 1 has this in seconds
#else
    if ( zCTimer* timer = zCTimer::GetTimer() )
        return timer->totalTimeFloatSecs;
#endif

    return 0.0f;
}

/** Returns the current frame time */
float GothicAPI::GetFrameTimeSec() {
#ifdef BUILD_GOTHIC_1_08k
    if ( zCTimer* timer = zCTimer::GetTimer() )
        return timer->frameTimeFloat / 1000.0f;
#else
    if ( zCTimer* timer = zCTimer::GetTimer() )
        return timer->frameTimeFloatSecs;
#endif
    return -1.0f;
}

/** Disables the input from gothic */
void GothicAPI::SetEnableGothicInput( bool value ) {
    zCInput* input = zCInput::GetInput();

    if ( !input )
        return;

    static int disableCounter = 0;

    // Check if everything has disabled input
    if ( disableCounter > 0 && value ) {
        disableCounter--;

        if ( disableCounter < 0 )
            disableCounter = 0;

        if ( disableCounter > 0 )
            return; // Do nothing, we decremented the counter and it's still not 0
    }

    if ( oCGame::GetPlayer() ) oCGame::GetPlayer()->SetSleeping( value ? 0 : 1 );
    if ( oCGame::GetGame() && oCGame::GetGame()->_zCSession_camVob ) oCGame::GetGame()->_zCSession_camVob->SetSleeping( value ? 0 : 1 );

    if ( !value ) {
        if ( disableCounter++ > 0 )
            return;
    }

#ifndef BUILD_SPACER
#ifndef BUILD_SPACER_NET
    // zMouse, false
    input->SetDeviceEnabled( 2, value ? 1 : 0 );
    input->SetDeviceEnabled( 1, value ? 1 : 0 );

    // ClearKeyBuffer - when using GD3D11 settings some keys will remain as pressed unless we do this
    input->ClearKeyBuffer();

    // Sometimes without this cursor aren't visible(it is only here as precaution)
    if ( value ) {
        while ( ShowCursor( false ) >= 0 );
    } else {
        while ( ShowCursor( true ) < 0 );
    }

    IDirectInputDevice7A* dInputMouse = *reinterpret_cast<IDirectInputDevice7A**>(GothicMemoryLocations::GlobalObjects::DInput7DeviceMouse);
    IDirectInputDevice7A* dInputKeyboard = *reinterpret_cast<IDirectInputDevice7A**>(GothicMemoryLocations::GlobalObjects::DInput7DeviceKeyboard);
    if ( dInputMouse ) {
        if ( !value )
            dInputMouse->Unacquire();
        else
            dInputMouse->Acquire();
    }

    if ( dInputKeyboard ) {
        if ( !value )
            dInputKeyboard->Unacquire();
        else
            dInputKeyboard->Acquire();
    }
#endif
#endif

}



/** Called when the window got set */
void GothicAPI::OnSetWindow( HWND hWnd ) {
    if ( OriginalGothicWndProc || !hWnd )
        return; // Dont do that twice

    OutputWindow = hWnd;

    // Start here, create our engine
    Engine::GraphicsEngine->SetWindow( hWnd );

    OriginalGothicWndProc = GetWindowLongPtrA( hWnd, GWL_WNDPROC );
    SetWindowLongPtrA( hWnd, GWL_WNDPROC, reinterpret_cast<LONG>(GothicWndProc) );
}

/** Returns the GraphicsState */
GothicRendererState& GothicAPI::GetRendererState() { return RendererState; }


/** Spawns a vegetationbox at the camera */
GVegetationBox* GothicAPI::SpawnVegetationBoxAt( const XMFLOAT3& position, const XMFLOAT3& min, const XMFLOAT3& max, float density, const std::string& restrictByTexture ) {
    GVegetationBox* v = new GVegetationBox;
    XMFLOAT3 minposition;
    XMFLOAT3 maxposition;
    XMStoreFloat3( &minposition, XMLoadFloat3( &min ) + XMLoadFloat3( &position ) );
    XMStoreFloat3( &maxposition, XMLoadFloat3( &max ) + XMLoadFloat3( &position ) );
    v->InitVegetationBox( minposition, maxposition, "", density, 1.0f, restrictByTexture );

    VegetationBoxes.push_back( v );

    return v;
}

/** Adds a vegetationbox to the world */
void GothicAPI::AddVegetationBox( GVegetationBox* box ) {
    VegetationBoxes.push_back( box );
}

/** Removes a vegetationbox from the world */
void GothicAPI::RemoveVegetationBox( GVegetationBox* box ) {
    VegetationBoxes.remove( box );
    delete box;
}

/** Resets the object, like at level load */
void GothicAPI::ResetWorld() {
    ResetVobs();
    ClearWorldSectionBVH();
    WorldSections.clear();

    SAFE_DELETE( WrappedWorldMesh );

    // Clear inventory too?
}

void GothicAPI::ReloadVobs() {
    ResetVobs();
    OnWorldLoaded();
}
void GothicAPI::ReloadPlayerVob() {
    auto player = static_cast<zCVob*>(oCGame::GetPlayer());
    if ( !player ) return;
    auto playerHomeworld = player->GetHomeWorld();
    if ( !playerHomeworld ) return;

    OnRemovedVob( player, playerHomeworld );
    OnAddVob( player, playerHomeworld );
}
/** Resets only the vobs */
void GothicAPI::ResetVobs() {
    
    // complete what ever is currently working, and clear everything else.
    Engine::WorkerThreadPool->clearAndFlush();
    
    // Delete light vobs, those depend on world sections and load stuff in the background.
    // by deleting them first we block the thread until the destructor finished
    for ( auto const& it : VobLightMap ) {
        Engine::GraphicsEngine->OnVobRemovedFromWorld( it.first );
        delete it.second;
    }
    VobLightMap.clear();
    
    // Clear sections
    for ( auto&& itx : Engine::GAPI->GetWorldSections() ) {
        for ( auto&& ity : itx.second ) {
            ity.second.Vobs.clear();
        }
    }

    // Remove vegetation
    ResetVegetation();

    // Clear helper-lists
    for ( zCVob* vob : ParticleEffectVobs ) {
        DestroyParticleEffect( vob );
    }

    FrameThunderPolyStrips.clear();
    FlashVisuals.clear();
    ParticleEffectVobs.clear();
    RegisteredVobs.clear();
    BspLeafVobLists.clear();
    LeafLinearCache.Clear();
    // Holds indices into the (now gone) sector arrays and BspInfo::SectorIds - must not outlive them.
    PortalCuller.Clear();
    DynamicallyAddedVobs.clear();
    DynamicMeshVobs.clear();   // non-owning, aliases VobMap's VobInfo* -- deleted below via VobMap, not here
    DecalVobs.clear();
    VobsByVisual.clear();
    SkeletalVobMap.clear();

    // Delete static mesh visuals
    for ( auto const& it : StaticMeshVisuals ) {
        delete it.second;
    }
    StaticMeshVisuals.clear();

    // Delete skeletal mesh visuals
    for ( auto const& it : SkeletalMeshVisuals ) {
        delete it.second;
    }
    for ( auto const& it : SkeletalMeshNpcs ) {
        delete it.second;
    }
    SkeletalMeshVisuals.clear();
    SkeletalMeshNpcs.clear();

    // Only the held references are left to hand back — clearAndFlush() above waited for every background
    // extraction and the infos they wrote into are gone. Safe inline rather than deferred: the maps are
    // empty, so the OnVisualDeleted a release may trigger finds nothing to tear down.
    s_AsyncVisualExtractor->CancelAll();

    // Delete static mesh vobs
    for ( auto const& it : VobMap ) {
        delete it.second;
    }
    VobMap.clear();

    // Delete skeletal mesh vobs
    for ( auto it : SkeletalMeshVobs ) {
        delete it;
    }
    SkeletalMeshVobs.clear();
    AnimatedSkeletalVobs.clear();

    // Every skeletal vob is gone, so this should find nothing left to own. Drops (and logs) whatever
    // is still there rather than carrying converted meshes into the next world.
    s_SharedVisualRegistry->Clear();
}

/** Called when the game loaded a new level */
void GothicAPI::OnGeometryLoaded( zCBspTree* tree ) {
    LogInfo() << "World loaded, getting Levelmesh now!";
    LogInfo() << " - Found " << tree->GetNumPolys() << " polygons";
    LogInfo() << "Extracting world";

    std::vector<zCPolygon*> polys;
    tree->GetLOD0Polygons( polys );
    GetLoadedWorldInfo()->BspTree = tree;

    ResetWorld();
    ResetMaterialInfo();

    bool indoorLocation = (LoadedWorldInfo->BspTree->GetBspTreeMode() == zBSP_MODE_INDOOR);
    std::string worldStr = "system\\GD3D11\\meshes\\WLD_" + LoadedWorldInfo->WorldName + ".obj";
    // Convert world to our own format
#ifdef BUILD_GOTHIC_2_6_fix
    WorldConverter::ConvertWorldMesh( &polys[0], polys.size(), &WorldSections, LoadedWorldInfo.get(), &WrappedWorldMesh, indoorLocation );
#else
    if ( Toolbox::FileExists( worldStr ) ) {
        WorldConverter::LoadWorldMeshFromFile( worldStr, &WorldSections, LoadedWorldInfo.get(), &WrappedWorldMesh );
        LoadedWorldInfo->CustomWorldLoaded = true;
    } else {
        WorldConverter::ConvertWorldMesh( &polys[0], polys.size(), &WorldSections, LoadedWorldInfo.get(), &WrappedWorldMesh, indoorLocation );
    }
#endif
    BuildWorldSectionBVH();
    LogInfo() << "Done extracting world!";
}

/** Called when the game is about to load a new level */
void GothicAPI::OnLoadWorld( const std::string& levelName, int loadMode ) {
    _canClearVobsByVisual = true;
    if ( (loadMode == zWLD_LOAD_GAME_STARTUP || loadMode == zWLD_LOAD_GAME_SAVED_STAT) ) {
        if ( !levelName.empty() ) {
            std::string name = levelName;
            const size_t last_slash_idx = name.find_last_of( "\\/" );
            if ( std::string::npos != last_slash_idx ) {
                name.erase( 0, last_slash_idx + 1 );
            }

            // Remove extension if present.
            const size_t period_idx = name.rfind( '.' );
            if ( std::string::npos != period_idx ) {
                name.erase( period_idx );
            }

            // Initial load
            LoadedWorldInfo->WorldName = name;
        }

        extern MeshManager* s_MeshManager;
        s_MeshManager->DropCaches();
        Engine::GraphicsEngine->OnLoadWorld();
    }

#ifndef PUBLIC_RELEASE
    // Disable input here, so you can tab out
    if ( loadMode == 2 ) {
        SetEnableGothicInput( false );
    }
#endif
}

/** Called when the game is done loading the world */
void GothicAPI::OnWorldLoaded() {
    _canRain = false;

    LoadCustomZENResources();

    LogInfo() << "Collecting vobs...";

    static bool s_firstLoad = true;
    if ( s_firstLoad ) {
        // Print information about the mod here.
        //TODO: Menu would be better, but that view doesn't exist then
        PrintModInfo();
        s_firstLoad = false;
    }

    LoadedWorldInfo->BspTree = oCGame::GetGame()->_zCSession_world->GetBspTree();

    // Get all VOBs
    zCTree<zCVob>* vobTree = oCGame::GetGame()->_zCSession_world->GetGlobalVobTree();
    TraverseVobTree( vobTree );

    // Build instancing cache for the static vobs for each section
    BuildStaticMeshInstancingCache();

    // Build vob info cache for the bsp-leafs
    BuildBspVobMapCache();

#ifdef BUILD_GOTHIC_1_08k
    if ( LoadedWorldInfo->CustomWorldLoaded ) {
        CreatezCPolygonsForSections();
        PutCustomPolygonsIntoBspTree();
    }
#endif

    LogInfo() << "Done!";

    LogInfo() << "Settings sky texture for " << LoadedWorldInfo->WorldName;

    // Hard code the original games sky textures here, since we can't modify the scripts to use the ikarus bindings without
    // installing more content like a .mod file
    if ( LoadedWorldInfo->WorldName == "OLDWORLD" || LoadedWorldInfo->WorldName == "WORLD" ) {
        GetSky()->SetSkyTexture( ESkyTexture::ST_OldWorld ); // Sky for gothic 2s oldworld
        RendererState.RendererSettings.SetupOldWorldSpecificValues();
    } else if ( LoadedWorldInfo->WorldName == "ADDONWORLD" ) {
        GetSky()->SetSkyTexture( ESkyTexture::ST_NewWorld ); // Sky for gothic 2s addonworld
        RendererState.RendererSettings.SetupAddonWorldSpecificValues();
    } else {
        GetSky()->SetSkyTexture( ESkyTexture::ST_NewWorld ); // Make newworld default
        RendererState.RendererSettings.SetupNewWorldSpecificValues();
    }

    // first load the global defaults, then the world specific ones
    LoadRendererWorldSettings( RendererState.RendererSettings, MENU_SETTINGS_FILE );
    LoadRendererWorldSettings( RendererState.RendererSettings );

    // Reset wetness
    SceneWetness = GetRainFXWeight();

#ifndef PUBLIC_RELEASE
    // Enable input again, disabled it when loading started
    SetEnableGothicInput( true );
#endif

    // Enable the editorpanel, if in spacer
#ifdef BUILD_SPACER
    Engine::GraphicsEngine->OnUIEvent( BaseGraphicsEngine::UI_OpenEditor );
#endif

    _canClearVobsByVisual = false;
}

void GothicAPI::LoadRendererWorldSettings( GothicRendererSettings& s )
{
    if ( !LoadedWorldInfo || LoadedWorldInfo->WorldName.empty() ) {
        return;
    }

    auto gameName = GetGameName();
    std::string zenFolder;
    if ( gameName == "Original" ) {
        zenFolder = "system\\GD3D11\\ZENResources\\";
    } else {
        zenFolder = "system\\GD3D11\\ZENResources\\" + gameName + "\\";
    }
    if ( !Toolbox::FolderExists( zenFolder ) ) {
        LogInfo() << "Custom ZEN-Resources. Directory not found: " << zenFolder;
        return;
    }

    auto const ini = zenFolder + LoadedWorldInfo->WorldName + ".INI";

    LoadRendererWorldSettings(s, ini.c_str());
}

void GothicAPI::LoadRendererWorldSettings( GothicRendererSettings& s, const char* iniFile ) {
    if ( !Toolbox::FileExists( iniFile ) ) {
        return;
    }
    
    if ( !LoadedWorldInfo || LoadedWorldInfo->WorldName.empty() ) {
        return;
    }

    const std::string ini = iniFile;
    if ( !Toolbox::FileExists( ini ) ) {
        return;
    }

    s.FogHeight = GetPrivateProfileFloatA( "Fog", "Height", s.FogHeight, ini );
    s.FogHeightFalloff = GetPrivateProfileFloatA( "Fog", "HeightFalloff", s.FogHeightFalloff, ini );
    s.FogGlobalDensity = GetPrivateProfileFloatA( "Fog", "GlobalDensity", s.FogGlobalDensity, ini );

    s.SunLightColor = float3::FromColor(
        GetPrivateProfileIntA( "Atmoshpere", "SunLightColorR", static_cast<int>(s.SunLightColor.x * 255.0f), ini.c_str() ),
        GetPrivateProfileIntA( "Atmoshpere", "SunLightColorG", static_cast<int>(s.SunLightColor.y * 255.0f), ini.c_str() ),
        GetPrivateProfileIntA( "Atmoshpere", "SunLightColorB", static_cast<int>(s.SunLightColor.z * 255.0f), ini.c_str() )
    );

    GetPrivateProfileRGB("Atmoshpere", "SunLightColor", s.SunLightColor, ini);

    s.FogColorMod = float3::FromColor(
        GetPrivateProfileIntA( "Atmoshpere", "FogColorModR", static_cast<int>(s.FogColorMod.x * 255.0f), ini.c_str() ),
        GetPrivateProfileIntA( "Atmoshpere", "FogColorModG", static_cast<int>(s.FogColorMod.y * 255.0f), ini.c_str() ),
        GetPrivateProfileIntA( "Atmoshpere", "FogColorModB", static_cast<int>(s.FogColorMod.z * 255.0f), ini.c_str() )
    );

    GetPrivateProfileRGB("Atmoshpere", "FogColorMod", s.FogColorMod, ini);

	s.GraphicsPreset = (GothicRendererSettings::E_GraphicsPreset)GetPrivateProfileIntA( "General", "GraphicsPreset", s.GraphicsPreset, ini.c_str() );
    if ( true ) {
	    s.VisualFXDrawRadius = GetPrivateProfileFloatA( "General", "VisualFXDrawRadius", s.VisualFXDrawRadius, ini );
	    s.VobLodDrawRadius = GetPrivateProfileFloatA( "General", "VobLodDrawRadius", s.VobLodDrawRadius, ini );
	    s.OutdoorVobDrawRadius = GetPrivateProfileFloatA( "General", "OutdoorVobDrawRadius", s.OutdoorVobDrawRadius, ini );
        s.OutdoorSmallVobDrawRadius = GetPrivateProfileFloatA( "General", "OutdoorSmallVobDrawRadius", s.OutdoorSmallVobDrawRadius, ini );
        s.IndoorVobDrawRadius = GetPrivateProfileFloatA( "General", "IndoorVobDrawRadius", s.IndoorVobDrawRadius, ini );
	    s.SkeletalMeshDrawRadius = GetPrivateProfileFloatA( "General", "SkeletalMeshDrawRadius", s.SkeletalMeshDrawRadius, ini );
	    s.SectionDrawRadius = GetPrivateProfileIntA( "General", "SectionDrawRadius", s.SectionDrawRadius, ini.c_str() );
    }

    s.RainRadiusRange = GetPrivateProfileFloatA( "Rain", "RadiusRange", s.RainRadiusRange, ini );
    s.RainHeightRange = GetPrivateProfileFloatA( "Rain", "HeightRange", s.RainHeightRange, ini );
    s.RainNumParticles = GetPrivateProfileIntA( "Rain", "NumParticles", s.RainNumParticles, ini.c_str() );
    GetPrivateProfileArray( "Rain", "GlobalVelocity", &s.RainGlobalVelocity.x, 3, &s.RainGlobalVelocity.x, ini );
    s.RainSceneWettness = GetPrivateProfileFloatA( "Rain", "SceneWettness", s.RainSceneWettness, ini );
    s.RainSunLightStrength = GetPrivateProfileFloatA( "Rain", "SunLightStrength", s.RainSunLightStrength, ini );
    GetPrivateProfileRGB( "Rain", "FogColor", s.RainFogColor, ini );
    s.RainFogDensity = GetPrivateProfileFloatA( "Rain", "FogDensity", s.RainFogDensity, ini );

    s.ReplaceSunDirection = GetPrivateProfileBoolA( "Atmoshpere", "ReplaceSunDirection", s.ReplaceSunDirection, ini );

    AtmosphereSettings& aS = GetSky()->GetAtmoshpereSettings();

    aS.LightDirection = XMFLOAT3(
        GetPrivateProfileFloatA( "Atmoshpere", "LightDirectionX", aS.LightDirection.x, ini ),
        GetPrivateProfileFloatA( "Atmoshpere", "LightDirectionY", aS.LightDirection.y, ini ),
        GetPrivateProfileFloatA( "Atmoshpere", "LightDirectionZ", aS.LightDirection.z, ini )
    );

    GetPrivateProfileArray("Atmoshpere", "LightDirection", &aS.LightDirection.x, 3, &aS.LightDirection.x, ini);

    s.GodRayDecay = GetPrivateProfileFloatA( "GodRays", "GodRayDecay", s.GodRayDecay, ini );
    s.GodRayDensity = GetPrivateProfileFloatA( "GodRays", "GodRayDensity", s.GodRayDensity, ini );
    s.GodRayWeight = GetPrivateProfileFloatA( "GodRays", "GodRayWeight", s.GodRayWeight, ini );
}

void GothicAPI::SaveRendererWorldSettings( const GothicRendererSettings& s )
{
    if ( !LoadedWorldInfo || LoadedWorldInfo->WorldName.empty() ) {
        return;
    }
    auto gameName = GetGameName();
    std::string zenFolder;
    if ( gameName == "Original" ) {
        zenFolder = "system\\GD3D11\\ZENResources\\";
    } else {
        zenFolder = "system\\GD3D11\\ZENResources\\" + gameName + "\\";
    }
    if ( !Toolbox::FolderExists( zenFolder ) ) {
        if ( !Toolbox::CreateDirectoryRecursive( zenFolder ) ) {
            LogError() << "Could not save custom ZEN-Resources. Could not create directory: " << zenFolder;
            return;
        }
    }

    auto const ini = zenFolder + LoadedWorldInfo->WorldName + ".INI";
    SaveRendererWorldSettings(s, ini.c_str());
}

void GothicAPI::SaveRendererWorldSettings( const GothicRendererSettings& s, const char* iniFile ) {
    const std::string ini = iniFile;

    WritePrivateProfileStringA( "Fog", "Height", to_string_locale_independent( s.FogHeight ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "Fog", "HeightFalloff", to_string_locale_independent( s.FogHeightFalloff ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "Fog", "GlobalDensity", to_string_locale_independent( s.FogGlobalDensity ).c_str(), ini.c_str() );

    WritePrivateProfileRGB("Atmoshpere", "SunLightColor", s.SunLightColor, ini);
    WritePrivateProfileRGB("Atmoshpere", "FogColorMod", s.FogColorMod, ini);

    WritePrivateProfileStringA( "General", "GraphicsPreset", to_string_locale_independent( (int)s.GraphicsPreset ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "General", "VisualFXDrawRadius", to_string_locale_independent( s.VisualFXDrawRadius ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "General", "VobLodDrawRadius", to_string_locale_independent( s.VobLodDrawRadius ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "General", "OutdoorVobDrawRadius", to_string_locale_independent( s.OutdoorVobDrawRadius ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "General", "OutdoorSmallVobDrawRadius", to_string_locale_independent( s.OutdoorSmallVobDrawRadius ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "General", "IndoorVobDrawRadius", to_string_locale_independent( s.IndoorVobDrawRadius ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "General", "SkeletalMeshDrawRadius", to_string_locale_independent( s.SkeletalMeshDrawRadius ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "General", "SectionDrawRadius", to_string_locale_independent( s.SectionDrawRadius ).c_str(), ini.c_str() );

    WritePrivateProfileStringA( "Rain", "RadiusRange", to_string_locale_independent( s.RainRadiusRange ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "Rain", "HeightRange", to_string_locale_independent( s.RainHeightRange ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "Rain", "NumParticles", to_string_locale_independent( s.RainNumParticles ).c_str(), ini.c_str() );
    WritePrivateProfileArray( "Rain", "GlobalVelocity", &s.RainGlobalVelocity.x, 3, ini.c_str() );
    WritePrivateProfileStringA( "Rain", "SceneWettness", to_string_locale_independent( s.RainSceneWettness ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "Rain", "SunLightStrength", to_string_locale_independent( s.RainSunLightStrength ).c_str(), ini.c_str() );
    WritePrivateProfileRGB( "Rain", "FogColor", s.RainFogColor, ini );
    WritePrivateProfileStringA( "Rain", "FogDensity", to_string_locale_independent( s.RainFogDensity ).c_str(), ini.c_str() );

    WritePrivateProfileStringA( "Atmoshpere", "ReplaceSunDirection", to_string_locale_independent( s.ReplaceSunDirection ? TRUE : FALSE ).c_str(), ini.c_str() );

    AtmosphereSettings& aS = GetSky()->GetAtmoshpereSettings();

    WritePrivateProfileArray("Atmoshpere", "LightDirection", &aS.LightDirection.x, 3, ini.c_str() );

    // delete old named keys

    WritePrivateProfileStringA( "Atmoshpere", "SunLightColorR", nullptr, ini.c_str() );
    WritePrivateProfileStringA( "Atmoshpere", "SunLightColorG", nullptr, ini.c_str() );
    WritePrivateProfileStringA( "Atmoshpere", "SunLightColorB", nullptr, ini.c_str() );

    WritePrivateProfileStringA( "Atmoshpere", "FogColorModR", nullptr, ini.c_str() );
    WritePrivateProfileStringA( "Atmoshpere", "FogColorModG", nullptr, ini.c_str() );
    WritePrivateProfileStringA( "Atmoshpere", "FogColorModB", nullptr, ini.c_str() );
    
    WritePrivateProfileStringA( "Atmoshpere", "LightDirectionX", nullptr, ini.c_str() );
    WritePrivateProfileStringA( "Atmoshpere", "LightDirectionY", nullptr, ini.c_str() );
    WritePrivateProfileStringA( "Atmoshpere", "LightDirectionZ", nullptr, ini.c_str() );

    WritePrivateProfileStringA( "GodRays", "GodRayDecay", to_string_locale_independent( s.GodRayDecay ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "GodRays", "GodRayDensity", to_string_locale_independent( s.GodRayDensity ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "GodRays", "GodRayWeight", to_string_locale_independent( s.GodRayWeight ).c_str(), ini.c_str() );    
}

/** Goes through the given zCTree and registers all found vobs */
void GothicAPI::TraverseVobTree( zCTree<zCVob>* tree ) {
    // Iterate through the nodes
    if ( tree->FirstChild != nullptr ) {
        TraverseVobTree( tree->FirstChild );
    }

    if ( tree->Next != nullptr ) {
        TraverseVobTree( tree->Next );
    }

    // Add the vob if it exists and has a visual
    if ( tree->Data ) {
        if ( tree->Data->GetVisual() )
            OnAddVob( tree->Data, oCGame::GetGame()->_zCSession_world );
    }
}

void GothicAPI::TraverseVobTree( zCTree<zCVob>* tree, std::function<void( zCVob* )> handler ) {
    if ( tree->FirstChild != nullptr ) {
        TraverseVobTree( tree->FirstChild, handler );
    }

    if ( tree->Next != nullptr ) {
        TraverseVobTree( tree->Next, handler );
    }

    if ( tree->Data ) {
        handler( tree->Data );
    }
}

/** Returns in which directory we started in */
const std::string& GothicAPI::GetStartDirectory() {
    return StartDirectory;
}

/** Builds the static mesh instancing cache */
void GothicAPI::BuildStaticMeshInstancingCache() {
    for ( auto const& it : StaticMeshVisuals ) {
        it.second->StartNewFrame();
    }
}

/** Returns if a player is NOT in a dialog with a npc */
int GothicAPI::DialogFinished() {
    static GetInformationManagerProc GetInformationManager = reinterpret_cast<GetInformationManagerProc>(GothicMemoryLocations::oCInformationManager::GetInformationManager);
    return *reinterpret_cast<int*>(GetInformationManager() + GothicMemoryLocations::oCInformationManager::IsDoneOffset);
}

static bool GetShouldRenderAsMorphMesh(SkeletalVobInfo* vi, zCModel* model) {
    auto& nodeAttachments = vi->NodeAttachments;
    auto nodeList = model->GetNodeList();
    auto numTransforms = static_cast<unsigned int>(nodeList->NumInArray);

    for ( unsigned int i = 0; i < numTransforms; ++i ) {
        // Check for new visual
        zCModelNodeInst* node = nodeList->Array[i];

        if ( !node->NodeVisual )
            continue; // Happens when you pull your sword for example

        // Check if this is loaded
        auto nodeAttachment = nodeAttachments.find( i );
        if ( node->NodeVisual && nodeAttachment == nodeAttachments.end() ) {
            // It's not, will be fixed in next frame.
            continue;
        }

        // Check for changed visual
        if ( nodeAttachments[i].size() && node->NodeVisual != nodeAttachments[i][0]->Visual ) {
            // will be fixed in next frame.
            continue;
        }

        if ( model->GetDrawHandVisualsOnly() ) {
            std::string NodeName = node->ProtoNode->NodeName.ToChar();
#ifdef BUILD_GOTHIC_2_6_fix
            if ( NodeName.find( "HAND" ) == std::string::npos && (*reinterpret_cast<BYTE*>(0x57A694) != 0x90 || NodeName.find( "ARM" ) == std::string::npos) ) {
#else
            if ( NodeName.find( "HAND" ) == std::string::npos ) {
#endif
                continue;
            }
        }

        if ( nodeAttachment != nodeAttachments.end() ) {
            // Go through all attachments this node has
            for ( MeshVisualInfo* mvi : nodeAttachment->second ) {
                if ( !mvi->Visual ) {
                    continue;
                }

                bool isMMS = strcmp( mvi->Visual->GetFileExtension( 0 ), ".MMS" ) == 0;
                if ( isMMS ) {
                    return true;
                }
            }
        }
    }
    return false;
}

/** Draws the world-mesh */
void GothicAPI::DrawWorldMeshNaive() {
    ZoneScopedN( "GothicAPI::DrawWorldMeshNaive" );
    if ( !GetSceneCamera() || !oCGame::GetGame() )
        return;

    static float setfovH = RendererState.RendererSettings.FOVHoriz;
    static float setfovV = RendererState.RendererSettings.FOVVert;

/*
#ifdef BUILD_GOTHIC_1_08k
    if ( RendererState.RendererSettings.ForceFOV ) {
        setfovH = RendererState.RendererSettings.FOVHoriz;
        setfovV = RendererState.RendererSettings.FOVVert;

        // Fix camera FOV-Bug
        GetSceneCamera()->SetFOV( RendererState.RendererSettings.FOVHoriz, (Engine::GraphicsEngine->GetResolution().y / static_cast<float>(Engine::GraphicsEngine->GetResolution().x)) * RendererState.RendererSettings.FOVVert );

        CurrentCamera = GetSceneCamera();
    }
#else
*/
#if defined(BUILD_GOTHIC_1_08k) || defined(BUILD_1_12F) || defined(BUILD_GOTHIC_2_6_fix)
    if ( RendererState.RendererSettings.ForceFOV ) {
        zCCamera* camera = GetSceneCamera();
        if ( camera )
            camera->GetFOV( setfovH, setfovV );

        if ( camera
            // FIXME: This is being reset after a dialog!
            && (camera != CurrentCamera || setfovH != RendererState.RendererSettings.FOVHoriz || setfovV != RendererState.RendererSettings.FOVVert || (setfovH == 90.0f && setfovV == 90.0f)) ) {
            // if player is in a dialog state with a npc, we do not change FOV, or create an option for it in F11 menu
            if ( DialogFinished() ) {
                setfovH = RendererState.RendererSettings.FOVHoriz;
                setfovV = RendererState.RendererSettings.FOVVert;

                // Fixing camera FOV-Bug, set it with DX11 settings
                camera->SetFOV( RendererState.RendererSettings.FOVHoriz,
                    (Engine::GraphicsEngine->GetResolution().y / static_cast<float>(Engine::GraphicsEngine->GetResolution().x)) * RendererState.RendererSettings.FOVVert );
                camera->Activate();

                CurrentCamera = camera;
            }

        }
    }
#endif
//#endif

    FrameParticleInfo.clear();
    FrameParticles.clear();
    FrameMeshInstances.clear();

    {
        ZoneScopedN( "World Mesh" );
        auto _1 = Engine::GraphicsEngine->RecordGraphicsEvent( GE_NAME( "World Mesh" ) );
        Engine::GraphicsEngine->DrawWorldMesh();
    }

    const auto cameraPosXm = GetCameraPositionXM();

    if ( RendererState.RendererSettings.DrawSkeletalMeshes ) {
        ZoneScopedN( "Animated Skeletal Meshes" );
        auto _1 = Engine::GraphicsEngine->RecordGraphicsEvent( GE_NAME( "Animated Skeletal Meshes" ) );

        // Set up frustum for the camera
        RendererState.RasterizerState.SetDefault();
        RendererState.RasterizerState.SetDirty();
        GetSceneCamera()->Activate();

        auto drawRadius = RendererState.RendererSettings.SkeletalMeshDrawRadius;

        static std::vector<SkeletalVobInfo*> drawAsMorphMesh;
        static std::vector<SkeletalVobInfo*> drawRegular;
        drawAsMorphMesh.reserve(50);
        drawRegular.reserve(200);

        for ( const auto& vobInfo : AnimatedSkeletalVobs ) {
            // Don't render if sleeping and has skeletal meshes available
            if ( !vobInfo->VisualInfo ) continue;

            float dist;
            XMStoreFloat( &dist, XMVector3Length( vobInfo->Vob->GetPositionWorldXM() - cameraPosXm ) );
            if ( dist > drawRadius )
                continue; // Skip out of range

            GetSceneCamera()->SetTransform( zCCamera::ETransformType::TT_WORLD, *vobInfo->Vob->GetWorldMatrixPtr() );

            //Engine::GraphicsEngine->GetLineRenderer()->AddAABBMinMax(bb.Min, bb.Max, XMFLOAT4(1, 1, 1, 1));

            int clipFlags = EGothicCullFlags::CullSidesNear; // No far clip
            if ( GetCameraBBox3DInFrustum( vobInfo->Vob, clipFlags, true ) == ZTCAM_CLIPTYPE_OUT )
                continue;

            // Indoor?
            vobInfo->IndoorVob = vobInfo->Vob->IsIndoorVob();

            zCModel* model = static_cast<zCModel*>(vobInfo->Vob->GetVisual());
            if ( !model )
                continue; // Gothic fortunately sets this to 0 when it throws the model out of the cache

            // This is important, because gothic only lerps between animation when this distance is set and below ~2000
            model->SetDistanceToCamera( dist );

            // Schedule for drawing in later stage if this vob is ghost
            if ( vobInfo->Vob->GetVisualAlpha() ) {
                TransparencyVobs.emplace_back( dist, vobInfo->Vob->GetVobTransparency(), vobInfo, nullptr );
                continue;
            }

            if (dist < 1000 && GetShouldRenderAsMorphMesh(vobInfo, model ) ) {
                drawAsMorphMesh.push_back( vobInfo );
            } else {
                drawRegular.push_back( vobInfo );
            }

            if( RendererState.RendererSettings.ShowSkeletalVertexNormals )
                VNSkeletalVobs.emplace_back( vobInfo );
        }
        D3D11GraphicsEngine* g = AsD3D11Engine(Engine::GraphicsEngine);

        if (!drawAsMorphMesh.empty()) {
            auto _ = Engine::GraphicsEngine->RecordGraphicsEvent( GE_NAME( "Draw Skeletal Morph Meshes" ) ); 
            // force drawing as morph Mesh for those, by setting distance very close.
            g->DrawSkeletalMeshVobs( drawAsMorphMesh, 500, true, true );
            drawAsMorphMesh.clear();
        }
        if (!drawRegular.empty()) {
            auto _ = Engine::GraphicsEngine->RecordGraphicsEvent( GE_NAME( "Draw Skeletal Meshes" ) );
            g->DrawSkeletalMeshVobs( drawRegular, FLT_MAX, true, true );
            drawRegular.clear();
        }
    }

    // Draw vobs in view
    Engine::GraphicsEngine->DrawVOBs();

    //DebugDrawBSPTree();

    ResetWorldTransform();
}

/** Prepares this frame's particle data and draws the particle prog-meshes. The concrete engine
    draws the collected FrameParticles into its own refraction targets after this returns. */
void GothicAPI::DrawParticlesSimple() {
    ZoneScopedN( "GothicAPI::DrawParticlesSimple" );
    ParticleFrameData data;

    if ( RendererState.RendererSettings.DrawParticleEffects ) {
        std::vector<zCVob*> renderedParticleFXs;
        GetVisibleParticleEffectsList( renderedParticleFXs );

        // now it is save to render
        for ( auto const& it : renderedParticleFXs ) {
            const zCVisual* vis = it->GetVisual();
            if ( vis ) {
                DrawParticleFX( it, reinterpret_cast<zCParticleFX*>(const_cast<zCVisual*>(vis)), data );
            }
        }

        Engine::GraphicsEngine->DrawFrameParticleMeshes( ParticleEffectProgMeshes );
    }
}

// Converts poly strip visuals to render ready geometry
void GothicAPI::CalcPolyStripMeshes() {
    ZoneScopedN( "GothicAPI::CalcPolyStripMeshes" );
    ExVertexStruct polyFan[4];
    PolyStripInfos.clear();

    for ( const auto& pStrip : PolyStripVisuals ) {
        if ( !pStrip ) continue;

        //Pointer passed is a placeholder, it'll not be used inside the function.
        //We need gothic engine to only execute relevant calculations inside native Render()
        //without actually rendering polygons. Inside Render() polygons are rendered
        //with zCRnd_D3D::DrawPoly(). Hook created inside zCRndD3D.h prevents native rendering.
        pStrip->Render( pStrip );
        //////////////////////////////

        zCPolyStripInstance* pStripInst = pStrip->GetInstanceData();
        zCMaterial* mat = pStripInst->material;
        zCTexture* tx = mat->GetAniTexture();
        if ( !tx ) {
            tx = mat->GetTextureSingle();
        }
        if ( !tx ) {
            // Whoops, why does this have no texture?
            // TODO: PolyStrips Why is this sometimes null?
            continue;
        }
        //These values go back to 0 after reaching maxSegAmount
        int firstSeg = pStripInst->firstSeg;
        int lastSeg = pStripInst->lastSeg;
        int maxSegAmount = pStripInst->numVert / 2;

        float* alphaList = pStripInst->alphaList;
        zCVertex* vertList = pStripInst->vertList;
        zCPolygon* poly = &(pStripInst->polyList[0]);

        //order of vertex indeces that make up a single poly
        int vertOrder[4] = { 0, 1, 3, 2 };

        //Loop though segment while allowing segment index to overflow maxSegAmount
        for ( int i = firstSeg; ; i++ ) {
            int segIndex = i % maxSegAmount;

            if ( segIndex == lastSeg ) {
                //Triangles for the last segment are created during previous iteration, so break here.
                break;
            }

#ifdef BUILD_GOTHIC_1_08k
            //For G1 vertices are taken from polygons in polyList
            poly = &pStripInst->polyList[segIndex];
            zCVertex** polyVertices = poly->getVertices();

            for ( int n = 0; n < 4; n++ ) {
                ExVertexStruct& vert = polyFan[n];
                vert.Position = polyVertices[n]->Position;
                vert.TexCoord = poly->getFeatures()[n]->texCoord;
                vert.Normal = poly->getFeatures()[n]->normal;
                vert.Color = poly->getFeatures()[n]->lightStatic;
            }
#endif
#ifdef BUILD_GOTHIC_2_6_fix
            //For G2 polyList only contains a single polygon (supposed to be kind of a reference it seems)
            //and vertices should be taken from vertList, while preserving a correct order making up a
            //properly winded polygon
            uint8_t maxSegAlpha = 0;
            for ( int n = 0; n < 4; n++ ) {
                //In similar fashion to segment index - vertex index should overflow numVert.
                int vInd = ((segIndex << 1) + vertOrder[n]) % pStripInst->numVert;
                //Segment index of the current vertex (it's not always equals `i` since we loop through next segment's vertices as well).
                int vSegInd = (((segIndex << 1) + vertOrder[n]) >> 1) % maxSegAmount;

                ExVertexStruct& vert = polyFan[n];
                vert.Position = vertList[vInd].Position;
                //Vertex features are hooked up from reference polygon's vertices
                vert.TexCoord = poly->getFeatures()[n]->texCoord;
                vert.Normal = poly->getFeatures()[n]->normal;
                vert.Color = poly->getFeatures()[n]->lightStatic;

                float alpha = alphaList[vSegInd];
                if ( alpha < 0.f ) alpha = 0.f;
                reinterpret_cast<uint8_t*>(&vert.Color)[3] = alpha;
                maxSegAlpha = std::max<uint8_t>( maxSegAlpha, reinterpret_cast<uint8_t*>(&vert.Color)[3] );
            }

            // Both blend modes scale by SRC_ALPHA, so a fully faded-out segment is an invisible quad.
            if ( maxSegAlpha == 0 ) continue;
#endif

            //Convert list of quads to list of triangles
            PolyStripInfos[tx].vertices.reserve( 4 * 3 );
            WorldConverter::TriangleFanToList( &polyFan[0], 4, &PolyStripInfos[tx].vertices );
            PolyStripInfos[tx].material = mat;
        }
    }
};

void GothicAPI::CalcFlashMeshes() {
    ZoneScopedN( "GothicAPI::CalcFlashMeshes" );
    if ( !RendererState.RendererSettings.DrawParticleEffects || (FlashVisuals.empty() && FrameThunderPolyStrips.empty()) ) {
        // Only consumer of the list, so drain it even when we draw nothing - otherwise it grows for as
        // long as the barrier keeps pushing bolts.
        FrameThunderPolyStrips.clear();
        return;
    }
    
    auto vVfxRangeSq = XMVectorReplicate(RendererState.RendererSettings.VisualFXDrawRadius * RendererState.RendererSettings.VisualFXDrawRadius);

    FXMVECTOR camPos = GetCameraPositionXM();
    static std::vector<zCPolyStrip*> polyStrips; polyStrips.clear();
    for ( auto it = FlashVisuals.begin(); it != FlashVisuals.end();) {
        zCFlash* flash = it->first;
        if ( XMVector3Greater(XMVector3LengthSq( flash->GetStartPositionWorld() - camPos ), vVfxRangeSq) &&
            XMVector3Greater(XMVector3LengthSq( flash->GetEndPositionWorld() - camPos ), vVfxRangeSq) ) {
            // Out of range this frame, but keep it alive. Must advance - the loop only steps at its tail.
            ++it;
            continue;
        }

        if ( flash->RenderFlash( polyStrips ) ) {
            zCVob* connectedVob = it->second;
            it = FlashVisuals.erase( it );
            if ( connectedVob ) {
                connectedVob->GetHomeWorld()->RemoveVob( connectedVob );
            }
            continue;
        }
        ++it;
    }

    if ( !FrameThunderPolyStrips.empty() ) {
        polyStrips.insert( polyStrips.end(), FrameThunderPolyStrips.begin(), FrameThunderPolyStrips.end() );
        FrameThunderPolyStrips.clear();
    }

    ExVertexStruct polyFan[4];
    for ( const auto& pStrip : polyStrips ) {
        //Pointer passed is a placeholder, it'll not be used inside the function.
        //We need gothic engine to only execute relevant calculations inside native Render()
        //without actually rendering polygons. Inside Render() polygons are rendered
        //with zCRnd_D3D::DrawPoly(). Hook created inside zCRndD3D.h prevents native rendering.
        pStrip->Render( pStrip );

        zCPolyStripInstance* pStripInst = pStrip->GetInstanceData();
        zCMaterial* mat = pStripInst->material;
        zCTexture* tx = mat->GetAniTexture();
        if ( !tx ) {
            tx = mat->GetTextureSingle();
        }
        if ( !tx ) {
            continue;
        }

        //These values go back to 0 after reaching maxSegAmount
        int firstSeg = pStripInst->firstSeg;
        int lastSeg = pStripInst->lastSeg;
        int maxSegAmount = pStripInst->numVert / 2;

        float* alphaList = pStripInst->alphaList;
        zCVertex* vertList = pStripInst->vertList;
        zCPolygon* poly = &(pStripInst->polyList[0]);

        //order of vertex indeces that make up a single poly
        int vertOrder[4] = { 0, 1, 3, 2 };

        //Loop though segment while allowing segment index to overflow maxSegAmount
        for ( int i = firstSeg; ; i++ ) {
            int segIndex = i % maxSegAmount;

            if ( segIndex == lastSeg ) {
                //Triangles for the last segment are created during previous iteration, so break here.
                break;
            }

#ifdef BUILD_GOTHIC_1_08k
            //For G1 vertices are taken from polygons in polyList
            poly = &pStripInst->polyList[segIndex];
            zCVertex** polyVertices = poly->getVertices();

            for ( int n = 0; n < 4; n++ ) {
                ExVertexStruct& vert = polyFan[n];
                vert.Position = polyVertices[n]->Position;
                vert.TexCoord = poly->getFeatures()[n]->texCoord;
                vert.Normal = poly->getFeatures()[n]->normal;
                vert.Color = poly->getFeatures()[n]->lightStatic;
            }
#endif
#ifdef BUILD_GOTHIC_2_6_fix
            //For G2 polyList only contains a single polygon (supposed to be kind of a reference it seems)
            //and vertices should be taken from vertList, while preserving a correct order making up a
            //properly winded polygon
            uint8_t maxSegAlpha = 0;
            for ( int n = 0; n < 4; n++ ) {
                //In similar fashion to segment index - vertex index should overflow numVert.
                int vInd = ((segIndex << 1) + vertOrder[n]) % pStripInst->numVert;
                //Segment index of the current vertex (it's not always equals `i` since we loop through next segment's vertices as well).
                int vSegInd = (((segIndex << 1) + vertOrder[n]) >> 1) % maxSegAmount;

                ExVertexStruct& vert = polyFan[n];
                vert.Position = vertList[vInd].Position;
                //Vertex features are hooked up from reference polygon's vertices
                vert.TexCoord = poly->getFeatures()[n]->texCoord;
                vert.Normal = poly->getFeatures()[n]->normal;
                vert.Color = poly->getFeatures()[n]->lightStatic;

                float alpha = alphaList[vSegInd];
                if ( alpha < 0.f ) alpha = 0.f;
                reinterpret_cast<uint8_t*>( &vert.Color )[3] = alpha;
                maxSegAlpha = std::max<uint8_t>( maxSegAlpha, reinterpret_cast<uint8_t*>( &vert.Color )[3] );
            }

            // Both blend modes scale by SRC_ALPHA, so a fully faded-out segment is an invisible quad -
            // and the barrier's sky-wide bolts spend most of their life fading.
            if ( maxSegAlpha == 0 ) continue;
#endif

            //Convert list of quads to list of triangles
            PolyStripInfos[tx].vertices.reserve( 4 * 3 );
            WorldConverter::TriangleFanToList( &polyFan[0], 4, &PolyStripInfos[tx].vertices );
            PolyStripInfos[tx].material = mat;
        }
    }
}

/** Returns a list of visible particle-effects */
void GothicAPI::GetVisibleParticleEffectsList( std::vector<zCVob*>& pfxList ) {
    ZoneScopedN( "GothicAPI::GetVisibleParticleEffectsList" );
    if ( RendererState.RendererSettings.DrawParticleEffects ) {
        FXMVECTOR camPos = GetCameraPositionXM();

        auto sceneCam = reinterpret_cast<zCCamera*>(oCGame::GetGame()->_zCSession_camera);
        if ( !sceneCam ) {
            // No camera??
            return;
        }

        const XMVECTOR vVfxRangeSq = XMVectorReplicate( RendererState.RendererSettings.VisualFXDrawRadius * RendererState.RendererSettings.VisualFXDrawRadius );

        for ( auto const& it : ParticleEffectVobs ) {
            if ( XMVector3Greater( XMVector3LengthSq( it->GetPositionWorldXM() - camPos ), vVfxRangeSq ) ) {
                // too far? It's ok for particles to not update and restart.
                continue;
            }

            INT clipFlags = EGothicCullFlags::CullSides;
            if ( sceneCam->BBox3DInFrustum( it->GetBBox(), clipFlags ) == ZTCAM_CLIPTYPE_OUT ) {
                if ( auto vis = it->GetVisual() ) {
                    // Do update particle state, even if not in frustum, so that if player turns back to it, it doesn't restart.
                    auto particleFx = reinterpret_cast<zCParticleFX*>(vis);
                    RepairShapeMeshEmitter( it, particleFx );
                    if ( IsUnservableSkeletalShapeEmitter( particleFx ) ) {
                        // Its shape-mesh isn't skinned yet - ticking it would spawn every particle
                        // on the model's origin. Let it start a few frames late instead.
                        continue;
                    }
                    particleFx->UpdateParticleFX();
                    if ( !particleFx->GetVisualDied() ) {
                        zCParticleFX::GetStaticPFXList()->TouchPfx( particleFx );
                    }
                }
                continue;
            }

            if ( it->GetVisual() && it->GetShowVisual() ) {
                pfxList.push_back( it );
            }
        }
    }
}

static bool DecalSortcmpFunc( const std::pair<zCVob*, float>& a, const std::pair<zCVob*, float>& b ) {
    return a.second > b.second; // Back to front
}

/** Gets a list of visible decals */
void GothicAPI::GetVisibleDecalList( std::vector<zCVob*>& decals ) {
    ZoneScopedN( "GothicAPI::GetVisibleDecalList" );
    FXMVECTOR camPos = GetCameraPositionXM();
    static std::vector<std::pair<zCVob*, float>> decalDistances; // Static to get around reallocations

    float vVfxRangeSq = RendererState.RendererSettings.VisualFXDrawRadius * RendererState.RendererSettings.VisualFXDrawRadius;
    float dist;
    for ( auto const& it : DecalVobs ) {
        XMStoreFloat( &dist, XMVector3LengthSq( it->GetPositionWorldXM() - camPos ) );
        if ( dist > vVfxRangeSq )
            continue;

        if ( GetCameraBBox3DInFrustum( it->GetBBox(), EGothicCullFlags::CullSidesNear ) == ZTCAM_CLIPTYPE_OUT ) {
            continue;
        }

        if ( it->GetVisual() && it->GetShowVisual() ) {
            decalDistances.push_back( std::make_pair( it, dist ) );
        }
    }

    // Sort back to front
    std::ranges::sort(decalDistances, DecalSortcmpFunc );

    // Put into output list
    decals.reserve(decalDistances.size());
    for ( auto const& it : decalDistances ) {
        decals.push_back( it.first );
    }

    decalDistances.clear();
}

/** Called when a material got removed */
void GothicAPI::OnMaterialDeleted( zCMaterial* mat ) {
    LoadedMaterials.erase( mat );
    {
        std::unique_lock lock( MaterialInfosMutex );
        MaterialInfos.erase( mat );
    }
    if ( !mat )
        return;
    for ( auto&& it : SkeletalMeshVisuals ) {
        // Skip entries a background LoadzCModelData(...) job is still filling in - mutating
        // Meshes/SkeletalMeshes here would race with the worker thread writing to the same maps.
        if ( !it.second->GetIsReady() ) continue;
        it.second->Meshes.erase(mat);
        it.second->SkeletalMeshes.erase(mat);
    }
    for ( auto&& it : SkeletalMeshNpcs ) {
        if ( !it.second->GetIsReady() ) continue;
        it.second->Meshes.erase(mat);
        it.second->SkeletalMeshes.erase(mat);
    }
}

/** Called when a material got created */
void GothicAPI::OnMaterialCreated( zCMaterial* mat ) {
    LoadedMaterials.insert( mat );
}

/** Returns if the material is currently active */
bool GothicAPI::IsMaterialActive( zCMaterial* mat ) const {
    return LoadedMaterials.contains(mat);
}

/** Called when a vob moved */
void GothicAPI::OnVobMoved( zCVob* vob ) {
    static auto checkMatrix = []( FXMMATRIX a, CXMMATRIX b ) -> bool {
        const uint32_t mask = _mm_movemask_epi8( _mm_packs_epi16(
            _mm_packs_epi32 (
            _mm_castps_si128( _mm_cmpeq_ps( a.r[0], b.r[0] ) ),
            _mm_castps_si128( _mm_cmpeq_ps( a.r[1], b.r[1] ) ) ),
            _mm_packs_epi32 (
            _mm_castps_si128( _mm_cmpeq_ps( a.r[2], b.r[2] ) ),
            _mm_castps_si128( _mm_cmpeq_ps( a.r[3], b.r[3] ) ) )
        ) );
        return (mask == 0xFFFF);
    };

    auto it = VobMap.find( vob );
    if ( it != VobMap.end() ) {
        VobInfo* vi = it->second;
        if ( checkMatrix( vob->GetWorldMatrixXM(), XMLoadFloat4x4( &vi->WorldMatrix ) ) ) {
            // No actual change
            return;
        }

        if ( !vi->ParentBSPNodes.empty() ) {
            // Move vob into the dynamic list, if not already done
            MoveVobFromBspToDynamic( vi );
        }

        vi->UpdateState();
        Engine::GAPI->GetRendererState().RendererInfo.FrameVobUpdates++;
    } else {
        auto sit = SkeletalVobMap.find( vob );
        if ( sit != SkeletalVobMap.end() ) {
            SkeletalVobInfo* vi = sit->second;
            if ( vi->ParentBSPNodes.empty() || checkMatrix( vob->GetWorldMatrixXM(), XMLoadFloat4x4( &vi->WorldMatrix ) ) ) {
                // No actual change
                return;
            }
            // This is a mob, remove it from the bsp-cache and add to dynamic list
            MoveVobFromBspToDynamic( vi );
            vi->UpdateState();
        }
    }
}

/** Called when a visual got removed */
void GothicAPI::OnVisualDeleted( zCVisual* visual ) {
    // Gothic frees this visual once we return - make sure no background node-attachment extraction
    // (WorldConverter::ExtractNodeVisualAsync) is still reading from it.
    WorldConverter::WaitForPendingNodeVisuals( visual );

    // Retire it as a shared-attachment key - the address can be recycled for an unrelated visual.
    // Attachments still holding the entry keep it alive until their "visual changed" check retires them.
    s_SharedVisualRegistry->Unregister( visual );

    std::vector<std::string> extv;

    zCClassDef* classDef = reinterpret_cast<zCObject*>(visual)->_GetClassDef();
    const char* className = classDef->className.ToChar();

    // Get the visuals possible file extensions
    int e = 0;
    while ( strlen( visual->GetFileExtension( e ) ) > 0 ) {
        extv.push_back( visual->GetFileExtension( e ) );
        e++;
    }

    // This is a poly strip vob
    if ( strcmp( className, "zCPolyStrip" ) == 0 ) {
        PolyStripVisuals.erase( reinterpret_cast<zCPolyStrip*>(visual) );
    }

    // Check every extension
    for (auto& ext : extv) {
        // Delete according to the type
        if ( ext == ".3DS" ) {
            // Clear the visual from all vobs (TODO: This may be slow!)
            for ( auto it = VobMap.begin(); it != VobMap.end();) {
                if ( !it->second->VisualInfo ) { // This happens sometimes, so get rid of it
                    delete it->second;
                    it = VobMap.erase( it );
                    continue;
                }

                if ( it->second->VisualInfo->Visual == static_cast<zCProgMeshProto*>(visual) ) {
                    it->second->VisualInfo = nullptr;
                }
                ++it;
            }

            delete StaticMeshVisuals[static_cast<zCProgMeshProto*>(visual)];
            StaticMeshVisuals.erase( static_cast<zCProgMeshProto*>(visual) );
            break;
        } else if ( ext == ".MDS" || ext == ".ASC" ) {
            // We can load some MDS/ASC models as inventory objects
            zCProgMeshProto* pm = static_cast<zCProgMeshProto*>(visual);
            auto vit = StaticMeshVisuals.find( pm );
            if ( vit != StaticMeshVisuals.end() ) {
                // Clear the visual from all vobs (TODO: This may be slow!)
                for ( auto it = VobMap.begin(); it != VobMap.end();) {
                    if ( !it->second->VisualInfo ) { // This happens sometimes, so get rid of it
                        delete it->second;
                        it = VobMap.erase( it );
                        continue;
                    }

                    if ( it->second->VisualInfo->Visual == pm ) {
                        it->second->VisualInfo = nullptr;
                    }
                    ++it;
                }

                delete StaticMeshVisuals[pm];
                StaticMeshVisuals.erase( pm );
            }

            zCModel* zmodel = static_cast<zCModel*>(visual);
            if ( zmodel->GetMainPrototypeReferences() <= 1 ) { // Check if it is the last reference in prototype so that we can delete this visual
                auto visName = zmodel->GetVisualName();
                std::string str( visName.data(), visName.size() );
                if ( str.empty() ) { // Happens when the model has no skeletal-mesh
                    str.append( zmodel->GetModelName() );
                }

                auto it = SkeletalMeshVisuals.find( str );
                // Only tear the entry down if it belongs to *this* model: an extraction reference can hold a
                // model's destructor past a reload that re-bound this name to a newer one. A null Visual
                // means the extraction never completed, so the entry is nobody else's.
                if ( it != SkeletalMeshVisuals.end() && (!it->second->Visual || it->second->Visual == zmodel) ) {
                    // Find vobs using this visual
                    for ( SkeletalVobInfo* vobInfo : SkeletalMeshVobs ) {
                        if ( vobInfo->VisualInfo == it->second ) {
                            vobInfo->VisualInfo = nullptr;
                        }
                    }

                    // Make sure no background extraction is still writing into the info we delete.
                    s_AsyncVisualExtractor->WaitForSkeletal( it->second );

                    delete SkeletalMeshVisuals[str];
                    SkeletalMeshVisuals.erase( str );
                }
            }

            zCVob* homeVob = zmodel->GetHomeVob();
            if ( homeVob && homeVob->GetVobType() == zVOB_TYPE_NSC ) {
                oCNPC* npc = static_cast<oCNPC*>(homeVob);
                auto it = SkeletalMeshNpcs.find( npc );
                // Same identity check as above; the armor change is exactly what it is for, since the
                // NPC's entry may already have been rebuilt from its new zCModel by the time this
                // (deferred) destructor gets to run.
                if ( it != SkeletalMeshNpcs.end() && (!it->second->Visual || it->second->Visual == zmodel) ) {
                    // Find vobs using this visual
                    for ( SkeletalVobInfo* vobInfo : SkeletalMeshVobs ) {
                        if ( vobInfo->VisualInfo == it->second ) {
                            vobInfo->VisualInfo = nullptr;
                        }
                    }

                    // Make sure no background extraction is still writing into the info we delete.
                    s_AsyncVisualExtractor->WaitForSkeletal( it->second );

                    delete SkeletalMeshNpcs[npc];
                    SkeletalMeshNpcs.erase( npc );
                }
            }
            break;
        }
    }

    // Clear
    auto& list = VobsByVisual[visual];
    if ( _canClearVobsByVisual ) {
        for ( auto const& it : list ) {
            OnRemovedVob( it->Vob, LoadedWorldInfo->MainWorld );
        }
    } else {
        // TODO: #8 - Figure out why exactly we don't get notified that a VOB is re-added after being removed.
        /*oCNPC* npcVob;
        for (auto const& it : list) {
            if (npcVob = it->Vob->AsNpc()) {
                LogInfo() << "Not removing NPC Vob: " << npcVob->GetName().ToChar();
            }
            else {
                OnRemovedVob(it->Vob, LoadedWorldInfo->MainWorld);
            }
        }*/
    }
    if ( list.size() > 0 ) {
#ifndef PUBLIC_RELEASE
        if ( RendererState.RendererSettings.EnableDebugLog )
            LogInfo() << className << " had " << list.size() << " vobs";
#endif

        VobsByVisual[visual].clear();
        VobsByVisual.erase( visual );
    }
}
/** Draws a MeshInfo */
void GothicAPI::DrawMeshInfo( zCMaterial* mat, MeshInfo* msh ) {
    // Check for material and bind the texture if it exists
    if ( mat ) {
        // Setup alphatest //TODO: This has to be done earlier!
        if ( mat->GetAlphaFunc() == zRND_ALPHA_FUNC_TEST )
            RendererState.GraphicsState.FF_GSwitches |= GSWITCH_ALPHAREF;
        else
            RendererState.GraphicsState.FF_GSwitches &= ~GSWITCH_ALPHAREF;
    }

    if ( !msh->MeshIndexBuffer ) {
        Engine::GraphicsEngine->DrawVertexBuffer( msh->GetMeshVertexBuffer(), msh->Vertices.size() );
    } else {
        Engine::GraphicsEngine->DrawVertexBufferIndexed( msh->GetMeshVertexBuffer(), msh->GetMeshIndexBuffer(), msh->Indices.size() );
    }
}

void GothicAPI::DrawMeshInfo_Layered( zCMaterial* mat, MeshInfo* msh ) {
    // Check for material and bind the texture if it exists
    if ( mat ) {
        // Setup alphatest //TODO: This has to be done earlier!
        if ( mat->GetAlphaFunc() == zRND_ALPHA_FUNC_TEST )
            RendererState.GraphicsState.FF_GSwitches |= GSWITCH_ALPHAREF;
        else
            RendererState.GraphicsState.FF_GSwitches &= ~GSWITCH_ALPHAREF;
    }

    D3D11GraphicsEngine* g = AsD3D11Engine(Engine::GraphicsEngine);
    if ( !msh->MeshIndexBuffer ) {
        g->DrawVertexBufferInstanced( msh->GetMeshVertexBuffer(), msh->Vertices.size(), 6 );
    } else {
        g->DrawVertexBufferInstancedIndexed( msh->GetMeshVertexBuffer(), msh->GetMeshIndexBuffer(), msh->Indices.size(), 6 );
    }
}

/** Locks the resource CriticalSection */
void GothicAPI::EnterResourceCriticalSection() {

    EnterCriticalSection( &ResourceCriticalSection );
}

/** Unlocks the resource CriticalSection */
void GothicAPI::LeaveResourceCriticalSection() {

    LeaveCriticalSection( &ResourceCriticalSection );
}

/** Swap-and-pop removal of a vob from one BSP leaf list. Order in these lists is irrelevant -
    collection walks them whole - so the swap keeps removal O(list size) instead of the shuffle
    an erase() would do, and every list stays contiguous for the SIMD distance reject. */
static void EraseVobFromLeafList( std::vector<LeafVobEntry>& list, const VobInfo* vob ) {
    for ( auto it = list.begin(); it != list.end(); ++it ) {
        if ( it->Info == vob ) {
            *it = list.back();
            list.pop_back();
            return;
        }
    }
}

/** Called when a VOB got removed from the world */
void GothicAPI::OnRemovedVob( zCVob* vob, zCWorld* world, bool tearDownLight ) {
    //LogInfo() << "Removing vob: " << vob;

    // Symmetric to the OnAddVob side: an inventory preview vob never entered the engine's world-scoped state, so
    // it must not tear it down either. Skipping this matters, not just for symmetry - ZenGin adds and removes the
    // item once per slot per frame at world position (0,0,0), and the notification invalidates the cached static
    // shadow cube of every point light reaching that position, forcing a re-render every frame the container is
    // open. A null world means we can't tell, so keep the old behavior there.
    if ( !world || world == LoadedWorldInfo->MainWorld )
        Engine::GraphicsEngine->OnVobRemovedFromWorld( vob );

    auto it = RegisteredVobs.find( vob );
    if ( it == RegisteredVobs.end() ) {
        // Not registered
        return;
    }

    RegisteredVobs.erase( it );

    zCVisual* visual = vob->GetVisual();
    if ( visual ) {
        zCClassDef* classDef = reinterpret_cast<zCObject*>(visual)->_GetClassDef();
        const char* className = classDef->className.ToChar();
        if ( strcmp( className, "zCPolyStrip" ) == 0 ) {
            PolyStripVisuals.erase( reinterpret_cast<zCPolyStrip*>(visual) ); //remove it if it exists in polystrips array
        }
    }

    // Erase the vob from visual-vob map
    auto& vec = VobsByVisual[vob->GetVisual()];
    for ( size_t i = 0; i < vec.size(); ++i ) {
        if ( vec[i]->Vob == vob ) {
            // Overwrite the deleted item with the last item, then shrink by 1
            vec[i] = vec.back();
            vec.pop_back();
            break; // Can (should!) only be in here once
        }
    }

    // TODO: This is sometimes NULL
    if ( world ) {
        // Check if this was in some inventory
        if ( Inventory->OnRemovedVob( vob, world ) )
            return; // Don't search in the other lists since it wont be in it anyways

        if ( world != LoadedWorldInfo->MainWorld )
            return; // *should* be already deleted from the inventory here. But watch out for dem leaks, dragons be here!
    }

    VobInfo* vi = VobMap[vob];
    SkeletalVobInfo* svi = SkeletalVobMap[vob];

    // Tell all dynamic lights that we removed a vob they could have cached. This is about other
    // lights' shadow-caster caches referencing vi/svi, not about `vob` itself being a light, so it
    // always applies regardless of tearDownLight.
    for ( auto& vlit : VobLightMap ) {
        if ( vi && vlit.second->LightShadowBuffers )
            vlit.second->LightShadowBuffers->OnVobRemovedFromWorld( vi );

        if ( svi && vlit.second->LightShadowBuffers )
            vlit.second->LightShadowBuffers->OnVobRemovedFromWorld( svi );
    }

    // A disabled zCVobLight keeps its slot in the BSP leaf's LightVobList (that array mirrors the
    // world's static light layout, not enabled state) and can be re-enabled with the same zCVob*.
    // Tearing its VobLightInfo down here would delete state that CollectLeafVobs' mirror
    // (base->Lights) and VobLightMap still expect to find, and that the vob's own IsEnabled() check
    // is what's supposed to filter out of rendering - not us deleting and recreating it every toggle.
    VobLightInfo* li = tearDownLight ? VobLightMap[static_cast<zCVobLight*>(vob)] : nullptr;

    // Erase it from the particle-effect list
    auto pit = std::ranges::find(ParticleEffectVobs, vob );
    if ( pit != ParticleEffectVobs.end() ) {
        DestroyParticleEffect( *pit );
        *pit = ParticleEffectVobs.back();
        ParticleEffectVobs.pop_back();
    }
    auto dit = std::ranges::find(DecalVobs, vob );
    if ( dit != DecalVobs.end() ) {
        *dit = DecalVobs.back();
        DecalVobs.pop_back();
    }

    // Erase it from the list of lights - only on a real removal (see tearDownLight comment above).
    if ( tearDownLight ) {
        VobLightMap.erase( static_cast<zCVobLight*>(vob) );
    }

    // Remove from BSP-Cache
    std::vector<BspInfo*>* nodes = nullptr;
    if ( vi )
        nodes = &vi->ParentBSPNodes;
    else if ( li )
        nodes = &li->ParentBSPNodes;
    else if ( svi )
        nodes = &svi->ParentBSPNodes;

    if ( nodes ) {
        for ( unsigned int i = 0; i < nodes->size(); i++ ) {
            BspInfo* node = (*nodes)[i];
            if ( vi ) {
                EraseVobFromLeafList( node->IndoorVobs, vi );
                EraseVobFromLeafList( node->Vobs, vi );
                EraseVobFromLeafList( node->SmallVobs, vi );
            }

            if ( li && nodes ) {
                for ( auto bit = node->Lights.begin(); bit != node->Lights.end(); ++bit ) {
                    if ( (*bit)->Vob == static_cast<zCVobLight*>(vob) ) {
                        (*bit) = node->Lights.back();
                        node->Lights.pop_back();
                        break;
                    }
                }

                for ( auto bit = node->IndoorLights.begin(); bit != node->IndoorLights.end(); ++bit ) {
                    if ( (*bit)->Vob == static_cast<zCVobLight*>(vob) ) {
                        (*bit) = node->IndoorLights.back();
                        node->IndoorLights.pop_back();
                        break;
                    }
                }
            }

            if ( svi && nodes ) {
                for ( auto bit = node->Mobs.begin(); bit != node->Mobs.end(); ++bit ) {
                    if ( (*bit)->Vob == vob ) {
                        (*bit) = node->Mobs.back();
                        node->Mobs.pop_back();
                        break;
                    }
                }
            }
        }
    }

    // Erase the vob from the section
    if ( vi && vi->VobSection ) {
        vi->VobSection->Vobs.remove( vi );
    }
    // Erase it from the skeletal vob-list
    for ( size_t i = 0; i< SkeletalMeshVobs.size(); ++i ) {
        if ( SkeletalMeshVobs[i]->Vob == vob ) {
            SkeletalMeshVobs[i] = SkeletalMeshVobs.back();
            SkeletalMeshVobs.pop_back();
            break;
        }
    }

    for ( size_t i = 0; i< AnimatedSkeletalVobs.size(); ++i ) {
        if ( AnimatedSkeletalVobs[i]->Vob == vob ) {
            AnimatedSkeletalVobs[i] = AnimatedSkeletalVobs.back();
            AnimatedSkeletalVobs.pop_back();
            break;
        }
    }

    for ( size_t i = 0; i< DynamicallyAddedVobs.size(); ++i ) {
        if ( DynamicallyAddedVobs[i]->Vob == vob ) {
            DynamicallyAddedVobs[i] = DynamicallyAddedVobs.back();
            DynamicallyAddedVobs.pop_back();
            break;
        }
    }

    for ( size_t i = 0; i< DynamicMeshVobs.size(); ++i ) {
        if ( DynamicMeshVobs[i]->Vob == vob ) {
            DynamicMeshVobs[i] = DynamicMeshVobs.back();
            DynamicMeshVobs.pop_back();
            break;
        }
    }

    // Erase it from vob-map
    auto vit = VobMap.find( vob );
    if ( vit != VobMap.end() ) {
        delete (*vit).second;
        VobMap.erase( vit );
    }
    auto svit = SkeletalVobMap.find( vob );
    if ( svit != SkeletalVobMap.end() ) {
        delete (*svit).second;
        SkeletalVobMap.erase( svit );
    }

    // delete light info, if valid
    if ( li ) delete li;
}

/** Called on a SetVisual-Call of a vob */
void GothicAPI::OnSetVisual( zCVob* vob ) {
    if ( !oCGame::GetGame() || !oCGame::GetGame()->_zCSession_world || !vob->GetHomeWorld() )
        return;

    // Add the vob to the set
    if ( RegisteredVobs.find( vob ) != RegisteredVobs.end() ) {
        for ( auto const& it : SkeletalMeshVobs ) {
            if ( it->VisualInfo && it->Vob == vob && it->VisualInfo->Visual == static_cast<zCModel*>(vob->GetVisual()) ) {
                return; // No change, skip this.
            }
        }
        // This one is already there. Re-Add it!
        OnRemovedVob( vob, vob->GetHomeWorld(), /*tearDownLight:*/ false);
    }

    OnAddVob( vob, vob->GetHomeWorld() );
}

/** Called when a VOB got added to the BSP-Tree */
void GothicAPI::OnAddVob( zCVob* vob, zCWorld* world ) {
    if ( !vob->GetVisual() ) return; // Don't need it if we can't render it
#ifdef BUILD_SPACER
    if ( strncmp( vob->GetVisual()->GetObjectName(), "INVISIBLE_", strlen( "INVISIBLE_" ) ) == 0 )
        return;
#endif

    // Add the vob to the set
    if ( RegisteredVobs.find( vob ) != RegisteredVobs.end() ) {
        // Already got that
        return;
    }
    RegisteredVobs.insert( vob );

    zCClassDef* classDef = reinterpret_cast<zCObject*>(vob->GetVisual())->_GetClassDef();
    const char* className = classDef->className.ToChar();

    std::vector<std::string> extv;

    int e = 0;
    while ( strlen( vob->GetVisual()->GetFileExtension( e ) ) > 0 ) {
        extv.push_back( vob->GetVisual()->GetFileExtension( e ) );
        e++;
    }

    if ( !world )
        world = oCGame::GetGame()->_zCSession_world;

    if ( strcmp( className, "zCPolyStrip" ) == 0 ) {
        PolyStripVisuals.insert( reinterpret_cast<zCPolyStrip*>(vob->GetVisual()) );
    }

    for (auto ext : extv) {
        if ( ext == ".3DS" || ext == ".MMS" ) {
            zCProgMeshProto* pm;
            if ( ext == ".3DS" )
                pm = static_cast<zCProgMeshProto*>(vob->GetVisual());
            else
                pm = reinterpret_cast<zCMorphMesh*>(vob->GetVisual())->GetMorphMesh();

            if ( StaticMeshVisuals.count( pm ) == 0 ) {
                if ( pm->GetNumSubmeshes() == 0 )
                    return; // Empty mesh?

                // Load the new visual
                MeshVisualInfo* mi = new MeshVisualInfo;
                if ( ext == ".MMS" ) {
                    mi->MorphMeshVisual = reinterpret_cast<void*>(vob->GetVisual());
                    zCObject_AddRef( mi->MorphMeshVisual );
                }

                // Hand the expensive part (vertex unpacking + GPU buffer creation) to a worker thread -
                // this fires once per distinct mesh during world load, and blocking here for every one of
                // them is what makes loading (and mass-PFX-spawn) stutter. 'mi' is inserted below already
                // Ready==false; draw sites must skip it until the job flips that back to true.
                WorldConverter::Extract3DSMeshFromVisual2Async( vob->GetVisual(), pm, mi );
                StaticMeshVisuals[pm] = mi;
            }

            INT2 section = WorldConverter::GetSectionOfPos( vob->GetPositionWorld() );

            VobInfo* vi = new VobInfo;
            vi->Vob = vob;
            vi->VisualInfo = StaticMeshVisuals[pm];

            // Check for mainworld
            if ( world == oCGame::GetGame()->_zCSession_world ) {
                VobMap[vob] = vi;

                vi->VobSection = &WorldSections[section.x][section.y];
                vi->VobSection->Vobs.push_back( vi );
                vi->UpdateState(); 

                if ( !BspLeafVobLists.empty() ) { // Check if this is the initial loading
                    // It's not, chose this as a dynamically added vob
                    DynamicallyAddedVobs.push_back( vi );
                }
                // Add to map
                VobsByVisual[vob->GetVisual()].push_back( vi );

                // Non-static (StaticVob==false) registry for the D3D12 point-shadow dynamic overlay; see GetDynamicMeshVobs().
                if ( !vob->GetFlags().StaticVob ) DynamicMeshVobs.push_back( vi );

                // Inventory vobs are deliberately left out of this: they are added and removed once per slot per
                // frame while a container is open, and the engine-side notification is world-scoped work
                // (D3D12 invalidates every cached point-light static shadow cube reaching the vob, and grows a
                // permanent instancing bucket per visual). DrawVobSingle draws them without any of it.
                Engine::GraphicsEngine->OnAddVob( vi );
            } else {
                // Must be inventory
                Inventory->OnAddVob( vi, world );

                // Add to map
                VobsByVisual[vob->GetVisual()].push_back( vi );
            }

            break;
        } else if ( ext == ".MDS" || ext == ".ASC" ) {
            // Some mods use MDS/ASC models for inventory
            if ( world != oCGame::GetGame()->_zCSession_world ) {
                // Cast to zCProgMeshProto only to make it work with StaticMeshVisuals
                zCProgMeshProto* pm = static_cast<zCProgMeshProto*>(vob->GetVisual());

                if ( StaticMeshVisuals.count( pm ) == 0 ) {
                    // Load the new visual
                    MeshVisualInfo* mi = new MeshVisualInfo;
                    WorldConverter::ExtractProgMeshProtoFromModel( static_cast<zCModel*>(vob->GetVisual()), mi );
                    StaticMeshVisuals[pm] = mi;
                }

                VobInfo* vi = new VobInfo;
                vi->Vob = vob;
                vi->VisualInfo = StaticMeshVisuals[pm];

                // Add to map
                VobsByVisual[vob->GetVisual()].push_back( vi );

                // Must be inventory
                Inventory->OnAddVob( vi, world );
                break;
            }

            // Add vob to the skeletal list
            SkeletalVobInfo* vi = new SkeletalVobInfo;
            vi->Vob = vob;
            vi->VisualInfo = vob->GetVobType() == zVOB_TYPE_NSC ?
                LoadzCModelData( static_cast<oCNPC*>(vob) ) :
                LoadzCModelData( static_cast<zCModel*>(vob->GetVisual()) );

            // Add to map
            VobsByVisual[vob->GetVisual()].push_back( vi );

            // Save worldmatrix to see if this vob changed positions later
            XMStoreFloat4x4( &vi->WorldMatrix, vob->GetWorldMatrixXM() );

            // Check for mainworld
            if ( world == oCGame::GetGame()->_zCSession_world ) {
                SkeletalMeshVobs.push_back( vi );
                SkeletalVobMap[vob] = vi;

                // If this can be animated, put it into another map as well
                if ( !BspLeafVobLists.empty() ) // Check if this is the initial loading
                {
                    AnimatedSkeletalVobs.push_back( vi );
                }
            }
            break;
        } else if ( ext == ".PFX" ) {
            ParticleEffectVobs.push_back( vob );
            break;
        } else if ( ext == ".TGA" ) {
            DecalVobs.push_back( vob );
            break;
        }
    }
}

/** Loads the data out of a zCModel */
SkeletalMeshVisualInfo* GothicAPI::LoadzCModelData( zCModel* model ) {
    auto visName = model->GetVisualName();
    std::string str(visName.data(), visName.size());
    if ( str.empty() ) { // Happens when the model has no skeletal-mesh
        str.append( model->GetModelName() );
    }

    SkeletalMeshVisualInfo* mi = SkeletalMeshVisuals[str];
    if ( !mi ) {
        mi = new SkeletalMeshVisualInfo;
        SkeletalMeshVisuals[str] = mi;
    }

    // Retire whatever job was still attached to this info before reading Meshes below: it may be
    // extracting from an older zCModel, and it writes straight into mi.
    s_AsyncVisualExtractor->WaitForSkeletal( mi );

    if ( !mi->Meshes.empty() )
        return mi; // Already loaded

    s_AsyncVisualExtractor->ExtractSkeletal( model, mi );
    return mi;
}

SkeletalMeshVisualInfo* GothicAPI::LoadzCModelData( oCNPC* npc ) {
    zCModel* model = static_cast<zCModel*>(npc->GetVisual());

    SkeletalMeshVisualInfo* mi = SkeletalMeshNpcs[npc];
    if ( !mi ) {
        mi = new SkeletalMeshVisualInfo;
        SkeletalMeshNpcs[npc] = mi;
    }

    // can't cache the meshes and VisualName as it otherwise
    // won't properly fire for changing armors. for whatever reason...
    s_AsyncVisualExtractor->ExtractSkeletal( model, mi );
    return mi;
}

/** Looks up the extracted skeletal mesh data for a live zCModel. Returns nullptr while the data
 *  does not exist yet or a background LoadzCModelData extraction is still running. */
SkeletalMeshVisualInfo* GothicAPI::ResolveSkeletalVisualInfo( zCModel* model ) {
    SkeletalMeshVisualInfo* skeletalMesh = nullptr;

    zCVob* homeVob = model->GetHomeVob();
    if ( homeVob && homeVob->GetVobType() == zVOB_TYPE_NSC ) {
        oCNPC* npc = static_cast<oCNPC*>(homeVob);
        auto it = SkeletalMeshNpcs.find( npc );
        if ( it != SkeletalMeshNpcs.end() ) {
            skeletalMesh = it->second;
        }
    } else {
        auto visName = model->GetVisualName();
        std::string str( visName.data(), visName.size() );
        if ( str.empty() ) { // Happens when the model has no skeletal-mesh
            str.append( model->GetModelName() );
        }

        auto it = SkeletalMeshVisuals.find( str );
        if ( it != SkeletalMeshVisuals.end() ) {
            skeletalMesh = it->second;
        }
    }

    if ( !skeletalMesh || !skeletalMesh->GetIsReady() )
        return nullptr; // Still being built on a worker thread - don't touch its mesh data yet

    // A model can reach OnAddVob before Gothic has built its soft-skin list (freshly spawned NPCs
    // do). LoadzCModelData then caches an EMPTY extraction against this oCNPC*/visual and, since it
    // only re-runs from OnAddVob, that empty result survives until the vob is removed and re-added.
    // ZENGIN reads 0 polys from it and drops every particle on the model's origin - permanently.
    // Repair it here, the same way the D3D12 skeletal prepare does (D3D12Scene.cpp:3905). Safe to
    // do synchronously: Ready is true, so no background job is writing to this info, and it happens
    // once per model. Interactive MOBs whose only content is a node attachment legitimately stay
    // empty, hence the NumInArray guard.
    if ( skeletalMesh->SkeletalMeshes.empty() && model->GetMeshSoftSkinList()->NumInArray > 0 ) {
        WorldConverter::ExtractSkeletalMeshFromVob( model, skeletalMesh );
    }

    return skeletalMesh;
}

/** ZENGIN's oCVisualFX::CalcPFXMesh points a zPFX_EMITTER_SHAPE_MESH emitter at the origin vob's visual
 *  exactly once, while the FX's own visual is being created. If the origin had no visual at that instant the
 *  pointer stays null for the life of the effect, zCParticleEmitter::GetPosition falls through its MESH case
 *  to (0,0,0), and every particle spawns on the vob's pivot - the "fire beast burns at one point until
 *  re-spawned" bug. Re-run the assignment here once the visual does exist. */
void GothicAPI::RepairShapeMeshEmitter( zCVob* source, zCParticleFX* fx ) {
#ifndef BUILD_SPACER
    oCVisualFX* visFx = source ? source->As<oCVisualFX>() : nullptr;
    if ( !visFx )
        return;

    zCVob* origin = visFx->GetOrigin();
    if ( !origin )
        return;

    zCVisual* originVisual = origin->GetVisual();
    if ( !originVisual )
        return; // Still nothing to point at - try again next frame

    // Both repairs below mirror a `zDYNAMIC_CAST<zCModel>(origin->GetVisual())` test in ZENGIN.
    const char* ext = originVisual->GetFileExtension( 0 );
    if ( !ext || (strcmp( ext, ".MDS" ) != 0 && strcmp( ext, ".ASC" ) != 0) )
        return;

    zCModel* originModel = static_cast<zCModel*>(originVisual);

    // (a) The emitter's shape mesh (oCVisualFX::CalcPFXMesh). Governs WHERE particles spawn:
    //     without it the MESH case yields (0,0,0) and the whole effect sits on the pivot.
#ifndef BUILD_GOTHIC_1_08k
    if ( zCParticleEmitter* emitter = fx->GetEmitter();
        emitter && emitter->GetVisShpType() == 5
        && !emitter->GetVisShpModel() && !emitter->GetVisShpMesh() && !emitter->GetVisShpProgMesh()
        // emAdjustShpToOrigin is what makes oCVisualFX the *owner* of the shape mesh, so it is also
        // what guarantees ReleasePFXMesh drops the reference we add here. Without it we would leak.
        && visFx->GetAdjustShapeToOrigin() ) {

        emitter->SetVisShpModel( originModel );
        zCObject_AddRef( originModel ); // matches CalcPFXMesh's orgModel->AddRef()

        LogInfo() << "Repaired shape-mesh emitter for '" << originModel->GetModelName()
            << "' - oCVisualFX started before the origin had a visual";
    }
#endif

    // (b) The origin NODE binding (oCVisualFX::Init, oVisFx.cpp:2305-2309). Governs WHERE THE WHOLE
    //     EFFECT rides: with orgNode null, oCVisualFX::DoMovements' EM_TRJ_FIXED branch falls back
    //     from origin->GetTrafoModelNodeToWorld(orgNode) to origin->GetNewTrafoObjToWorld(), i.e.
    //     the vob pivot instead of the bone. This is what pinned the undead dragon's eye effects to
    //     its origin. Same one-shot-resolve trap as (a), so the same repair applies.
    if ( !visFx->GetOriginNode() ) {
        const zSTRING* nodeName = visFx->GetOriginNodeName();
        if ( nodeName && nodeName->Length() > 0 ) {
            // ZENGIN uppercases emTrjOriginNode_S before the lookup; node names are upper case and
            // so are the script instances in practice, so a mixed-case name simply fails to resolve
            // here and we retry next frame rather than mutating Gothic's string.
            if ( zCModelNodeInst* node = originModel->SearchNode( *nodeName ) ) {
                visFx->SetOriginNode( node );
                LogInfo() << "Repaired VisualFX origin node '" << nodeName->ToChar() << "' on '"
                    << originModel->GetModelName() << "'";
            }
        }
    }
#endif
}

bool GothicAPI::IsUnservableSkeletalShapeEmitter( zCParticleFX* fx ) {
    zCParticleEmitter* emitter = fx->GetEmitter();
    if ( !emitter )
        return false;

    // zPFX_EMITTER_SHAPE_MESH. shpModel is only non-null on the oCVisualFX "emAdjustShpToOrigin"
    // path, where it points at the *origin vob's live zCModel* (oVisFx.cpp).
    if ( emitter->GetVisShpType() != 5 )
        return false;

    zCModel* shapeModel = emitter->GetVisShpModel();
    if ( !shapeModel )
        return false; // Plain zCMesh/zCProgMeshProto shapes are handled by ZENGIN itself

    // Not just "no info yet" - an info that resolves but carries no skinned geometry is equally
    // unservable, and ZENGIN would never even reach GetLowestLODPoly for it (GetPosition bails on
    // numPolys==0), so this is the only place that failure mode can be caught.
    SkeletalMeshVisualInfo* info = ResolveSkeletalVisualInfo( shapeModel );
    return info == nullptr || info->SkeletalMeshes.empty();
}

int GothicAPI::GetLowestLODNumPolys_SkeletalMesh( zCModel* model ) {
    int numPolys = 0;

    SkeletalMeshVisualInfo* skeletalMesh = ResolveSkeletalVisualInfo( model );
    if ( skeletalMesh ) {
        for ( auto const& itm : skeletalMesh->SkeletalMeshes ) {
            for ( auto& mesh : itm.second ) {
                numPolys += static_cast<int>(mesh->Indices.size() / 3);
            }
        }
    }
    return numPolys;
}

float3* GothicAPI::GetLowestLODPoly_SkeletalMesh( zCModel* model, const int polyId, float3*& polyNormal ) {
    static float3 returnPositions[3];
    static float3 defaultPolyNormal( 0.f, 1.f, 0.f );
    size_t polyIndex = static_cast<size_t>(polyId) * 3;
    polyNormal = &defaultPolyNormal;

    SkeletalMeshVisualInfo* skeletalMesh = ResolveSkeletalVisualInfo( model );
    if ( skeletalMesh ) {
        // ZENGIN calls this once per spawned particle, so the skeleton is evaluated once per
        // (model, frame) and reused. GetBoneTransformsTo writes into our own scratch buffer
        // instead of zCModelNodeInst::TrafoObjToCam - we are inside an engine callback here and
        // must not clobber ZENGIN's world-space node cache (see GetBoneTransformsTo).
        static std::vector<XMFLOAT4X4> transforms;
        static zCModel* cachedModel = nullptr;
        static size_t cachedFrame = static_cast<size_t>(-1);

        const size_t now = GetFrameNumber();
        if ( cachedModel != model || cachedFrame != now ) {
            transforms.clear();
            model->GetBoneTransformsTo( transforms );
            cachedModel = model;
            cachedFrame = now;
        }

        for ( auto const& itm : skeletalMesh->SkeletalMeshes ) {
            for ( auto& mesh : itm.second ) {
                if ( polyIndex >= mesh->Indices.size() ) {
                    polyIndex -= mesh->Indices.size();
                } else {
                    if ( transforms.empty() )
                        break; // No node list on this model - fall through to the guard below

                    float fatness = model->GetModelFatness();

                    for ( int i = 0; i < 3; ++i ) {
                        VERTEX_INDEX _polyId = mesh->Indices[polyIndex + i];
                        ExSkelVertexStruct& _polyVert = mesh->Vertices[_polyId];

                        alignas(32) float floats_0[8];
                        alignas(32) float floats_1[8];
                        alignas(16) unsigned short half2float_0[8] = { _polyVert.Position[0][0], _polyVert.Position[0][1], _polyVert.Position[0][2], _polyVert.weights[0],
                                                                        _polyVert.Position[1][0], _polyVert.Position[1][1], _polyVert.Position[1][2], _polyVert.weights[1] };
                        alignas(16) unsigned short half2float_1[8] = { _polyVert.Position[2][0], _polyVert.Position[2][1], _polyVert.Position[2][2], _polyVert.weights[2],
                                                                        _polyVert.Position[3][0], _polyVert.Position[3][1], _polyVert.Position[3][2], _polyVert.weights[3] };
                        UnquantizeHalfFloat_X8( half2float_0, floats_0 );
                        UnquantizeHalfFloat_X8( half2float_1, floats_1 );

                        XMVECTOR position = XMVectorZero();
                        position += XMVectorReplicate( floats_0[3] ) * XMVector3Transform(
                            XMVectorSet( floats_0[0], floats_0[1], floats_0[2], 1.f ),
                            XMMatrixTranspose( XMLoadFloat4x4( &transforms[_polyVert.boneIndices[0]] ) ) );

                        position += XMVectorReplicate( floats_0[7] ) * XMVector3Transform(
                            XMVectorSet( floats_0[4], floats_0[5], floats_0[6], 1.f ),
                            XMMatrixTranspose( XMLoadFloat4x4( &transforms[_polyVert.boneIndices[1]] ) ) );

                        position += XMVectorReplicate( floats_1[3] ) * XMVector3Transform(
                            XMVectorSet( floats_1[0], floats_1[1], floats_1[2], 1.f ),
                            XMMatrixTranspose( XMLoadFloat4x4( &transforms[_polyVert.boneIndices[2]] ) ) );

                        position += XMVectorReplicate( floats_1[7] ) * XMVector3Transform(
                            XMVectorSet( floats_1[4], floats_1[5], floats_1[6], 1.f ),
                            XMMatrixTranspose( XMLoadFloat4x4( &transforms[_polyVert.boneIndices[3]] ) ) );

                        position += XMVectorReplicate( fatness ) * XMLoadFloat3( reinterpret_cast<const XMFLOAT3*>(&_polyVert.BindPoseNormal) ) ;

                        // world matrix is applied later when particle calculate world position
                        XMMATRIX scale = XMMatrixScalingFromVector( model->GetModelScaleXM() );
                        XMStoreFloat3( reinterpret_cast<XMFLOAT3*>(&returnPositions[i]), XMVector3Transform( position, XMMatrixTranspose( scale ) ) );
                    }
                    return returnPositions;
                }
            }
        }
    }

    // Last-resort guard. Every particle placed from here lands on the model's own origin, so this
    // must stay rare - IsUnservableSkeletalShapeEmitter is supposed to stop the emitter from
    // ticking at all while we cannot serve it. Log once per model so a regression is visible.
    static std::unordered_set<zCModel*> loggedOriginFallback;
    if ( loggedOriginFallback.insert( model ).second ) {
        LogWarn() << "GetLowestLODPoly_SkeletalMesh: no skinned mesh data for '" << model->GetModelName()
            << "' - particles from this shape-mesh emitter fall back to the model origin";
    }

    returnPositions[0] = float3( 0.f, 0.f, 0.f );
    returnPositions[1] = float3( 0.f, 0.f, 0.f );
    returnPositions[2] = float3( 0.f, 0.f, 0.f );
    return returnPositions;
}

/** Called to update the compress backbuffer state */
void GothicAPI::UpdateCompressBackBuffer() {
    // Backend-neutral: reachable from the ImGui settings window and the graphics-preset apply path,
    // both of which run under D3D12 too. OnResetBackBuffer is a BaseGraphicsEngine virtual.
    if ( Engine::GraphicsEngine ) {
        Engine::GraphicsEngine->OnResetBackBuffer();
    }
}

/** Draws a skeletal mesh-vob */
void GothicAPI::DrawSkeletalMeshVob( SkeletalVobInfo* vi, float distance, bool updateState, const std::move_only_function<bool(const zCVob*) const>& ignoreVob ) {

    if (ignoreVob != nullptr && ignoreVob(vi->Vob)){
        // Dont draw main mesh if vob is ignored.
        return;
    }
    // TODO: Put this into the renderer!!
    D3D11GraphicsEngine* g = AsD3D11Engine(Engine::GraphicsEngine);

    zCModel* model = static_cast<zCModel*>(vi->Vob->GetVisual());

    if ( !model || !vi->VisualInfo )
        return; // Gothic fortunately sets this to 0 when it throws the model out of the cache

    if ( !static_cast<SkeletalMeshVisualInfo*>(vi->VisualInfo)->GetIsReady() )
        return; // Still being built on a worker thread - don't touch its mesh data yet

    model->SetIsVisible( true );
    if ( !vi->Vob->GetShowVisual() )
        return;

    const auto now = Engine::GAPI->GetFrameNumber();

    if ( updateState ) {
        // Update attachments
        if ( vi->LastAniUpdateFrame != now ) {
            vi->LastAniUpdateFrame = now;
            model->UpdateAttachedVobs();
        }
        model->UpdateMeshLibTexAniState();
    }

    float4 modelColor;
    if ( Engine::GAPI->GetRendererState().RendererSettings.EnableShadows ) {
        // Let shadows do the work
        modelColor = 0xFFFFFFFF;
    } else {
        if ( vi->Vob->IsIndoorVob() ) {
            // All lightmapped polys have this color, so just use it
            modelColor = DEFAULT_LIGHTMAP_POLY_COLOR;
        } else {
            // Get the color from vob position of the ground poly
            if ( zCPolygon* polygon = vi->Vob->GetGroundPoly() ) {
                static const float inv255f = (1.0f / 255.0f);
                float3 vobPos = vi->Vob->GetPositionWorld();
                float3 polyLightStat = polygon->GetLightStatAtPos( vobPos );
                modelColor.x = polyLightStat.z * inv255f;
                modelColor.y = polyLightStat.y * inv255f;
                modelColor.z = polyLightStat.x * inv255f;
                modelColor.w = 1.f;
            } else {
                modelColor = 0xFFFFFFFF;
            }
        }
    }

    XMMATRIX scale = XMMatrixScalingFromVector( model->GetModelScaleXM() );

    XMMATRIX xmWorld = vi->Vob->GetWorldMatrixXM() * scale;
    XMFLOAT4X4 world; XMStoreFloat4x4(&world, xmWorld);

    float fatness = model->GetModelFatness();

    // Get the bone transforms
    static std::vector<XMFLOAT4X4> transforms;
    transforms.clear();
    model->GetBoneTransforms( &transforms );

    if ( !static_cast<SkeletalMeshVisualInfo*>(vi->VisualInfo)->SkeletalMeshes.empty() ) {
#ifdef BUILD_GOTHIC_2_6_fix
        if ( !model->GetDrawHandVisualsOnly() || *reinterpret_cast<BYTE*>(0x57A694) == 0x90 ) {
#else
        if ( !model->GetDrawHandVisualsOnly() ) {
#endif
            Engine::GraphicsEngine->DrawSkeletalMesh( vi, std::span( transforms ), modelColor, world, fatness );
        }
    } else {
        if ( model->GetMeshSoftSkinList()->NumInArray > 0 ) {
            // Just in case somehow we end up without skeletal meshes and they are available
            WorldConverter::ExtractSkeletalMeshFromVob( model, static_cast<SkeletalMeshVisualInfo*>(vi->VisualInfo) );
        }
    }

    if ( g->GetRenderingStage() == DES_SHADOWMAP_CUBE )
        g->SetActiveVertexShader( VShaderID::VS_ExNodeCube );
    else
        g->SetActiveVertexShader( VShaderID::VS_ExNode );

    // Set up instance info
    VS_ExConstantBuffer_PerInstanceNode instanceInfo;
    instanceInfo.Color = modelColor;

    g->SetupVS_ExMeshDrawCall();
    g->SetupVS_ExConstantBuffer();

    const std::vector<XMFLOAT4X4>& prevBoneTransforms = (vi->HasValidPrevTransforms && !vi->PrevBoneTransforms.empty())
        ? vi->PrevBoneTransforms
        : transforms;
    const XMMATRIX prevWorldMatrix = vi->HasValidPrevTransforms
        ? XMLoadFloat4x4( &vi->PrevWorldMatrix )
        : XMLoadFloat4x4( &world );
    
    gtl::flat_hash_map<int, std::vector<MeshVisualInfo*>>& nodeAttachments = vi->NodeAttachments;
    
    auto cbPool = g->GetConstantBufferPool();
    auto vsBufMPI = g->GetActiveVS()->GetInputIndex( "Matrices_PerInstances" );

    oCNPC* npc = vi->Vob->As<oCNPC>();
    zCModel* mvis = static_cast<zCModel*>( vi->Vob->GetVisual() );
    auto nodeList = mvis->GetNodeList();
    for ( unsigned int i = 0; i < transforms.size(); i++ ) {
        // Check for new visual
        zCModelNodeInst* node = nodeList->Array[i];

        if ( !node->NodeVisual )
            continue; // Happens when you pull your sword for example

        if (npc
            && ignoreVob != nullptr
            && node->ProtoNode
            && node->ProtoNode->NodeName.Length()) {
            if (auto slot = npc->GetInvSlot(node->ProtoNode->NodeName)) {
                if (slot->vob && ignoreVob(slot->vob)) {
                    continue;
                }
            }
        }

        if ( !node->NodeVisual )
            continue; // Happens when you pull your sword for example

        // Check if this is loaded
        if ( node->NodeVisual && nodeAttachments.find( i ) == nodeAttachments.end() ) {
            // It's not, extract it
            WorldConverter::ExtractNodeVisualAsync( i, node, nodeAttachments );
        }

        // Check for changed visual. Gated on GetIsReady(): the worker thread writes Visual as it
        // finishes extracting, so comparing against it any earlier races the extraction job (mirrors
        // the D3D12 attachment path).
        if ( nodeAttachments[i].size() && nodeAttachments[i][0]->GetIsReady()
            && node->NodeVisual != nodeAttachments[i][0]->Visual ) {
            // Check for deleted attachment
            if ( !node->NodeVisual ) {
                // Remove attachment. Shared, so it goes back to the registry, not deleted here.
                WorldConverter::ReleaseNodeAttachments( nodeAttachments, i );

                LogInfo() << "Removed attachment from model " << vi->VisualInfo->VisualName;

                continue; // Go to next attachment
            }
            // Load the new one
            WorldConverter::ExtractNodeVisualAsync( i, node, nodeAttachments );
        }

        if ( model->GetDrawHandVisualsOnly() ) {
            std::string NodeName = node->ProtoNode->NodeName.ToChar();
#ifdef BUILD_GOTHIC_2_6_fix
            if ( NodeName.find( "HAND" ) == std::string::npos && (*reinterpret_cast<BYTE*>(0x57A694) != 0x90 || NodeName.find( "ARM" ) == std::string::npos) ) {
#else
            if ( NodeName.find( "HAND" ) == std::string::npos ) {
#endif
                continue;
            }
        }

        auto nodeAttachment = nodeAttachments.find( i );
        if ( nodeAttachment != nodeAttachments.end() ) {
            // Go through all attachments this node has
            for ( MeshVisualInfo* mvi : nodeAttachment->second ) {
                XMMATRIX curTransform = XMLoadFloat4x4( &transforms[i] );
                XMFLOAT4X4 finalWorld;
                XMStoreFloat4x4( &finalWorld, xmWorld * curTransform );

                XMMATRIX prevTransform = XMLoadFloat4x4( &prevBoneTransforms[i] );
                auto prevWorldNode = prevWorldMatrix * prevTransform;

                // Still being extracted on a worker thread — Meshes is being written to right now, so
                // skip this attachment entirely for this frame rather than race it (mirrors D3D12).
                if ( !mvi->GetIsReady() || !mvi->Visual ) {
                    continue;
                }

                // Setup pixel shader here so that we get correct normals
                // Somehow BindShaderForTexture make normals to be inversed
                if ( g->GetRenderingStage() == DES_MAIN ) {
                    g->SetActivePixelShader( PShaderID::PS_DiffuseAlphaTest );
                    g->BindActivePixelShader();
                }

                // Update animated textures
                bool isMMS = strcmp( mvi->Visual->GetFileExtension( 0 ), ".MMS" ) == 0;
                if ( updateState ) {
                    node->TexAniState.UpdateTexList();
                    if ( isMMS ) {
                        zCMorphMesh* mm = reinterpret_cast<zCMorphMesh*>(mvi->Visual);
                        mm->GetTexAniState()->UpdateTexList();
                    }
                }

                if ( isMMS ) {
                    // Only 0.35f of the fatness wanted by gothic.
                    // They seem to compensate for that with the scaling.
                    instanceInfo.Fatness = std::max<float>( 0.f, fatness * 0.35f );
                    instanceInfo.Scaling = fatness * 0.02f + 1.f;
                } else {
                    instanceInfo.Fatness = 0.f;
                    instanceInfo.Scaling = 1.f;
                }

                if ( distance < 1000 && isMMS ) {
                    zCMorphMesh* mm = reinterpret_cast<zCMorphMesh*>( mvi->Visual );
                    // Only draw this as a morphmesh when rendering the main scene or when rendering as ghost
                    if ( g->GetRenderingStage() == DES_MAIN || g->GetRenderingStage() == DES_GHOST ) {
                        // Update constantbuffer
                        instanceInfo.World = finalWorld;
                        XMStoreFloat4x4( &instanceInfo.PrevWorld, prevWorldNode );
                        cbPool->BindVS(vsBufMPI , cbPool->Allocate(&instanceInfo, sizeof(instanceInfo)));

                        if ( updateState ) {
                            if ( mvi->LastAniUpdateFrame != now ) {
                                WorldConverter::UpdateMorphMeshVisual( mm, mvi );
                                mvi->LastAniUpdateFrame = now;
                            }
                        }
                        DrawMorphMesh( mm, mvi->Meshes );
                        continue;
                    }
                }

                instanceInfo.World = finalWorld;
                XMStoreFloat4x4( &instanceInfo.PrevWorld, prevWorldNode );
                cbPool->BindVS(vsBufMPI, cbPool->Allocate(&instanceInfo, sizeof(instanceInfo)));

                // Go through all materials registered here

                if ( g->GetRenderingStage() == DES_SHADOWMAP
                    || g->GetRenderingStage() == DES_SHADOWMAP_CUBE ) {

                    const bool isCube = g->GetRenderingStage() == DES_SHADOWMAP_CUBE;
                    g->GetWhiteTexture()->BindToPixelShader( 0 );
                    void* lastTex = g->GetWhiteTexture()->GetShaderResourceView().Get();

                    for ( auto const& itm : mvi->Meshes ) {
                        // no texture binding for shadowmap
                        zCTexture* aniTex = nullptr;
                        if ( !itm.first || !(aniTex = itm.first->GetAniTexture()) ) {
                            continue;
                        }

                        if ( lastTex != aniTex ) {
                            if ( aniTex->GetCacheState() != zRES_CACHED_IN ) {
                                continue;
                            }
                            if ( itm.first->HasAlphaTest() || aniTex->HasAlphaChannel() ) {
                                aniTex->GetSurface()->GetEngineTexture()->BindToPixelShader( 0 );
                                lastTex = aniTex;
                            } else if ( isCube ) {
                                g->GetWhiteTexture()->BindToPixelShader( 0 );
                                lastTex = g->GetWhiteTexture()->GetShaderResourceView().Get();
                            } else {
                                lastTex = nullptr;
                                static ID3D11ShaderResourceView* nullSrv = nullptr;
                                g->GetContext()->PSSetShaderResources( 0, 1, &nullSrv );
                            }
                        }

                        // Go through all meshes using that material
                        for ( unsigned int m = 0; m < itm.second.size(); m++ ) {
                            DrawMeshInfo( itm.first, itm.second[m].get() );
                        }
                    }
                } else {
                    for ( auto const& itm : mvi->Meshes ) {
                        if ( itm.first && (itm.first->GetAniTexture()) != nullptr ) {
                            if ( !g->BindTextureNRFX( itm.first, (g->GetRenderingStage() == DES_MAIN) ) )
                                continue;
                        }

                        // Go through all meshes using that material
                        for ( unsigned int m = 0; m < itm.second.size(); m++ ) {
                            DrawMeshInfo( itm.first, itm.second[m].get() );
                        }
                    }
                }
            }
        }
    }

    RendererState.RendererInfo.FrameDrawnVobs++;
}

void GothicAPI::DrawSkeletalMeshVob_Layered( SkeletalVobInfo * vi, float distance, bool updateState, const std::move_only_function<bool(const zCVob*) const>& ignoreVob ) {
    if (ignoreVob != nullptr && ignoreVob(vi->Vob)){
        // Dont draw main mesh if vob is ignored.
        return;
    }
    
    // TODO: Put this into the renderer!!
    D3D11GraphicsEngine* g = AsD3D11Engine(Engine::GraphicsEngine);

    zCModel* model = static_cast<zCModel*>(vi->Vob->GetVisual());

    if ( !model || !vi->VisualInfo )
        return; // Gothic fortunately sets this to 0 when it throws the model out of the cache

    if ( !static_cast<SkeletalMeshVisualInfo*>(vi->VisualInfo)->GetIsReady() )
        return; // Still being built on a worker thread - don't touch its mesh data yet

    model->SetIsVisible( true );
    if ( !vi->Vob->GetShowVisual() )
        return;

    const auto now = Engine::GAPI->GetFrameNumber();

    if ( updateState ) {
        // Update attachments
        if ( vi->LastAniUpdateFrame != now ) {
            vi->LastAniUpdateFrame = now;
            model->UpdateAttachedVobs();
        }
        model->UpdateMeshLibTexAniState();
    }

    float4 modelColor;
    if ( Engine::GAPI->GetRendererState().RendererSettings.EnableShadows ) {
        // Let shadows do the work
        modelColor = 0xFFFFFFFF;
    } else {
        if ( vi->Vob->IsIndoorVob() ) {
            // All lightmapped polys have this color, so just use it
            modelColor = DEFAULT_LIGHTMAP_POLY_COLOR;
        } else {
            // Get the color from vob position of the ground poly
            if ( zCPolygon* polygon = vi->Vob->GetGroundPoly() ) {
                static const float inv255f = (1.0f / 255.0f);
                float3 vobPos = vi->Vob->GetPositionWorld();
                float3 polyLightStat = polygon->GetLightStatAtPos( vobPos );
                modelColor.x = polyLightStat.z * inv255f;
                modelColor.y = polyLightStat.y * inv255f;
                modelColor.z = polyLightStat.x * inv255f;
                modelColor.w = 1.f;
            } else {
                modelColor = 0xFFFFFFFF;
            }
        }
    }

    XMMATRIX scale = XMMatrixScalingFromVector( model->GetModelScaleXM() );

    XMMATRIX xmWorld = vi->Vob->GetWorldMatrixXM() * scale;
    XMFLOAT4X4 world; XMStoreFloat4x4( &world, xmWorld );

    float fatness = model->GetModelFatness();

    // Get the bone transforms
    static std::vector<XMFLOAT4X4> transforms;
    transforms.clear();
    model->GetBoneTransforms( &transforms );

    if ( !static_cast<SkeletalMeshVisualInfo*>(vi->VisualInfo)->SkeletalMeshes.empty() ) {
#ifdef BUILD_GOTHIC_2_6_fix
        if ( !model->GetDrawHandVisualsOnly() || *reinterpret_cast<BYTE*>(0x57A694) == 0x90 ) {
#else
        if ( !model->GetDrawHandVisualsOnly() ) {
#endif
            g->DrawSkeletalMesh_Layered( vi, std::span( transforms ), modelColor, world, fatness );
        }
    } else {
        if ( model->GetMeshSoftSkinList()->NumInArray > 0 ) {
            // Just in case somehow we end up without skeletal meshes and they are available
            WorldConverter::ExtractSkeletalMeshFromVob( model, static_cast<SkeletalMeshVisualInfo*>(vi->VisualInfo) );
        }
    }
    g->SetActiveVertexShader( VShaderID::VS_ExNodeLayered );

    // Set up instance info
    VS_ExConstantBuffer_PerInstanceNode instanceInfo;
    instanceInfo.Color = modelColor;

    g->SetupVS_ExMeshDrawCall();
    g->SetupVS_ExConstantBuffer();

    auto& nodeAttachments = vi->NodeAttachments;
    auto vsBufMPI = g->GetActiveVS()->GetInputIndex( "Matrices_PerInstances" );
    auto cbPool = g->GetConstantBufferPool();

    g->GetWhiteTexture()->BindToPixelShader( 0 );
    void* lastTex = g->GetWhiteTexture()->GetShaderResourceView().Get();

    oCNPC* npc = vi->Vob->As<oCNPC>();
    zCModel* mvis = static_cast<zCModel*>( vi->Vob->GetVisual() );
    auto nodeList = mvis->GetNodeList();
    for ( unsigned int i = 0; i < transforms.size(); i++ ) {
        // Check for new visual
        zCModelNodeInst* node = nodeList->Array[i];

        if ( !node->NodeVisual )
            continue; // Happens when you pull your sword for example

        if (npc
            && ignoreVob != nullptr
            && node->ProtoNode
            && node->ProtoNode->NodeName.Length()) {
            if (auto slot = npc->GetInvSlot(node->ProtoNode->NodeName)) {
                if (slot->vob && ignoreVob(slot->vob)) {
                    continue;
                }
            }
        }
        
        // Check if this is loaded
        if ( node->NodeVisual && nodeAttachments.find( i ) == nodeAttachments.end() ) {
            // It's not, extract it
            WorldConverter::ExtractNodeVisualAsync( i, node, nodeAttachments );
        }

        // Check for changed visual. Gated on GetIsReady(): the worker thread writes Visual as it
        // finishes extracting, so comparing against it any earlier races the extraction job (mirrors
        // the D3D12 attachment path).
        if ( nodeAttachments[i].size() && nodeAttachments[i][0]->GetIsReady()
            && node->NodeVisual != nodeAttachments[i][0]->Visual ) {
            // Check for deleted attachment
            if ( !node->NodeVisual ) {
                // Remove attachment. Shared, so it goes back to the registry, not deleted here.
                WorldConverter::ReleaseNodeAttachments( nodeAttachments, i );

                LogInfo() << "Removed attachment from model " << vi->VisualInfo->VisualName;

                continue; // Go to next attachment
            }
            // Load the new one
            WorldConverter::ExtractNodeVisualAsync( i, node, nodeAttachments );
        }

        if ( model->GetDrawHandVisualsOnly() ) {
            std::string_view NodeName = node->ProtoNode->NodeName.ToView();
#ifdef BUILD_GOTHIC_2_6_fix
            if ( NodeName.find( "HAND" ) == std::string_view::npos && (*reinterpret_cast<BYTE*>(0x57A694) != 0x90 || NodeName.find( "ARM" ) == std::string_view::npos) ) {
#else
            if ( NodeName.find( "HAND" ) == std::string_view::npos ) {
#endif
                continue;
            }
        }

        auto nodeAttachment = nodeAttachments.find( i );
        if ( nodeAttachment != nodeAttachments.end() ) {
            // Go through all attachments this node has
            for ( MeshVisualInfo* mvi : nodeAttachment->second ) {
                XMMATRIX curTransform = XMLoadFloat4x4( &transforms[i] );
                XMFLOAT4X4 finalWorld;
                XMStoreFloat4x4( &finalWorld, xmWorld * curTransform );

                // Still being extracted on a worker thread — Meshes is being written to right now, so
                // skip this attachment entirely for this frame rather than race it (mirrors D3D12).
                if ( !mvi->GetIsReady() || !mvi->Visual ) {
                    continue;
                }

                // Setup pixel shader here so that we get correct normals
                // Somehow BindShaderForTexture make normals to be inversed
                if ( g->GetRenderingStage() == DES_MAIN ) {
                    g->SetActivePixelShader( PShaderID::PS_DiffuseAlphaTest );
                    g->BindActivePixelShader();
                }

                // Update animated textures
                bool isMMS = strcmp( mvi->Visual->GetFileExtension( 0 ), ".MMS" ) == 0;
                if ( updateState ) {
                    node->TexAniState.UpdateTexList();
                    if ( isMMS ) {
                        zCMorphMesh* mm = reinterpret_cast<zCMorphMesh*>(mvi->Visual);
                        mm->GetTexAniState()->UpdateTexList();
                    }
                }

                if ( isMMS ) {
                    // Only 0.35f of the fatness wanted by gothic.
                    // They seem to compensate for that with the scaling.
                    instanceInfo.Fatness = std::max<float>( 0.f, fatness * 0.35f );
                    instanceInfo.Scaling = fatness * 0.02f + 1.f;
                } else {
                    instanceInfo.Fatness = 0.f;
                    instanceInfo.Scaling = 1.f;
                }

                instanceInfo.World = finalWorld;
                instanceInfo.PrevWorld = finalWorld;
                // Update constantbuffer
                cbPool->BindVS(vsBufMPI , cbPool->Allocate(&instanceInfo, sizeof(instanceInfo)));

                if ( distance < 1000 && isMMS ) {
                    zCMorphMesh* mm = reinterpret_cast<zCMorphMesh*>( mvi->Visual );
                    // Only draw this as a morphmesh when rendering the main scene or when rendering as ghost
                    if ( g->GetRenderingStage() == DES_MAIN || g->GetRenderingStage() == DES_GHOST ) {
                        if ( updateState ) {
                            if ( mvi->LastAniUpdateFrame != now ) {
                                WorldConverter::UpdateMorphMeshVisual( mm, mvi );
                                mvi->LastAniUpdateFrame = now;
                            }
                        }
                        DrawMorphMesh_Layered( mm, mvi->Meshes );
                        continue;
                    }
                }

                // Go through all materials registered here
                for ( auto const& itm : mvi->Meshes ) {
                    zCTexture* texture;
                    if ( itm.first && (texture = itm.first->GetAniTexture()) != nullptr ) {
                        if ( texture->CacheIn( 0.6f ) != zRES_CACHED_IN ) {
                            continue; // we cant determine if we need to draw this, alpha data is only available after loading a texture.
                        }

                        const bool needTex = texture != lastTex
                            && (texture->HasAlphaChannel() || itm.first->HasAlphaTest());

                        if ( needTex ) {
                            texture->GetSurface()->GetEngineTexture()->BindToPixelShader( 0 );
                            lastTex = texture;
                        } else if ( lastTex != g->GetWhiteTexture()->GetShaderResourceView().Get() ) {
                            g->GetWhiteTexture()->BindToPixelShader( 0 );
                            lastTex = g->GetWhiteTexture()->GetShaderResourceView().Get();
                        }
                    }

                    // Go through all meshes using that material
                    for ( unsigned int m = 0; m < itm.second.size(); m++ ) {
                        DrawMeshInfo_Layered( itm.first, itm.second[m].get() );
                    }
                }
            }
        }
    }

    RendererState.RendererInfo.FrameDrawnVobs++;
}


void GothicAPI::BeginTransparencyVobRun() {
    // Setup alpha blending
    RendererState.RasterizerState.SetDefault();
    RendererState.RasterizerState.SetDirty();
    RendererState.BlendState.SetAlphaBlending();
    RendererState.BlendState.SetDirty();
    RendererState.DepthState.SetDefault();
    RendererState.DepthState.SetDirty();
}

void GothicAPI::DrawTransparencyVob( const TransparencyVobInfo& TransVobInfo ) {
    ZoneScopedN( "GothicAPI::DrawTransparencyVob" );
    D3D11GraphicsEngine* g = AsD3D11Engine(Engine::GraphicsEngine);

    auto cbPool = g->GetConstantBufferPool();
    auto psBufGAI = g->GetShaderManager().GetPShader( PShaderID::PS_Transparency )->GetInputIndex( "GhostAlphaInfo" );

    VS_ExConstantBuffer_PerInstance cbPerInstance;
    {
        if ( TransVobInfo.skeletalVob ) {
            // We need to do Z-prepass first
            g->UnbindActivePS();
            D3D11PipelineStateCache::SetPixelShader( g->GetContext().Get(), nullptr );
            DrawSkeletalMeshVob( TransVobInfo.skeletalVob, TransVobInfo.distance );
            RendererState.RendererInfo.FrameDrawnVobs--; // Don't calculate prepass as drawn vob

            // Now actually draw mesh using transparency pixel shader
            g->SetActivePixelShader( PShaderID::PS_TransparencySkel );
            g->BindActivePixelShader();

            // Update transparency alpha information
            GhostAlphaConstantBuffer gacb;
            gacb.GA_ViewportSize = float2( Engine::GraphicsEngine->GetResolution().x, Engine::GraphicsEngine->GetResolution().y );
            gacb.GA_Alpha = TransVobInfo.alpha;
            cbPool->BindPS(psBufGAI , cbPool->Allocate(&gacb, sizeof(gacb)));
            DrawSkeletalMeshVob( TransVobInfo.skeletalVob, TransVobInfo.distance, false );
        } else if ( TransVobInfo.normalVob ) {
            // Still being filled in on a worker thread (GothicAPI::OnAddVob's async
            // Extract3DSMeshFromVisual2Async) - skip until Meshes is safe to iterate.
            if ( !TransVobInfo.normalVob->VisualInfo->GetIsReady() ) return;

            g->SetActiveVertexShader( VShaderID::VS_Ex );
            g->SetupVS_ExMeshDrawCall();
            
            TransVobInfo.normalVob->UpdateVobConstantBuffer( cbPerInstance );
            cbPool->BindVS(1 , cbPool->Allocate(&cbPerInstance, sizeof(cbPerInstance)));

            // We need to do Z-prepass first
            g->UnbindActivePS();
            D3D11PipelineStateCache::SetPixelShader( g->GetContext().Get(), nullptr );

            for ( auto const& materialMesh : TransVobInfo.normalVob->VisualInfo->Meshes ) {
                if ( materialMesh.first ) {
                    if ( zCTexture* aniTex = materialMesh.first->GetAniTexture() ) {
                        if ( aniTex->CacheIn( 0.6f ) == zRES_CACHED_IN ) {
                            aniTex->Bind( 0 );
                        }
                    }
                }

                for ( auto const& meshInfo : materialMesh.second ) {
                    g->DrawVertexBufferIndexed(
                        meshInfo->GetMeshVertexBuffer(),
                        meshInfo->GetMeshIndexBuffer(),
                        meshInfo->Indices.size() );
                }
            }
            RendererState.RendererInfo.FrameDrawnVobs--; // Don't calculate prepass as drawn vob

            // Now actually draw mesh using transparency pixel shader
            g->SetActivePixelShader( PShaderID::PS_Transparency );
            g->BindActivePixelShader();

            // Update transparency alpha information
            GhostAlphaConstantBuffer gacb;
            gacb.GA_ViewportSize = float2( Engine::GraphicsEngine->GetResolution().x, Engine::GraphicsEngine->GetResolution().y );
            gacb.GA_Alpha = TransVobInfo.alpha;
            cbPool->BindPS(psBufGAI , cbPool->Allocate(&gacb, sizeof(gacb)));

            for ( auto const& materialMesh : TransVobInfo.normalVob->VisualInfo->Meshes ) {
                if ( materialMesh.first ) {
                    if ( zCTexture* aniTex = materialMesh.first->GetAniTexture() ) {
                        if ( aniTex->CacheIn( 0.6f ) == zRES_CACHED_IN ) {
                            aniTex->Bind( 0 );
                        }
                    }
                }

                for ( auto const& meshInfo : materialMesh.second ) {
                    g->DrawVertexBufferIndexed(
                        meshInfo->GetMeshVertexBuffer(),
                        meshInfo->GetMeshIndexBuffer(),
                        meshInfo->Indices.size() );
                }
            }
        }
    }
}

void GothicAPI::DrawSkeletalVN() {
    while ( !VNSkeletalVobs.empty() ) {
        SkeletalVobInfo* vi = VNSkeletalVobs.back();

        RendererState.RasterizerState.SetDefault();
        RendererState.RasterizerState.SetDirty();
        RendererState.BlendState.SetAlphaBlending();
        RendererState.BlendState.SetDirty();
        RendererState.DepthState.SetDefault();
        RendererState.DepthState.SetDirty();

        D3D11GraphicsEngine* g = AsD3D11Engine(Engine::GraphicsEngine);

        zCModel* model = static_cast<zCModel*>(vi->Vob->GetVisual());

        if ( model && vi->VisualInfo && static_cast<SkeletalMeshVisualInfo*>(vi->VisualInfo)->GetIsReady() ) {
            XMMATRIX scale = XMMatrixScalingFromVector( model->GetModelScaleXM() );

            XMMATRIX xmWorld = vi->Vob->GetWorldMatrixXM() * scale;
            XMFLOAT4X4 world; XMStoreFloat4x4( &world, xmWorld );

            float fatness = model->GetModelFatness();

            // Get the bone transforms
            static std::vector<XMFLOAT4X4> transforms;
            transforms.clear();
            model->GetBoneTransforms( &transforms );

            if ( !static_cast<SkeletalMeshVisualInfo*>(vi->VisualInfo)->SkeletalMeshes.empty() ) {
                g->DrawSkeletalVertexNormals( vi, world, std::span( transforms ), 0xFFFFFF, fatness);
            }
        }

        VNSkeletalVobs.pop_back();
    }
}

/** Called when a particle system got removed */
void GothicAPI::OnParticleFXDeleted( zCParticleFX* pfx ) {
    // Remove this from our list
    size_t i = 0, end = ParticleEffectVobs.size();
    while ( i < end ) {
        zCVob* pfxVob = ParticleEffectVobs[i];
        if ( pfxVob->GetVisual() == reinterpret_cast<zCVisual*>(pfx) ) {
            DestroyParticleEffect( ParticleEffectVobs[i] );
            ParticleEffectVobs[i] = ParticleEffectVobs.back();
            ParticleEffectVobs.pop_back();
            --end;
        } else {
            ++i;
        }
    }
}


/** Draws a zCParticleFX */
void GothicAPI::DrawParticleFX( zCVob* source, zCParticleFX* fx, ParticleFrameData& data ) {
    // ZENGIN may have left this emitter's shape mesh unset because the origin had no visual yet.
    RepairShapeMeshEmitter( source, fx );

    if ( IsUnservableSkeletalShapeEmitter( fx ) ) {
        // This emitter samples its spawn positions off a skeletal mesh whose skinned data we
        // haven't extracted yet (LoadzCModelData runs on a worker). Spawning now would put every
        // particle on the model's origin, so hold the effect instead - see
        // IsUnservableSkeletalShapeEmitter. Nothing is drawn this frame either - anything already
        // spawned sits on the origin and is exactly what we don't want on screen.
        if ( !fx->GetVisualDied() ) {
            zCParticleFX::GetStaticPFXList()->TouchPfx( fx );
        }
        return;
    }

    // Update effects time
    fx->UpdateTime();

    // Maybe create more emitters?
    fx->CheckDependentEmitter();

    zTParticle* pfx = fx->GetFirstParticle();
    if ( pfx ) {
        // Get texture
        zCTexture* texture = nullptr;
        if ( zCParticleEmitter* emitter = fx->GetEmitter() ) {
            if ( emitter->GetVisShpType() == 5 && !ParticleEffectProgMeshes.contains(source) ) {
                AddParticleEffect( source );
            }
            if ( (texture = emitter->GetVisTexture( pfx )) != nullptr ) {
                // Check if it's loaded
                if ( texture->CacheIn( 0.6f ) != zRES_CACHED_IN ) {
                    return;
                }
            } else {
                return;
            }
        }

        // Set render states for this type
        ParticleRenderInfo& inf = FrameParticleInfo[texture];

        switch ( fx->GetEmitter()->GetVisAlphaFunc() ) {
        case zRND_ALPHA_FUNC_ADD:
            inf.BlendState.SetAdditiveBlending();
            inf.BlendMode = zRND_ALPHA_FUNC_ADD;
            break;

        case zRND_ALPHA_FUNC_MUL:
            inf.BlendState.SetModulateBlending();
            inf.BlendMode = zRND_ALPHA_FUNC_MUL;
            break;

        default:
            inf.BlendState.SetAlphaBlending();
            inf.BlendMode = zRND_ALPHA_FUNC_BLEND;
            break;
        }

        std::vector<ParticleInstanceInfo>& part = FrameParticles[texture];

        // Check for kill
        zTParticle* kill = nullptr;
        zTParticle* p = nullptr;

        for ( ;;) {
            kill = pfx;
            if ( kill && (kill->LifeSpan < *fx->GetPrivateTotalTime()) ) {
                if ( kill->PolyStrip )
                    zCObject_Release( kill->PolyStrip ); // TODO: MEMLEAK RIGHT HERE!

                pfx = kill->Next;
                fx->SetFirstParticle( pfx );

                kill->Next = *reinterpret_cast<zTParticle**>(GothicMemoryLocations::GlobalObjects::s_globFreePart);
                *reinterpret_cast<zTParticle**>(GothicMemoryLocations::GlobalObjects::s_globFreePart) = kill;
                continue;
            }
            break;
        }

        for ( p = pfx; p; p = p->Next ) {
            for ( ;;) {
                kill = p->Next;
                if ( kill && (kill->LifeSpan < *fx->GetPrivateTotalTime()) ) {
                    if ( kill->PolyStrip )
                        zCObject_Release( kill->PolyStrip );

                    p->Next = kill->Next;
                    kill->Next = *reinterpret_cast<zTParticle**>(GothicMemoryLocations::GlobalObjects::s_globFreePart);
                    *reinterpret_cast<zTParticle**>(GothicMemoryLocations::GlobalObjects::s_globFreePart) = kill;
                    continue;
                }
                break;
            }

            if ( p->PolyStrip ) {
                PolyStripVisuals.insert( p->PolyStrip );
            };

            // Generate instance info
            part.emplace_back();
            ParticleInstanceInfo& ii = part.back();
            ii.scale = float3( p->Size.x, p->Size.y, 0.f );

            // Construct world matrix
            ii.drawMode = fx->GetEmitter()->GetVisAlignment();
            if ( fx->GetEmitter()->GetVisIsQuadPoly() ) {
                ii.drawMode += 10;
            }

            float4 color;
            color.x = p->Color.x / 255.0f;
            color.y = p->Color.y / 255.0f;
            color.z = p->Color.z / 255.0f;

            if ( fx->GetEmitter()->GetVisTexAniIsLooping() != 2 ) { // 2 seems to be some magic case with sinus smoothing
                color.w = std::min( p->Alpha, 255.0f ) / 255.0f;
            } else {
                color.w = std::min( (zCParticleFX::SinSmooth( fabs( (p->Alpha - fx->GetEmitter()->GetVisAlphaStart()) * fx->GetEmitter()->GetAlphaDist() ) ) * p->Alpha) / 255.0f, 1.0f );
            }

            color.w = std::max( color.w, 0.0f );

            ii.position = p->PositionWS;
            ii.color = color;
            ii.velocity = p->Vel;

            if ( fx->GetEmitter()->GetVisAlignment() == 2 ) {
                if ( zCVob* connectedVob = fx->GetConnectedVob() ) {
                    XMFLOAT4X4* worldMatrix = connectedVob->GetWorldMatrixPtr();
                    ii.scale = float3( worldMatrix->m[0][0] * p->Size.x, worldMatrix->m[1][0] * p->Size.x, worldMatrix->m[2][0] * p->Size.x );
                    ii.velocity = float3( worldMatrix->m[0][2] * p->Size.y, worldMatrix->m[1][2] * p->Size.y, worldMatrix->m[2][2] * p->Size.y );
                }
            }

            fx->UpdateParticle( p );
        }
    }

    // Create new particles?
    fx->CreateParticlesUpdateDependencies();

    if ( fx->GetVisualDied() ) {
        if ( zCVob* connectedVob = fx->GetConnectedVob() ) {
            // delete FX, it will be invalid after this call!
            connectedVob->GetHomeWorld()->RemoveVob( connectedVob );
        }
    } else {
        zCParticleFX::GetStaticPFXList()->TouchPfx( fx );
    }
}

/** Debugging */
void GothicAPI::DrawTriangle( float3 pos = { 0.0f,0.0f,0.0f } ) {
    std::unique_ptr<GfxVertexBuffer> vxb;
    Engine::GraphicsEngine->CreateVertexBuffer( vxb );
    vxb->Init( nullptr, 6 * sizeof( ExVertexStruct ), GfxVertexBuffer::EBindFlags::B_VERTEXBUFFER, GfxVertexBuffer::EUsageFlags::U_DYNAMIC, GfxVertexBuffer::CA_WRITE );

    ExVertexStruct vx[6];
    ZeroMemory( vx, sizeof( vx ) );

    float scale = 50.0f;
    vx[0].Position = float3( 0.0f, 0.5f * scale, 0.0f );
    vx[1].Position = float3( 0.45f * scale, -0.5f * scale, 0.0f );
    vx[2].Position = float3( -0.45f * scale, -0.5f * scale, 0.0f );

    vx[0].Color = float4( 1, 0, 0, 1 ).ToDWORD();
    vx[1].Color = float4( 0, 1, 0, 1 ).ToDWORD();
    vx[2].Color = float4( 0, 0, 1, 1 ).ToDWORD();

    vx[3].Position = vx[0].Position;
    vx[5].Position = vx[1].Position;
    vx[4].Position = vx[2].Position;

    vx[3].Color = vx[0].Color;
    vx[5].Color = vx[1].Color;
    vx[4].Color = vx[2].Color;

    for (auto& i : vx) {
        i.Position.x += pos.x;
        i.Position.y += pos.y;
        i.Position.z += pos.z;
    }

    vxb->UpdateBuffer( vx );

    Engine::GraphicsEngine->DrawVertexBuffer( vxb.get(), 6 );
}

/** Sets the Projection matrix */
void XM_CALLCONV GothicAPI::SetProjTransformXM( const XMMATRIX proj ) {
    XMStoreFloat4x4( &RendererState.TransformState.TransformProj, proj );
}

/** Sets the Projection matrix */
XMFLOAT4X4 GothicAPI::GetProjTransform() {
    return RendererState.TransformState.TransformProj;
}

/** Sets the world matrix */
void XM_CALLCONV GothicAPI::SetWorldTransformXM( XMMATRIX world, bool transpose ) {
    if ( transpose )
        XMStoreFloat4x4( &RendererState.TransformState.TransformWorld, XMMatrixTranspose( world ) );
    else
        XMStoreFloat4x4( &RendererState.TransformState.TransformWorld, world );
}

/** Sets the world matrix */
void XM_CALLCONV GothicAPI::SetViewTransformXM( XMMATRIX view, bool transpose ) {
    if ( transpose )
        XMStoreFloat4x4( &RendererState.TransformState.TransformView, XMMatrixTranspose( view ) );
    else
        XMStoreFloat4x4( &RendererState.TransformState.TransformView, view );
}

/** Sets the world matrix */
void GothicAPI::SetViewTransform( const XMFLOAT4X4& view, bool transpose ) {
    if ( transpose )
        XMStoreFloat4x4( &RendererState.TransformState.TransformView, XMMatrixTranspose( XMLoadFloat4x4( &view ) ) );
    else
        RendererState.TransformState.TransformView = view;
}

/** Sets the world matrix */
void GothicAPI::SetWorldViewTransform( const XMFLOAT4X4& world, const XMFLOAT4X4& view ) {
    RendererState.TransformState.TransformWorld = world;
    RendererState.TransformState.TransformView = view;
}

/** Sets the world matrix */
void XM_CALLCONV  GothicAPI::SetWorldViewTransform( XMMATRIX world, CXMMATRIX view ) {
    XMStoreFloat4x4( &RendererState.TransformState.TransformWorld, world );
    XMStoreFloat4x4( &RendererState.TransformState.TransformView, view );
}

/** Sets the world matrix */
void GothicAPI::ResetWorldTransform() {
    XMStoreFloat4x4( &RendererState.TransformState.TransformWorld, XMMatrixTranspose( XMMatrixIdentity() ) );
}

/** Sets the world matrix */
void GothicAPI::ResetViewTransform() {
    XMStoreFloat4x4( &RendererState.TransformState.TransformView, XMMatrixTranspose( XMMatrixIdentity() ) );
}

/** Returns the wrapped world mesh */
MeshInfo* GothicAPI::GetWrappedWorldMesh() {
    return WrappedWorldMesh;
}

/** Returns the loaded sections */
std::map<int, std::map<int, WorldMeshSectionInfo>>& GothicAPI::GetWorldSections() {
    return WorldSections;
}

static bool TraceWorldMeshBoxCmp( const std::pair<WorldMeshSectionInfo*, float>& a, const std::pair<WorldMeshSectionInfo*, float>& b ) {
    return a.second > b.second;
}

/** Traces vobs with static mesh visual */
VobInfo* GothicAPI::TraceStaticMeshVobsBB( const XMFLOAT3& origin, const XMFLOAT3& dir, XMFLOAT3& hit, zCMaterial** hitMaterial ) {
    float closest = FLT_MAX;

    std::list<VobInfo*> hitBBs;

    XMFLOAT3 min;
    XMFLOAT3 max;

    for ( auto& [vob, vobInfo] : VobMap ) {
        // Still being filled in on a worker thread (GothicAPI::OnAddVob's async
        // Extract3DSMeshFromVisual2Async) - skip until BBox/Meshes are safe to read (this is the editor's
        // mouse-picking ray, which later walks vobInfo->VisualInfo->Meshes via TraceVisualInfo).
        if ( !vobInfo->VisualInfo || !vobInfo->VisualInfo->GetIsReady() ) continue;

        XMMATRIX world = XMMatrixTranspose( XMLoadFloat4x4( vob->GetWorldMatrixPtr() ) );
        XMStoreFloat3( &min, XMVector3TransformCoord( XMLoadFloat3( &vobInfo->VisualInfo->BBox.Min ), world ) );
        XMStoreFloat3( &max, XMVector3TransformCoord( XMLoadFloat3( &vobInfo->VisualInfo->BBox.Max ), world ) );

        float t = 0;
        if ( Toolbox::IntersectBox( min, max, origin, dir, t ) ) {
            if ( t < closest ) {
                closest = t;
                hitBBs.push_back( vobInfo );
            }
        }
    }

    // Now trace the actual triangles to find the real hit

    closest = FLT_MAX;
    zCMaterial* closestMaterial = nullptr;
    VobInfo* closestVob = nullptr;
    XMFLOAT3 localOrigin;
    XMFLOAT3 localDir;

    for ( VobInfo* vobInfo : hitBBs ) {
        XMMATRIX invWorld = XMMatrixInverse( nullptr, XMMatrixTranspose( XMLoadFloat4x4( vobInfo->Vob->GetWorldMatrixPtr() ) ) );
        XMStoreFloat3( &localOrigin, XMVector3TransformCoord( XMLoadFloat3( &origin ), invWorld ) );
        XMStoreFloat3( &localDir, XMVector3TransformNormal( XMLoadFloat3( &dir ), invWorld ) );

        zCMaterial* hitMat = nullptr;
        float t = TraceVisualInfo( localOrigin, localDir, vobInfo->VisualInfo, &hitMat );
        if ( t > 0.0f && t < closest ) {
            closest = t;
            closestVob = vobInfo;
            closestMaterial = hitMat;
        }
    }

    if ( closest == FLT_MAX )
        return nullptr;

    if ( hitMaterial )
        *hitMaterial = closestMaterial;

    XMStoreFloat3( &hit, XMLoadFloat3( &origin ) + XMLoadFloat3( &dir ) * closest );

    return closestVob;
}

SkeletalVobInfo* GothicAPI::TraceSkeletalMeshVobsBB( const XMFLOAT3& origin, const XMFLOAT3& dir, XMFLOAT3& hit ) {
    float closest = FLT_MAX;
    SkeletalVobInfo* vob = nullptr;
    XMFLOAT3 BBoxlocal_min;
    XMFLOAT3 BBoxlocal_max;

    for ( auto const& it : SkeletalMeshVobs ) {
        float t = 0;
        XMStoreFloat3( &BBoxlocal_min, XMVectorSet( it->Vob->GetBBoxLocal().Min.x, it->Vob->GetBBoxLocal().Min.y, it->Vob->GetBBoxLocal().Min.z, 0 ) + it->Vob->GetPositionWorldXM() );
        XMStoreFloat3( &BBoxlocal_max, XMVectorSet( it->Vob->GetBBoxLocal().Max.x, it->Vob->GetBBoxLocal().Max.y, it->Vob->GetBBoxLocal().Max.z, 0 ) + it->Vob->GetPositionWorldXM() );
        if ( Toolbox::IntersectBox( BBoxlocal_min, BBoxlocal_max, origin, dir, t ) ) {
            if ( t < closest ) {
                closest = t;
                vob = it;
            }
        }
    }

    if ( closest == FLT_MAX )
        return nullptr;

    XMStoreFloat3( &hit, XMLoadFloat3( &origin ) + XMLoadFloat3( &dir ) * closest );

    return vob;
}

float GothicAPI::TraceVisualInfo( const XMFLOAT3& origin, const XMFLOAT3& dir, BaseVisualInfo* visual, zCMaterial** hitMaterial ) {
    float u, v, t;
    float closest = FLT_MAX;

    for ( auto const& it : visual->Meshes ) {
        for ( unsigned int m = 0; m < it.second.size(); m++ ) {
            auto& mesh = it.second[m];

            for ( unsigned int i = 0; i < mesh->Indices.size(); i += 3 ) {
                if ( Toolbox::IntersectTri( mesh->Vertices[mesh->Indices[i]].Position,
                    mesh->Vertices[mesh->Indices[i + 1]].Position,
                    mesh->Vertices[mesh->Indices[i + 2]].Position,
                    origin, dir, u, v, t ) ) {
                    if ( t > 0 && t < closest ) {
                        closest = t;

                        if ( hitMaterial )
                            *hitMaterial = it.first;
                    }
                }
            }
        }
    }

    return closest == FLT_MAX ? -1.0f : closest;
}

/** Traces the worldmesh and returns the hit-location */
bool GothicAPI::TraceWorldMesh( const XMFLOAT3& origin, const XMFLOAT3& dir, XMFLOAT3& hit, std::string* hitTextureName, XMFLOAT3* hitTriangle, MeshInfo** hitMesh, zCMaterial** hitMaterial ) {
    const int maxSections = 2;
    float closest = FLT_MAX;
    std::list<std::pair<WorldMeshSectionInfo*, float>> hitSections;

    // Trace bounding-boxes first
    for ( auto&& itx : WorldSections ) {
        for ( auto&& ity : itx.second ) {
            WorldMeshSectionInfo& section = ity.second;

            if ( section.WorldMeshes.empty() )
                continue;

            float t = 0;
            if ( Toolbox::PositionInsideBox( origin, section.BoundingBox.Min, section.BoundingBox.Max ) || Toolbox::IntersectBox( section.BoundingBox.Min, section.BoundingBox.Max, origin, dir, t ) ) {
                if ( t < maxSections * WORLD_SECTION_SIZE )
                    hitSections.push_back( std::make_pair( &section, t ) );
            }
        }
    }
    // Distance-sort
    hitSections.sort( TraceWorldMeshBoxCmp );

    int numProcessed = 0;
    for ( auto const& bit : hitSections ) {
        for (auto& worldMesh : bit.first->WorldMeshes) {
            float u, v, t;

            // Positions come from the slim CPU copy the world mesh keeps instead of the full vertices.
            const std::vector<WorldVertexCPU>& verts = worldMesh.second->CpuVertices;
            if ( verts.empty() ) {
                continue;
            }

            for ( unsigned int i = 0; i < worldMesh.second->Indices.size(); i += 3 ) {
                if ( Toolbox::IntersectTri( verts[worldMesh.second->Indices[i]].Position,
                    verts[worldMesh.second->Indices[i + 1]].Position,
                    verts[worldMesh.second->Indices[i + 2]].Position,
                    origin, dir, u, v, t ) ) {
                    if ( t > 0 && t < closest ) {
                        closest = t;

                        if ( hitTriangle ) {
                            hitTriangle[0] = verts[worldMesh.second->Indices[i]].Position;
                            hitTriangle[1] = verts[worldMesh.second->Indices[i + 1]].Position;
                            hitTriangle[2] = verts[worldMesh.second->Indices[i + 2]].Position;
                        }

                        if ( hitMesh ) {
                            *hitMesh = worldMesh.second;
                        }

                        if ( hitMaterial ) {
                            *hitMaterial = worldMesh.first.Material;
                        }

                        if ( hitTextureName && worldMesh.first.Material && worldMesh.first.Material->GetTextureSingle() )
                            *hitTextureName = worldMesh.first.Material->GetTextureSingle()->GetNameWithoutExt();
                    }
                }
            }

            numProcessed++;

            if ( numProcessed == maxSections && closest != FLT_MAX )
                break;
        }
    }
    if ( closest == FLT_MAX )
        return false;


    XMStoreFloat3( &hit, XMLoadFloat3( &origin ) + XMLoadFloat3( &dir ) * closest );

    return true;
}

/** Unprojects a pixel-position on the screen */
void XM_CALLCONV GothicAPI::UnprojectXM(float2 p, XMVECTOR& worldPos, XMVECTOR& worldDir) {
    if ( !GetSceneCamera() ) {
        worldPos = XMVectorReplicate( 0.0f );
        worldDir = worldPos;
        return;
    }
    const auto res = Engine::GraphicsEngine->GetBackbufferResolution();

    XMMATRIX proj    = XMMatrixTranspose(XMLoadFloat4x4(&RendererState.TransformState.TransformProj));
    XMMATRIX invView = XMMatrixTranspose(XMLoadFloat4x4(&GetSceneCamera()->GetTransformDX( zCCamera::ETransformType::TT_VIEW_INV )));

    const float ux = (((2.0f * p.x) / res.x) - 1.0f) / XMVectorGetX(proj.r[0]);
    const float uy = -(((2.0f * p.y) / res.y) - 1.0f) / XMVectorGetY(proj.r[1]);
    XMVECTOR u = XMVectorSet(ux, uy, 1.0f, 0.0f);

    XMVECTOR wPos = XMVector3TransformCoord(u, invView);
    worldPos = wPos;

    XMVECTOR dir  = XMVector3Normalize(u);
    dir = XMVector3TransformNormal(dir, invView);
    worldDir = dir;
}

/** Unprojects the current cursor */
XMVECTOR GothicAPI::UnprojectCursorXM() {
    XMVECTOR mPos, mDir;
    POINT p = GetCursorPosition();

    Engine::GAPI->UnprojectXM( float2(static_cast<float>(p.x), static_cast<float>(p.y)), mPos, mDir );

    return mDir;
}

/** Returns the current cameraposition */
XMFLOAT3 GothicAPI::GetCameraPosition() {
    if ( !oCGame::GetGame()->_zCSession_camVob )
        return XMFLOAT3( 0, 0, 0 );

    if ( CameraReplacementPtr )
        return CameraReplacementPtr->PositionReplacement;

    return oCGame::GetGame()->_zCSession_camVob->GetPositionWorld();
}
/** Returns the current cameraposition */
XMVECTOR GothicAPI::GetCameraPositionXM() {
    if ( !oCGame::GetGame()->_zCSession_camVob )
        return g_XMZero;

    if ( CameraReplacementPtr )
        return XMLoadFloat3( &CameraReplacementPtr->PositionReplacement );

    return oCGame::GetGame()->_zCSession_camVob->GetPositionWorldXM();
}

zTCam_ClipType GothicAPI::GetCameraBBox3DInFrustum( const zTBBox3D& box, int clipFlags ) {
    if ( !oCGame::GetGame()->_zCSession_camVob )
        return zTCam_ClipType::ZTCAM_CLIPTYPE_IN;

    if ( CameraReplacementPtr ) {
        auto result = CameraReplacementPtr->frustum.Contains(box);
        if ( result == ContainmentType::DISJOINT )
            return zTCam_ClipType::ZTCAM_CLIPTYPE_OUT;
        if ( result == ContainmentType::INTERSECTS )
            return zTCam_ClipType::ZTCAM_CLIPTYPE_CROSSING;
        return zTCam_ClipType::ZTCAM_CLIPTYPE_IN;
    }
    if (auto cam = static_cast<zCCamera*>(oCGame::GetGame()->_zCSession_camera); cam) {
        return cam->BBox3DInFrustum(box, clipFlags);
    }
    
    return zTCam_ClipType::ZTCAM_CLIPTYPE_IN;
}

zTCam_ClipType GothicAPI::GetCameraBBox3DInFrustum( const zCVob* vob, int clipFlags, bool isLocalCamera ) {
    if ( CameraReplacementPtr ) {
        auto box = vob->GetBBox();
        return GetCameraBBox3DInFrustum(box, clipFlags);
    }
    if (auto cam = GetSceneCamera(); cam) {
        auto box = isLocalCamera ? vob->GetBBoxLocal() : vob->GetBBox();
        return GetCameraBBox3DInFrustum(box, clipFlags);
    }
    
    return zTCam_ClipType::ZTCAM_CLIPTYPE_IN;
}


/** Returns the view matrix */
void GothicAPI::GetViewMatrix( XMFLOAT4X4* view ) {
    if ( CameraReplacementPtr ) {
        *view = CameraReplacementPtr->ViewReplacement;
        return;
    }

    *view = GetSceneCamera()->GetTransformDX( zCCamera::ETransformType::TT_VIEW );
}

/** Returns the view matrix */
XMMATRIX GothicAPI::GetViewMatrixXM() {
    if ( CameraReplacementPtr ) {
        return XMLoadFloat4x4( &CameraReplacementPtr->ViewReplacement );
    }
    return XMLoadFloat4x4( &GetSceneCamera()->GetTransformDX( zCCamera::ETransformType::TT_VIEW ) );
}

/** Returns the view matrix */
void GothicAPI::GetInverseViewMatrixXM( XMFLOAT4X4* invView ) {
    if ( CameraReplacementPtr ) {
        XMStoreFloat4x4( invView, XMMatrixInverse( nullptr, XMLoadFloat4x4( &CameraReplacementPtr->ViewReplacement ) ) );
        return;
    }

    *invView = GetSceneCamera()->GetTransformDX( zCCamera::ETransformType::TT_VIEW_INV );
}

/** Returns the projection-matrix in Column-Major format, with reversed depth buffer */
XMFLOAT4X4& GothicAPI::GetProjectionMatrix() {
    if ( CameraReplacementPtr ) {
        return CameraReplacementPtr->ProjectionReplacement;
    }

    // Reverse depth buffer with infinite far plane:
    // depth = NearClip / viewZ, where NearClip is fixed at 1.0 in engine units.
    constexpr float NearClip = 1.0f;
    RendererState.TransformState.TransformProj._33 = 0.0f;
    RendererState.TransformState.TransformProj._34 = NearClip;
    return RendererState.TransformState.TransformProj;
}

/** Returns the GSky-Object */
GSky* GothicAPI::GetSky() const {
    return SkyRenderer.get();
}

/** Returns the inventory */
GInventory* GothicAPI::GetInventory() {
    return Inventory.get();
}

/** Returns the far Z */
float GothicAPI::GetFarZ() {
    zCSkyController_Outdoor* sc = oCGame::GetGame()->_zCSession_world->GetSkyControllerOutdoor();
    return sc->GetFarZ();
}

/** Returns the fog-color */
XMVECTOR GothicAPI::GetFogColor() {
    zCSkyController_Outdoor* sc = oCGame::GetGame()->_zCSession_world->GetSkyControllerOutdoor();

    XMVECTOR FogColorMod = XMLoadFloat3( &RendererState.RendererSettings.FogColorMod );

    // Only give the overridden color out if the flag is set
    if ( !sc || !sc->GetOverrideFlag() )

        return FogColorMod;

    const XMFLOAT3 overrideColor = sc->GetOverrideColor();
    XMVECTOR color = XMLoadFloat3( &overrideColor );

    // Clamp to length of 0.5f. Gothic does it at an intensity of 120 / 255.
    float len;
    XMStoreFloat( &len, XMVector3Length( color ) );
    if ( len > 0.5f ) {
        color = XMVector3Normalize( color ) * 0.5f;
        len = 0.5f;
    }

    // Mix these, so the fog won't get black at transitions
    color = XMVectorLerpV( FogColorMod, color, XMVectorSet( len * 2.0f, len * 2.0f, len * 2.0f, 0 ) );

    return color;
}

/** Returns true, if the game was paused */
bool GothicAPI::IsGamePaused() {
    oCGame* game = oCGame::GetGame();
    if ( !game )
        return true;

    return game->GetSingleStep();
}

/** Returns true while an in-game menu holds the game paused */
bool GothicAPI::IsIngameMenuPaused() {
    // Deliberately stricter than IsGamePaused(): that one reports "paused" when there is no game
    // session at all (out-game menu, startup, teardown), which is indistinguishable from loading.
    // With a session present, singleStep can only have been set by oCGame::Pause(), which every
    // in-game menu (ESC main menu, inventory/status, log, map) calls before entering zCMenu::Run().
    oCGame* game = oCGame::GetGame();
    return game && game->GetSingleStep();
}

/** Checks if a game is being saved now */
bool GothicAPI::IsSavingGameNow() {
    oCGame* game = oCGame::GetGame();
    if ( !game )
        return false;

    return (game->save_screen || (game->load_screen && game->inLevelChange));
}

/** Checks if a game is being saved or loaded now */
bool GothicAPI::IsInSavingLoadingState() {
    oCGame* game = oCGame::GetGame();
    if ( !game )
        return false;

    return (game->save_screen || game->load_screen);
}

/** Returns true if the game is overwriting the fog color with a fog-zone */
float GothicAPI::GetFogOverride() {
    zCSkyController_Outdoor* sc = oCGame::GetGame()->_zCSession_world->GetSkyControllerOutdoor();

    // Catch invalid controller
    if ( !sc )
        return 0.0f;
    float veclenght;
    const XMFLOAT3 overrideColor = sc->GetOverrideColor();
    XMStoreFloat( &veclenght, XMVector3Length( XMLoadFloat3( &overrideColor ) ) );
    return sc->GetOverrideFlag() ? std::min( veclenght, 0.5f ) * 2.0f : 0.0f;
}

/** Draws the inventory */
void GothicAPI::DrawInventory( zCWorld* world, zCCamera& camera ) {
    Inventory->DrawInventory( world, camera );
}

LRESULT CALLBACK GothicAPI::GothicWndProc(
    HWND hWnd,
    UINT msg,
    WPARAM wParam,
    LPARAM lParam
) {
    return Engine::GAPI->OnWindowMessage( hWnd, msg, wParam, lParam );
}

/** Sends a message to the original gothic-window */
void GothicAPI::SendMessageToGameWindow( UINT msg, WPARAM wParam, LPARAM lParam ) {
    if ( OriginalGothicWndProc ) {
        CallWindowProc( (WNDPROC)OriginalGothicWndProc, OutputWindow, msg, wParam, lParam );
    }
}

/** Message-Callback for the main window */
LRESULT GothicAPI::OnWindowMessage( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam ) {
    switch ( msg ) {
    case WM_KEYDOWN:
        Engine::GraphicsEngine->OnKeyDown( wParam );
        switch ( wParam ) {
            //#define DUMP_CLASSDEF 1
#if DUMP_CLASSDEF
        case VK_NUMPAD9:
        {
            if ( !oCGame::GetGame()
                || !oCGame::GetGame()->_zCSession_world
                || !oCGame::GetGame()->_zCSession_world->GetGlobalVobTree() ) {
                break;
            }
            zCTree<zCVob>* vobTree = oCGame::GetGame()->_zCSession_world->GetGlobalVobTree();
            std::unordered_map<std::string, uint32_t> items = {};
            TraverseVobTree( vobTree, [&]( zCVob* vob ) {
                zCClassDef* classDef = reinterpret_cast<zCObject*>(vob)->_GetClassDef();
                while ( classDef ) {
                    items[classDef->className.ToChar()] = (uint32_t)classDef;
                    classDef = classDef->baseClassDef;
                }
            } );

            std::stringstream ss;
            for ( auto& kvp : items ) {
                ss.str( std::string{} );
                ss << "static const unsigned int " << kvp.first << " = 0x00" << std::hex << kvp.second << ";";
                LogInfo() << ss.str();
            }
            break;
       
#endif
        case VK_F1:
#ifdef BUILD_SPACER
#define IS_SPACER_BUILD true
#else
#define IS_SPACER_BUILD false
#endif
            if (zCOption::GetOptions()->IsParameter("XEnableEditorPanel") || IS_SPACER_BUILD) {
                Engine::GraphicsEngine->OnUIEvent( BaseGraphicsEngine::EUIEvent::UI_OpenEditor );
            }
            break;
        case VK_F11:
            if ( ( GetAsyncKeyState( VK_CONTROL ) & 0x8000 ) ) {
                Engine::GraphicsEngine->OnUIEvent( BaseGraphicsEngine::EUIEvent::UI_ToggleAdvancedSettings );
            } else {
                Engine::GraphicsEngine->OnUIEvent( BaseGraphicsEngine::EUIEvent::UI_OpenSettings );
            }
            break;

        case VK_ESCAPE:
            Engine::GraphicsEngine->OnUIEvent( BaseGraphicsEngine::EUIEvent::UI_ClosedSettings );
            break;

        case VK_NUMPAD1:
            if ( !Engine::ImGuiHandle->GetIsActive() && !GMPModeActive && Engine::GAPI->GetRendererState().RendererSettings.AllowNumpadKeys )
                SpawnVegetationBoxAt( GetCameraPosition() );
            break;
        }
        default:
            if ( Engine::ImGuiHandle->GetIsActive() ) {
                // do not delegate input further if settings is open
                Engine::ImGuiHandle->OnWindowMessage( hWnd, msg, wParam, lParam );
                return DefWindowProc( hWnd, msg, wParam, lParam );
            }
        break;
    case WM_KEYUP:
        if ( Engine::ImGuiHandle->GetIsActive() ) {
            // do not delegate input further if settings is open
            Engine::ImGuiHandle->OnWindowMessage( hWnd, msg, wParam, lParam );
            return DefWindowProc( hWnd, msg, wParam, lParam );
        }
    // Disable any painting that zengine might be doing
    case WM_PAINT:
    case WM_NCPAINT:
        return DefWindowProc( hWnd, msg, wParam, lParam );

#ifdef BUILD_SPACER
    case WM_SIZE:
        // Reset resolution to windowsize
        Engine::GraphicsEngine->SetWindow( hWnd );
        break;
#endif
    }

    // This is only processed when the bar is activated, so just call this here
    Engine::ImGuiHandle->OnWindowMessage( hWnd, msg, wParam, lParam );
    // Engine::AntTweakBar->OnWindowMessage( hWnd, msg, wParam, lParam );
    Engine::GraphicsEngine->OnWindowMessage( hWnd, msg, wParam, lParam );

#ifdef BUILD_SPACER
    if ( msg == WM_RBUTTONDOWN )
        return 0; // We handle this ourselfes, because we need the ability to hold down the RMB
#endif

    if ( OriginalGothicWndProc ) {
        return CallWindowProc( (WNDPROC)OriginalGothicWndProc, hWnd, msg, wParam, lParam );
    } else
        return 0;
}

/** Recursive helper function to draw the BSP-Tree */
void GothicAPI::DebugDrawTreeNode( zCBspBase* base, zTBBox3D boxCell, int clipFlags ) {
    auto camPos = GetCameraPosition();
    auto camPosXm = GetCameraPositionXM();
    while ( base ) {
        if ( clipFlags > 0 ) {
            float yMaxWorld = Engine::GAPI->GetLoadedWorldInfo()->BspTree->GetRootNode()->BBox3D.Max.y;

            zTBBox3D nodeBox = base->BBox3D;
            float nodeYMax = std::min( yMaxWorld, camPos.y );
            nodeYMax = std::max( nodeYMax, base->BBox3D.Max.y );
            nodeBox.Max.y = nodeYMax;

            zTCam_ClipType nodeClip = GetCameraBBox3DInFrustum( nodeBox, clipFlags );

            if ( nodeClip == ZTCAM_CLIPTYPE_OUT )
                return; // Nothig to see here. Discard this node and the subtree
        }

        if ( base->IsLeaf() ) {
            // Check if this leaf is inside the frustum
            if ( clipFlags > 0 && RendererState.RendererSettings.DebugSettings.Culling.CullBspSections ) {
                if ( GetCameraBBox3DInFrustum( base->BBox3D, clipFlags ) == ZTCAM_CLIPTYPE_OUT )
                    return;
            }

            zCBspLeaf* leaf = static_cast<zCBspLeaf*>(base);
            if ( !leaf->sectorIndex )
                return;

            Engine::GraphicsEngine->GetLineRenderer()->AddAABBMinMax( base->BBox3D.Min, base->BBox3D.Max );
            return;
        } else {
            zCBspNode* node = static_cast<zCBspNode*>(base);

            int	planeAxis = node->PlaneSignbits;

            boxCell.Min.y = node->BBox3D.Min.y;
            boxCell.Max.y = node->BBox3D.Min.y;

            zTBBox3D tmpbox = boxCell;
            XMVECTOR dotResult = XMVector3Dot( XMLoadFloat3( &node->Plane.Normal ), camPosXm );
            if ( XMVectorGetX( dotResult ) > node->Plane.Distance ) {
                if ( node->Front ) {
                    reinterpret_cast<float*>(&tmpbox.Min)[planeAxis] = node->Plane.Distance;
                    DebugDrawTreeNode( node->Front, tmpbox, clipFlags );
                }

                reinterpret_cast<float*>(&boxCell.Max)[planeAxis] = node->Plane.Distance;
                base = node->Back;
            } else {
                if ( node->Back ) {
                    reinterpret_cast<float*>(&tmpbox.Max)[planeAxis] = node->Plane.Distance;
                    DebugDrawTreeNode( node->Back, tmpbox, clipFlags );
                }

                reinterpret_cast<float*>(&boxCell.Min)[planeAxis] = node->Plane.Distance;
                base = node->Front;
            }
        }
    }
}

/** Outlines the world's ghost occluders (WorldOccluders) so their placement can be eyeballed before
    anything culls against them. Frustum- and distance-limited: a world ships up to ~2500 of them and
    feeding every one to the line renderer every frame would swamp it. */
void GothicAPI::DebugDrawOccluders( const Frustum& frustum ) {
    if ( !LoadedWorldInfo || LoadedWorldInfo->Occluders.IsEmpty() )
        return;

    const WorldOccluders& occ = LoadedWorldInfo->Occluders;
    BaseLineRenderer* lines = Engine::GraphicsEngine->GetLineRenderer();
    if ( !lines )
        return;

    const XMVECTOR camPos = GetCameraPositionXM();
    const float maxDist = 20000.0f;   // 200m - beyond that the outlines are unreadable anyway
    const size_t maxDrawn = 400;      // line-renderer budget; overflow is logged once below

    size_t drawn = 0;
    size_t skippedForBudget = 0;
    for ( const WorldOccluders::Entry& e : occ.Entries ) {
        float distSq;
        XMStoreFloat( &distSq, XMVector3LengthSq( XMLoadFloat3( &e.Center ) - camPos ) );
        if ( distSq > (maxDist + e.Radius) * (maxDist + e.Radius) )
            continue;
        if ( !frustum.Intersects( zTBBox3D{
                XMFLOAT3( e.Center.x - e.Radius, e.Center.y - e.Radius, e.Center.z - e.Radius ),
                XMFLOAT3( e.Center.x + e.Radius, e.Center.y + e.Radius, e.Center.z + e.Radius ) } ) )
            continue;

        if ( drawn >= maxDrawn ) { skippedForBudget++; continue; }

        // Green near, red far - makes it obvious which ones actually bound the current view.
        const float t = std::min( 1.0f, std::sqrtf( distSq ) / maxDist );
        const XMFLOAT4 color( t, 1.0f - t, 0.2f, 1.0f );

        for ( uint32_t v = 0; v < e.NumVerts; v++ ) {
            const XMFLOAT3& a = occ.Verts[e.VertexOffset + v];
            const XMFLOAT3& b = occ.Verts[e.VertexOffset + ((v + 1) % e.NumVerts)];
            lines->AddLine( LineVertex( a, color ), LineVertex( b, color ) );
        }
        drawn++;
    }

    if ( skippedForBudget && !OccluderDebugBudgetLogged ) {
        LogInfo() << "DrawWorldOccluders: showing " << maxDrawn << " of " << (drawn + skippedForBudget)
            << " in-view occluders (line budget)";
        OccluderDebugBudgetLogged = true;
    }
}

/** Draws the AABB for the BSP-Tree using the line renderer*/
void GothicAPI::DebugDrawBSPTree() {
    zCBspTree* tree = LoadedWorldInfo->BspTree;
    zCBspBase* root = tree->GetRootNode();

    // Recursively go through the tree and draw all nodes
    DebugDrawTreeNode( root, root->BBox3D );
}

/** Collects vobs using gothics BSP-Tree */
void GothicAPI::CollectVisibleVobs( 
    std::vector<VobInfo*>& vobs,
    std::vector<VobLightInfo*>& lights, 
    std::vector<SkeletalVobInfo*>& mobs, 
    EGothicCullFlags cullFlags,
    EBspTreeCollectFlags collectFlags,
    bool skipVobFrustumCull ) {
    ZoneScopedN( "GothicAPI::CollectVisibleVobsLegacy" );
    zCBspTree* tree = LoadedWorldInfo->BspTree;

    zCBspBase* rootBsp = tree->GetRootNode();
    Frustum frustum = Frustum::AlwaysContainingFrustum();
    bool haveCameraMatrices = false;
    XMMATRIX worldToClip = XMMatrixIdentity();
    // Kept alongside worldToClip so the horizon cull can measure depth in the SAME camera's space.
    XMMATRIX cameraView = XMMatrixIdentity();
    if ( auto cam = GetSceneCamera() ) {
        cam->Activate();

        // Row-Major view
        const auto& view = cam->trafoView;
        const auto& proj = cam->trafoProjection;
        const XMMATRIX viewM = XMMatrixTranspose( XMLoadFloat4x4( &view ) );
        const XMMATRIX projM = XMLoadFloat4x4( &proj );
        frustum.BuildPerspective( viewM, projM );

        worldToClip = XMMatrixMultiply( viewM, projM );
        cameraView = viewM;
        haveCameraMatrices = true;
    }

    if ( CameraReplacementPtr ) {
        LogError() << "Invalid usage of legacy API. Must not use this withCameraReplacementPtr.";
    }

    XMVECTOR cameraPosition = GetCameraPositionXM();
    XMVECTOR playerPosition = Engine::GAPI->GetPlayerVob() != nullptr ? Engine::GAPI->GetPlayerVob()->GetPositionWorldXM() : XMVectorSet( FLT_MAX, FLT_MAX, FLT_MAX, 0 );
    // Take cameraposition if we are freelooking
    if ( zCCamera::IsFreeLookActive() ) {
        playerPosition = cameraPosition;
    }

    // LegacyRenderQueueProxy marks every found item and only pushes unique ones.
    LegacyRenderQueueProxy renderQueue(vobs, lights, mobs );

    RndCullContext ctx;
    ctx.queue = &renderQueue;
    ctx.cameraPosition = GetCameraPosition();
    ctx.stage = RenderStage::STAGE_DRAW_WORLD;
    ctx.frustum = frustum;
    ctx.drawDistances.OutdoorVobs = RendererState.RendererSettings.OutdoorVobDrawRadius;
    ctx.drawDistances.OutdoorVobsSmall = RendererState.RendererSettings.OutdoorSmallVobDrawRadius;
    ctx.drawDistances.IndoorVobs = RendererState.RendererSettings.IndoorVobDrawRadius;
    ctx.drawDistances.VisualFX = RendererState.RendererSettings.VisualFXDrawRadius;
    ctx.drawDistancesSq.OutdoorVobs = ctx.drawDistances.OutdoorVobs * ctx.drawDistances.OutdoorVobs;
    ctx.drawDistancesSq.OutdoorVobsSmall = ctx.drawDistances.OutdoorVobsSmall * ctx.drawDistances.OutdoorVobsSmall;
    ctx.drawDistancesSq.IndoorVobs = ctx.drawDistances.IndoorVobs * ctx.drawDistances.IndoorVobs;
    ctx.drawDistancesSq.VisualFX = ctx.drawDistances.VisualFX * ctx.drawDistances.VisualFX;
    ctx.drawFlags.DrawVOBs = RendererState.RendererSettings.DrawVOBs;
    ctx.drawFlags.DrawMobs = RendererState.RendererSettings.DrawMobs;
    ctx.drawFlags.EnableDynamicLighting = RendererState.RendererSettings.EnableDynamicLighting;
    ctx.drawFlags.EnableOcclusionCulling = RendererState.RendererSettings.EnableOcclusionCulling;
    ctx.drawFlags.CullVobs = RendererState.RendererSettings.DebugSettings.Culling.CullVobs;
    ctx.drawFlags.SkipVobFrustumCull = skipVobFrustumCull;
    ctx.drawFlags.CollectIndoorVobs = true;
    ctx.drawFlags.CollectMobs = true;
    ctx.drawFlags.CollectLights = true;

    // This overload is the main camera pass of both backends, and the only place portal culling
    // applies: shadow passes need casters from rooms the player cannot see into.
    if ( haveCameraMatrices && PortalCuller.IsActive() ) {
        oCGame* game = oCGame::GetGame();
        PortalCuller.Solve( worldToClip, ctx.cameraPosition, game ? game->_zCSession_camVob : nullptr );
        ctx.portalCuller = &PortalCuller;
    }

    // Rasterize the ghost-occluder horizon for THIS camera, then hand it to the collect. Main camera
    // pass only - a shadow cascade has its own frustum and must not test against the player's skyline.
    Horizon.SetEnabled( RendererState.RendererSettings.EnableHorizonCulling );
    if ( haveCameraMatrices && LoadedWorldInfo && !LoadedWorldInfo->Occluders.IsEmpty() ) {
        const INT2 res = Engine::GraphicsEngine->GetResolution();
        // viewM, NOT GetViewMatrixXM(): worldToClip is viewM*projM from this zCCamera and the horizon
        // compares depths in that camera's space, while TransformView is pass-dependent (the shadow
        // cascades overwrite it through SetCameraReplacementPtr).
        Horizon.Build( LoadedWorldInfo->Occluders, worldToClip, cameraView, ctx.cameraPosition,
            frustum, res.x, res.y );
        if ( Horizon.IsActive() )
            ctx.horizon = &Horizon;
    } else {
        Horizon.Invalidate();
    }

    if ( RendererState.RendererSettings.DrawWorldOccluders ) {
        DebugDrawOccluders( frustum );
    }

    CollectVisibleVobs( ctx );

    if ( RendererState.RendererSettings.SortRenderQueue ) {
        struct SortableVob {
            VobInfo* vob;
            float distSq;
        };
        static thread_local std::vector<SortableVob> sortList;
        sortList.clear();
        sortList.reserve( vobs.size() );

        for ( auto* v : vobs ) {
            float d = XMVectorGetX( XMVector3LengthSq( v->Vob->GetPositionWorldXM() - cameraPosition ) );
            sortList.push_back( { v, d } );
        }

        std::ranges::sort(sortList, []( const SortableVob& a, const SortableVob& b ) {
            return a.distSq < b.distSq;
        } );
        for ( size_t i = 0; i < vobs.size(); ++i ) vobs[i] = sortList[i].vob;

        struct SortableSkeletalVob {
            SkeletalVobInfo* vob;
            float distSq;
        };
        static thread_local std::vector<SortableSkeletalVob> skelsortList;
        skelsortList.clear();
        skelsortList.reserve( mobs.size() );
        for ( auto* v : mobs ) {
            float d = XMVectorGetX( XMVector3LengthSq( v->Vob->GetPositionWorldXM() - cameraPosition ) );
            skelsortList.push_back( { v, d } );
        }

        std::ranges::sort(skelsortList, []( const SortableSkeletalVob& a, const SortableSkeletalVob& b ) {
            return a.distSq < b.distSq;
        } );
        for ( size_t i = 0; i < mobs.size(); ++i ) mobs[i] = skelsortList[i].vob;
    }

    // Copy them into the target
    // they should be unique at this point.

    if ( collectFlags & COLLECT_MUTATE ) {
        const int interactiveFocusEnabled = oCGame::GetHighlightInteractFocus();
        const zCVob* playerFocusVob = interactiveFocusEnabled && oCGame::GetPlayer() ? oCGame::GetPlayer()->GetFocusVob() : nullptr;

        for ( auto it : renderQueue.vobs ) {
            // Still being filled in on a worker thread (GothicAPI::OnAddVob's async
            // Extract3DSMeshFromVisual2Async) - don't add an instance for it yet. DrawVOBsInstanced
            // gates solely on Instances.empty(), so a not-ready visual pushed here would get iterated
            // (MeshesByTexture/Meshes) on the render thread while the worker is still writing to it.
            if ( !it->VisualInfo->GetIsReady() ) continue;

            VobInstanceInfo vii = {};
            vii.world = it->WorldMatrix;
            vii.prevWorld = it->HasValidPrevMatrix ? it->PrevWorldMatrix : it->WorldMatrix;
            vii.color = it->GroundColor;
            vii.windStrenth = 0.0f;
            vii.canBeAffectedByPlayer = 0;
            vii.GP_Slot |= playerFocusVob == it->Vob ? 1 << 31 : 0;
            // Bit 30: StaticVob flag, read by the D3D12 point-shadow static-VOB gather (CPU-only, no shader use).
            vii.GP_Slot |= it->Vob->GetFlags().StaticVob ? 1u << 30 : 0;

            zTAnimationMode aniMode = it->Vob->GetVisualAniMode();
            if ( aniMode != zVISUAL_ANIMODE_NONE ) {
                vii.canBeAffectedByPlayer = (!it->Vob->GetDynColl() ? 1.0f : 0.0f);
                ProcessVobAnimation( it->Vob, aniMode, vii );
            }
            reinterpret_cast<MeshVisualInfo*>(it->VisualInfo)->Instances.push_back( vii );
        }

        if ( renderQueue.transparent.size() ) {
            TransparencyVobs.insert( TransparencyVobs.end(),
                std::make_move_iterator(renderQueue.transparent.begin()),
                std::make_move_iterator(renderQueue.transparent.end()) );
            // ignore dead items in renderQueue.transparent after move-insert
            // No sort here anymore - the transparency queue orders ghosts against every other
            // blended drawable, not just against each other.
        }

        float minDynamicUpdateLightRange = Engine::GAPI->GetRendererState().RendererSettings.MinLightShadowUpdateRange;
    

        std::vector<std::pair<float, VobLightInfo*>> lightWithDist;
        lightWithDist.reserve( renderQueue.lights.size() );

        float lightPlayerDist;
        for ( auto vi : renderQueue.lights ) {
            if ( vi->Vob->IsEnabled() ) {
                XMStoreFloat( &lightPlayerDist, XMVector3LengthSq( playerPosition - vi->Vob->GetPositionWorldXM() ) ); 
                lightWithDist.emplace_back( lightPlayerDist, vi );
            }
        }

        std::ranges::sort(lightWithDist, []( const std::pair<float, VobLightInfo*>& a, const std::pair<float, VobLightInfo*>& b ) {
            return a.first < b.first;
        });

        renderQueue.lights.clear();

        const bool lightUpdateEnabled = RendererState.RendererSettings.EnablePointlightShadows >= GothicRendererSettings::PLS_UPDATE_DYNAMIC;
        const bool lightShadowsEnabled = RendererState.RendererSettings.EnablePointlightShadows >= GothicRendererSettings::PLS_STATIC_ONLY;
        for ( auto [distSq, vi] : lightWithDist ) {
            renderQueue.lights.push_back( vi );
            vi->VisibleInFrame = true;

            if ( lightUpdateEnabled ) {
                // things like candles MUST also draw shadow cubes, otherwise they shine through walls.
                const float lightRange = vi->Vob->GetLightRange();

                // force light update for static/pfx lights, well anyone who hasent managed to get its shadow at least once
                // otherwise we get ugly light bleeding from stuff like torches or spellFx
                if ( distSq < (lightRange * lightRange) )
                    vi->UpdateShadows = true;
            }
            // not else!
            if ( lightShadowsEnabled ) {
                // just always update static lights, they should be pretty cheap. We limit them anyways.
                if ( vi->IsStaticVobLight || !vi->IsPFXVobLight || vi->IsIndoorVob ) {
                    vi->UpdateShadows = true;
                }
            }

        }
    }
}

void GothicAPI::BuildWorldSectionBVH() {
    ClearWorldSectionBVH();

    std::vector<WorldSectionBVHBuildPrimitive> primitives;
    primitives.reserve( 4096 );

    for ( auto& [_, byY] : WorldSections ) {
        for ( auto& [__, section] : byY ) {
            if ( !IsValidSectionBounds( section.BoundingBox ) ) {
                continue;
            }

            WorldSectionBVHBuildPrimitive primitive;
            primitive.Section = &section;
            primitive.Bounds = Frustum::BBoxFromzTBBox3D( section.BoundingBox );
            primitive.Center = primitive.Bounds.Center;
            primitives.push_back( primitive );
        }
    }

    if ( primitives.empty() ) {
        return;
    }

    std::vector<uint32_t> primitiveIndices( primitives.size() );
    std::iota( primitiveIndices.begin(), primitiveIndices.end(), 0u );

    WorldSectionBVHNodes.reserve( primitives.size() * 2 );
    WorldSectionBVHSections.reserve( primitives.size() );

    auto buildRecursive = [&]( auto&& self, uint32_t begin, uint32_t end ) -> uint32_t {
        const uint32_t nodeIndex = static_cast<uint32_t>(WorldSectionBVHNodes.size());
        WorldSectionBVHNodes.emplace_back();
        auto& node = WorldSectionBVHNodes.back();

        DirectX::BoundingBox bounds = primitives[primitiveIndices[begin]].Bounds;
        XMFLOAT3 centroidMin = primitives[primitiveIndices[begin]].Center;
        XMFLOAT3 centroidMax = centroidMin;

        for ( uint32_t i = begin + 1; i < end; ++i ) {
            const auto& primitive = primitives[primitiveIndices[i]];
            bounds = MergeBoundingBoxes( bounds, primitive.Bounds );

            centroidMin.x = std::min( centroidMin.x, primitive.Center.x );
            centroidMin.y = std::min( centroidMin.y, primitive.Center.y );
            centroidMin.z = std::min( centroidMin.z, primitive.Center.z );
            centroidMax.x = std::max( centroidMax.x, primitive.Center.x );
            centroidMax.y = std::max( centroidMax.y, primitive.Center.y );
            centroidMax.z = std::max( centroidMax.z, primitive.Center.z );
        }

        node.Bounds = bounds;

        const uint32_t primitiveCount = end - begin;
        const XMFLOAT3 centroidExtent(
            centroidMax.x - centroidMin.x,
            centroidMax.y - centroidMin.y,
            centroidMax.z - centroidMin.z );

        int splitAxis = 0;
        float axisExtent = centroidExtent.x;
        if ( centroidExtent.y > axisExtent ) {
            splitAxis = 1;
            axisExtent = centroidExtent.y;
        }
        if ( centroidExtent.z > axisExtent ) {
            splitAxis = 2;
            axisExtent = centroidExtent.z;
        }

        if ( primitiveCount <= WORLD_SECTION_BVH_LEAF_SIZE || axisExtent <= 0.001f ) {
            node.LeafStart = static_cast<uint32_t>(WorldSectionBVHSections.size());
            node.LeafCount = primitiveCount;
            for ( uint32_t i = begin; i < end; ++i ) {
                WorldSectionBVHSections.push_back( primitives[primitiveIndices[i]].Section );
            }
            return nodeIndex;
        }

        const uint32_t splitIndex = begin + primitiveCount / 2;
        std::nth_element(
            primitiveIndices.begin() + begin,
            primitiveIndices.begin() + splitIndex,
            primitiveIndices.begin() + end,
            [&]( uint32_t a, uint32_t b ) {
                return GetAxisValue( primitives[a].Center, splitAxis )
                    < GetAxisValue( primitives[b].Center, splitAxis );
            } );

        node.LeftChild = self( self, begin, splitIndex );
        node.RightChild = self( self, splitIndex, end );
        return nodeIndex;
    };

    buildRecursive( buildRecursive, 0, static_cast<uint32_t>(primitiveIndices.size()) );
    WorldSectionBVHValid = !WorldSectionBVHNodes.empty();
}

void GothicAPI::ClearWorldSectionBVH() {
    WorldSectionBVHValid = false;
    WorldSectionBVHNodes.clear();
    WorldSectionBVHSections.clear();
}

bool GothicAPI::IsWorldMeshVisibleInFrustum( const WorldMeshInfo* mesh, const Frustum& frustum ) const {
    if ( !mesh ) {
        return false;
    }

    if ( !mesh->HasBoundingBox ) {
        return true;
    }

    return frustum.Intersects( mesh->BoundingBox );
}

void GothicAPI::QueryWorldSectionBVH( const Frustum& frustum,
    std::vector<WorldMeshSectionInfo*>& sections,
    bool useSectionRadiusFilter,
    const HorizonCuller* horizon ) const {
    if ( !WorldSectionBVHValid || WorldSectionBVHNodes.empty() ) {
        return;
    }

    static thread_local std::vector<uint32_t> nodeStack;
    nodeStack.clear();
    nodeStack.push_back( 0 );

    INT2 camSection = {};
    int sectionViewDist = 0;
    if ( useSectionRadiusFilter ) {
        camSection = WorldConverter::GetSectionOfPos( Engine::GAPI->GetCameraPosition() );
        sectionViewDist = Engine::GAPI->GetRendererState().RendererSettings.SectionDrawRadius;
    }

    while ( !nodeStack.empty() ) {
        const uint32_t nodeIndex = nodeStack.back();
        nodeStack.pop_back();

        const WorldSectionBVHNode& node = WorldSectionBVHNodes[nodeIndex];
        if ( !frustum.Intersects( node.Bounds ) ) {
            continue;
        }
        // Horizon on the BVH node itself: rejecting an interior node drops its whole subtree of
        // sections in one test, which is where this pays best.
        if ( horizon ) {
            const XMFLOAT3 nodeMin( node.Bounds.Center.x - node.Bounds.Extents.x,
                                    node.Bounds.Center.y - node.Bounds.Extents.y,
                                    node.Bounds.Center.z - node.Bounds.Extents.z );
            const XMFLOAT3 nodeMax( node.Bounds.Center.x + node.Bounds.Extents.x,
                                    node.Bounds.Center.y + node.Bounds.Extents.y,
                                    node.Bounds.Center.z + node.Bounds.Extents.z );
            if ( !horizon->IsBoxVisible( nodeMin, nodeMax ) ) {
                continue;
            }
        }

        if ( node.IsLeaf() ) {
            const uint32_t leafEnd = node.LeafStart + node.LeafCount;
            for ( uint32_t i = node.LeafStart; i < leafEnd; ++i ) {
                WorldMeshSectionInfo* section = WorldSectionBVHSections[i];
                if ( !section ) {
                    continue;
                }

                if ( useSectionRadiusFilter ) {
                    if ( abs( section->WorldCoordinates.x - camSection.x ) >= sectionViewDist ) {
                        continue;
                    }
                    if ( abs( section->WorldCoordinates.y - camSection.y ) >= sectionViewDist ) {
                        continue;
                    }
                }

                sections.push_back( section );
            }
        } else {
            nodeStack.push_back( node.LeftChild );
            nodeStack.push_back( node.RightChild );
        }
    }
}

bool GothicAPI::UseWorldSectionBVH() const {
    return RendererState.RendererSettings.DebugSettings.FeatureSet.UseWorldSectionBVH;
}

void GothicAPI::UpdateShouldBlockGameInput( ) {
    if ( auto hImgui = Engine::ImGuiHandle ) {
        auto oldIsActive = hImgui->IsActive;
        hImgui->IsActive = hImgui->SettingsVisible || hImgui->GetIsEditorVisible() || hImgui->AdvancedSettingsVisible || hImgui->LibShowBlockingThisFrame;
        hImgui->UpdateBlockGameInput();

        if ( oldIsActive != hImgui->IsActive ) {
            Engine::GAPI->SetEnableGothicInput( !hImgui->IsActive );
        }
    }
}
    
/** Collects visible sections from the current camera perspective */
void GothicAPI::CollectVisibleSections( std::vector<WorldMeshSectionInfo*>& sections,
    const Frustum* queryFrustum,
    bool useSectionRadiusFilter,
    const HorizonCuller* horizon ) {
    const XMFLOAT3 camPos = Engine::GAPI->GetCameraPosition();
    const INT2 camSection = WorldConverter::GetSectionOfPos( camPos );
    auto cullingEnabled = Engine::GAPI->GetRendererState().RendererSettings.DebugSettings.Culling.CullBspSections;
    auto drawSectionIntersections = Engine::GAPI->GetRendererState().RendererSettings.DrawSectionIntersections;

    auto sectionInFrustum = [&]( const WorldMeshSectionInfo& section ) {
        if ( !cullingEnabled ) {
            return true;
        }

        if ( queryFrustum ) {
            if ( !queryFrustum->IsValid() ) {
                return true;
            }
            if ( !queryFrustum->Intersects( section.BoundingBox ) ) return false;
        } else if ( GetCameraBBox3DInFrustum( section.BoundingBox, EGothicCullFlags::CullSidesNear ) == ZTCAM_CLIPTYPE_OUT ) {
            return false;
        }

        if ( horizon && !horizon->IsBoxVisible( section.BoundingBox.Min, section.BoundingBox.Max ) ) {
            return false;
        }
        return true;
    };

    const bool queryFrustumValid = queryFrustum == nullptr || queryFrustum->IsValid();
    if ( UseWorldSectionBVH() && WorldSectionBVHValid && cullingEnabled && queryFrustumValid
        && (queryFrustum || !drawSectionIntersections) ) {
        ZoneScopedN( "GothicAPI::CollectVisibleSections->BVH" );

        Frustum generatedFrustum;
        const Frustum* activeFrustum = queryFrustum;

        if ( !activeFrustum ) {
            if ( auto cam = (zCCamera*)oCGame::GetGame()->_zCSession_camera ) {
                const auto& view = cam->trafoView; // Column-Major, needs Transpose for DxMath
                const auto& proj = cam->trafoProjection; // Row-Major, does not need transpose.

                generatedFrustum.BuildPerspective(
                    XMMatrixTranspose( XMLoadFloat4x4( &view ) ),
                    XMLoadFloat4x4( &proj )
                );
            } else {
                generatedFrustum = Frustum::AlwaysContainingFrustum();
            }
            activeFrustum = &generatedFrustum;
        }

        QueryWorldSectionBVH( *activeFrustum, sections, useSectionRadiusFilter, horizon );
        return;
    }

    ZoneScopedN( "GothicAPI::CollectVisibleSections" );
    if ( drawSectionIntersections ) {
        if ( !useSectionRadiusFilter ) {
            for ( auto& [_, byY] : WorldSections ) {
                for ( auto& [__, section] : byY ) {
                    if ( sectionInFrustum( section ) ) {
                        sections.push_back( &section );
                    }
                }
            }
            return;
        }

        const float sectionViewDist = Engine::GAPI->GetRendererState().RendererSettings.SectionDrawRadius * WORLD_SECTION_SIZE;
        for ( auto& itx : WorldSections ) {
            for ( auto& ity : itx.second ) {
                WorldMeshSectionInfo& section = ity.second;

                float dist = Toolbox::ComputePointAABBDistance( camPos, section.BoundingBox.Min, section.BoundingBox.Max );
                if ( dist < sectionViewDist ) {
                    if ( !sectionInFrustum( section ) )
                        continue;

                    sections.push_back( &section );
                }
            }
        }
    } else {
        if ( !useSectionRadiusFilter ) {
            for ( auto& [_, byY] : WorldSections ) {
                for ( auto& [__, section] : byY ) {
                    if ( sectionInFrustum( section ) ) {
                        sections.push_back( &section );
                    }
                }
            }
            return;
        }

        // run through every section and check for range and frustum
        const int sectionViewDist = Engine::GAPI->GetRendererState().RendererSettings.SectionDrawRadius;
        for ( auto& itx : WorldSections ) {
            if ( abs( itx.first - camSection.x ) >= sectionViewDist ) {
                continue;
            }

            for ( auto& ity : itx.second ) {
                WorldMeshSectionInfo& section = ity.second;

                // Simple range-check
                if ( abs( ity.first - camSection.y ) < sectionViewDist ) {
                    if ( !sectionInFrustum( section ) )
                        continue;

                    sections.push_back( &section );
                }
            }
        }
    }
}

/** Moves the given vob from a BSP-Node to the dynamic vob list */
void GothicAPI::MoveVobFromBspToDynamic( SkeletalVobInfo* vob ) {
    auto& parentBspNodes = vob->ParentBSPNodes;
    for ( auto const& node : parentBspNodes ) {
        // Remove from possible lists
        for ( std::vector<SkeletalVobInfo*>::iterator it = node->Mobs.begin(); it != node->Mobs.end(); ++it ) {
            if ( (*it) == vob ) {
                (*it) = node->Mobs.back();
                node->Mobs.pop_back();
                break;
            }
        }
    }

    parentBspNodes.clear();

    AnimatedSkeletalVobs.push_back( vob );
}

/** Moves the given vob from a BSP-Node to the dynamic vob list */
void GothicAPI::MoveVobFromBspToDynamic( VobInfo* vob ) {
    // Remove from all nodes
    for ( size_t i = 0; i < vob->ParentBSPNodes.size(); i++ ) {
        BspInfo* node = vob->ParentBSPNodes[i];

        // Remove from possible lists
        EraseVobFromLeafList( node->IndoorVobs, vob );
        EraseVobFromLeafList( node->SmallVobs, vob );
        EraseVobFromLeafList( node->Vobs, vob );
    }
    vob->ParentBSPNodes.clear();

    // Add to dynamic vob list
    DynamicallyAddedVobs.push_back( vob );
}

std::vector<LeafVobEntry>::iterator GothicAPI::MoveVobFromBspToDynamic( VobInfo* vob, std::vector<LeafVobEntry>* source ) {
    std::vector<LeafVobEntry>::iterator itn = source->end();
    std::vector<LeafVobEntry>::iterator itc;

    // Remove from all nodes
    for ( size_t i = 0; i < vob->ParentBSPNodes.size(); i++ ) {
        BspInfo* node = vob->ParentBSPNodes[i];

        // Remove from possible lists
        for ( auto it = node->IndoorVobs.begin(); it != node->IndoorVobs.end(); ++it ) {
            if ( it->Info == vob ) {
                itc = node->IndoorVobs.erase( it );
                break;
            }
        }

        if ( &node->IndoorVobs == source )
            itn = itc;

        for ( auto it = node->SmallVobs.begin(); it != node->SmallVobs.end(); ++it ) {
            if ( it->Info == vob ) {
                itc = node->SmallVobs.erase( it );
                break;
            }
        }

        if ( &node->SmallVobs == source )
            itn = itc;

        for ( auto it = node->Vobs.begin(); it != node->Vobs.end(); ++it ) {
            if ( it->Info == vob ) {
                itc = node->Vobs.erase( it );
                break;
            }
        }

        if ( &node->Vobs == source )
            itn = itc;
    }

    // Add to dynamic vob list
    DynamicallyAddedVobs.push_back( vob );

    return itn;
}

static void CVVH_AddNotDrawnVobToList(
        FXMVECTOR distSq,
        std::vector<LeafVobEntry>& source,
        const RndCullContext& ctx,
        DirectX::ContainmentType bspContainment,
        BspTreeVobVisitor* visitor,
        // Non-null only for the indoor list of a portal-culled pass: the vob must additionally
        // reach the screen-space aperture its room is seen through (ScreenProjectionTouchesPortal).
        const BspInfo* portalLeaf = nullptr
    ) {
    const auto camPos = XMLoadFloat3( &ctx.cameraPosition );
    // SkipVobFrustumCull: the backend culls these on the GPU (D3D12), so collect distance-only.
    const bool cullingEnabled = ctx.drawFlags.CullVobs && !ctx.drawFlags.SkipVobFrustumCull;
    // Hoisted out of the loop: with a CONTAINS leaf the per-vob box test is provably redundant
    // (see CollectVisibleVobsWithLeafCache), so the whole branch collapses to a constant here.
    const bool needFrustumTest = cullingEnabled && bspContainment != ContainmentType::CONTAINS;
    const HorizonCuller* horizon = ctx.horizon;
    const float minVobSize = ctx.minVobSize;

    for ( const LeafVobEntry& entry : source ) {
        // Reject on distance FIRST, out of the list element's OWN mirrored position: every later step
        // dereferences a scattered VobInfo, and most candidates never survive to need one. The reject path
        // therefore touches nothing but the contiguous 16-byte entries.
        XMVECTOR vvdSq = XMVector3LengthSq( camPos - XMLoadFloat3( &entry.Position ) );
        if ( XMVector3Greater( vvdSq, distSq )) continue;

        VobInfo* it = entry.Info;
        if ( !visitor->Visit( it ) ) continue;

        // Caster size gate (shadow cascades only; minVobSize is 0 for every main-view pass). After Visit,
        // not before: MeshSize is leaf-independent but costs two pointer hops, so pay it once per vob per
        // pass rather than once per leaf.
        if ( minVobSize > 0.0f && it->VisualInfo && it->VisualInfo->MeshSize < minVobSize ) continue;

        const zTVobFlags vobFlags = it->Vob->GetFlags();
        if ( !vobFlags.ShowVisual ) continue;

        // LastRenderBBox rather than Vob->GetBBox(): same value, but it lives in VobInfo instead of
        // Gothic's heap, so the reject path stays off a second allocation entirely.
        if ( needFrustumTest && !ctx.frustum.Intersects( it->LastRenderBBox ) ) {
            continue;
        }
        // Horizon: hidden behind an occluder, so nothing below is built - no instance upload, no indirect
        // command, no CacheIn. After the frustum test, which is much cheaper.
        if ( horizon ) {
            const zTBBox3D& hb = it->LastRenderBBox;
            if ( !horizon->IsBoxVisible( hb.Min, hb.Max ) )
                continue;
        }
        if ( portalLeaf ) {
            const zTBBox3D& bb = it->LastRenderBBox;
            if ( !ctx.portalCuller->IsBoxVisibleInLeafSectors( *portalLeaf, bb.Min, bb.Max ) )
                continue;
        }
        if ( vobFlags.VisualAlphaEnabled ) {
            ctx.queue->PushTransparencyVob( TransparencyVobInfo{ std::sqrtf( XMVectorGetX( vvdSq ) ), it->Vob->GetVobTransparency(), nullptr, it } );
            continue;
        }

        ctx.queue->PushStaticVob( it );
    }
}

static void CVVH_AddNotDrawnVobToList(
    std::vector<SkeletalVobInfo*>& source,
    float distSq, const RndCullContext& ctx,
    DirectX::ContainmentType bspContainment,
    BspTreeVobVisitor* visitor) {
    const auto camPos = XMLoadFloat3( &ctx.cameraPosition );

    const bool cullingEnabled = ctx.drawFlags.CullVobs;
    const auto vDistSq = XMVectorReplicate( distSq );

    // Same hoist as the static-vob overload: a CONTAINS leaf makes the per-mob box test redundant.
    const bool needFrustumTest = cullingEnabled && bspContainment != ContainmentType::CONTAINS;

    for ( auto const& it : source ) {
        // Distance before Visit(): the test is leaf-independent, so an out-of-range mob fails it in every
        // leaf and marking it seen changes nothing. Keeps the atomic off the reject path.
        if ( XMVector3Greater( XMVector3LengthSq( camPos - it->Vob->GetPositionWorldXM() ), vDistSq ) ) {
            continue;
        }

        if ( !visitor->Visit( it ) ) continue;

        if ( !it->Vob->GetShowVisual() )
            continue;

        if ( needFrustumTest && !ctx.frustum.Intersects( it->Vob->GetBBox() ) ) {
            continue;
        }
        // Horizon: static MOBs draw per-mesh rather than indirect, so a rejection saves a whole draw plus
        // its material binds.
        if ( ctx.horizon ) {
            const zTBBox3D bb = it->Vob->GetBBox();
            if ( !ctx.horizon->IsBoxVisible( bb.Min, bb.Max ) )
                continue;
        }

        ctx.queue->PushSkeletalVob( it );
    }
}

/** Helper function for going through the bsp-tree */
void GothicAPI::BuildBspVobMapCacheHelper( zCBspBase* base ) {
    if ( !base )
        return;

    // Put it into the cache
    BspInfo& bvi = BspLeafVobLists[base];
    bvi.OriginalNode = base;

    bool outdoorLocation = (LoadedWorldInfo->BspTree->GetBspTreeMode() == zBSP_MODE_OUTDOOR);
    if ( base->IsLeaf() ) {
        zCBspLeaf* leaf = static_cast<zCBspLeaf*>(base);

        bvi.Front = nullptr;
        bvi.Back = nullptr;

        for ( int i = 0; i < leaf->LeafVobList.NumInArray; i++ ) {
            zCVob* vob = leaf->LeafVobList.Array[i];

            // Get the vob info for this one
            auto vit = VobMap.find( vob );
            if ( vit != VobMap.end() ) {
                VobInfo* v = vit->second;
                if ( v ) {
                    float vobSmallSize = Engine::GAPI->GetRendererState().RendererSettings.SmallVobSize;

                    // Position is read straight off the zCVob rather than from VobInfo::LastRenderPosition:
                    // this runs at world load and a VobInfo registered moments ago may not have had
                    // UpdateState() called on it yet. See LeafVobEntry for why the mirror stays valid.
                    const LeafVobEntry entry{ vob->GetPositionWorld(), v };
                    const auto sameVob = [v]( const LeafVobEntry& e ) { return e.Info == v; };

                    // Treat indoor vobs as indoor vobs only in outdoor locations
                    if ( outdoorLocation && vob->IsIndoorVob() ) {
                        // Only add once
                        if (std::ranges::find_if(bvi.IndoorVobs, sameVob ) == bvi.IndoorVobs.end() ) {
                            v->ParentBSPNodes.push_back( &bvi );
                            bvi.IndoorVobs.push_back( entry );
                            v->IsIndoorVob = true;
                        }
                    } else if ( v->VisualInfo->MeshSize < vobSmallSize ) {
                        // Only add once
                        if (std::ranges::find_if(bvi.SmallVobs, sameVob ) == bvi.SmallVobs.end() ) {
                            v->ParentBSPNodes.push_back( &bvi );
                            bvi.SmallVobs.push_back( entry );
                        }
                    } else {
                        // Only add once
                        if (std::ranges::find_if(bvi.Vobs, sameVob ) == bvi.Vobs.end() ) {
                            v->ParentBSPNodes.push_back( &bvi );
                            bvi.Vobs.push_back( entry );
                        }
                    }
                }
            }

            // Get mobs
            auto sit = SkeletalVobMap.find( vob );
            if ( sit != SkeletalVobMap.end() ) {
                SkeletalVobInfo* v = sit->second;
                if ( v ) {
                    // Only add once
                    if (std::ranges::find(bvi.Mobs, v ) == bvi.Mobs.end() ) {
                        v->ParentBSPNodes.push_back( &bvi );
                        bvi.Mobs.push_back( v );
                    }
                }
            }
        }

        // Mirrors zCBspLeaf::LightVobList as already-resolved VobLightInfo*, index for index, so
        // CollectLeafVobs needs no VobLightMap lookup per light per leaf. Validated there by a
        // pointer compare and repaired on the spot when the game adds/removes a light at runtime.
        bvi.Lights.clear();
        bvi.Lights.reserve( leaf->LightVobList.NumInArray );

        for ( int i = 0; i < leaf->LightVobList.NumInArray; i++ ) {
            zCVobLight* vob = leaf->LightVobList.Array[i];

            // Add the light to the map if not already done
            auto vit = VobLightMap.find( vob );
            if ( vit == VobLightMap.end() ) {
                VobLightInfo* vi = new VobLightInfo;
                vi->Vob = vob;
                VobLightMap[vob] = vi;
                if ( vob->IsIndoorVob() ) {
                    vi->IsIndoorVob = true;
                }

                if ( zCVob* parent = vob->GetVobParent(); parent ) {
                    if ( auto visFx = parent->As<oCVisualFX>() ) {
                        bool isPfx = true;
                        if (auto origin = visFx->GetOrigin()) {
                            // any PFX that stems from an ITEM should be counted as simple light.
                            isPfx = !origin->As<oCItem>();
                        }
                        vi->IsPFXVobLight = isPfx;
                    }
                }
                
                vi->IsStaticVobLight = vob->GetLightInfoFlags().isStatic;

                float minDynamicUpdateLightRange = Engine::GAPI->GetRendererState().RendererSettings.MinLightShadowUpdateRange;
                if ( RendererState.RendererSettings.EnablePointlightShadows >= GothicRendererSettings::PLS_STATIC_ONLY
                    && vi->Vob->GetLightRange() > minDynamicUpdateLightRange ) {
                    // Create shadowcubemap, if wanted
                    BaseShadowedPointLight* bpl = nullptr;
                    Engine::GraphicsEngine->CreateShadowedPointLight( &bpl, vi );
                    vi->LightShadowBuffers.reset(bpl);
                }


                bvi.Lights.push_back( vi );
            } else {
                bvi.Lights.push_back( vit->second );
            }
        }

        bvi.NumStaticLights = leaf->LightVobList.NumInArray;
    } else {
        zCBspNode* node = static_cast<zCBspNode*>(base);

        bvi.OriginalNode = base;

        BuildBspVobMapCacheHelper( node->Front );
        BuildBspVobMapCacheHelper( node->Back );

        // Save front and back to this
        bvi.Front = &BspLeafVobLists[node->Front];
        bvi.Back = &BspLeafVobLists[node->Back];
    }
}

/** Builds the flat leaf cache by DFS over the BspInfo mirror tree */
void BspLeafLinearCache::Build( BspInfo* root ) {
    std::vector<BspInfo*> stack;
    stack.reserve( 512 );
    stack.push_back( root );

    while ( !stack.empty() ) {
        BspInfo* base = stack.back();
        stack.pop_back();

        if ( !base || !base->OriginalNode )
            continue;

        if ( base->OriginalNode->IsLeaf() ) {
            const zTBBox3D& bb = base->OriginalNode->BBox3D;
            MinX.push_back( bb.Min.x );
            MinY.push_back( bb.Min.y );
            MinZ.push_back( bb.Min.z );
            MaxX.push_back( bb.Max.x );
            MaxY.push_back( bb.Max.y );
            MaxZ.push_back( bb.Max.z );
            Leaves.push_back( base );
        } else {
            if ( base->Front ) stack.push_back( base->Front );
            if ( base->Back )  stack.push_back( base->Back );
        }
    }

    Count = static_cast<uint32_t>( Leaves.size() );

    // Pad to the next multiple of 8 with sentinel values that always fail frustum and distance tests.
    // Padding Min with +FLT_MAX and Max with -FLT_MAX makes any AABB distance check return infinity
    // and the p-vertex dot product will be < 0 for any valid frustum plane.
    const uint32_t padded = (Count + 7u) & ~7u;
    MinX.resize( padded,  FLT_MAX ); MinY.resize( padded,  FLT_MAX ); MinZ.resize( padded,  FLT_MAX );
    MaxX.resize( padded, -FLT_MAX ); MaxY.resize( padded, -FLT_MAX ); MaxZ.resize( padded, -FLT_MAX );
    Leaves.resize( padded, nullptr );
}

void BspLeafLinearCache::Clear() {
    MinX.clear(); MinY.clear(); MinZ.clear();
    MaxX.clear(); MaxY.clear(); MaxZ.clear();
    Leaves.clear();
    Count = 0;
}

/** Builds our BspTreeVobMap */
void GothicAPI::BuildBspVobMapCache() {
    ZoneScopedN( "GothicAPI::BuildBspVobMapCache" );
    BuildBspVobMapCacheHelper( LoadedWorldInfo->BspTree->GetRootNode() );
    BuildBspLeafLinearCache();

    // Needs the BspInfo mirror tree above to exist - it tags the leafs with their sector ids.
    PortalCuller.SetEnabled( RendererState.RendererSettings.EnablePortalCulling );
    PortalCuller.SetNearSectorRadius( RendererState.RendererSettings.PortalCullingNearRadius );
    PortalCuller.BuildFromWorld( LoadedWorldInfo->BspTree );
}

void GothicAPI::BuildBspLeafLinearCache() {
    LeafLinearCache.Clear();
    BspInfo* root = &BspLeafVobLists[LoadedWorldInfo->BspTree->GetRootNode()];
    LeafLinearCache.Build( root );
    LogInfo() << "BspLeafLinearCache: " << LeafLinearCache.Count << " leaves indexed for SIMD culling";
}

/** Cleans empty BSPNodes */
void GothicAPI::CleanBSPNodes() {
    for ( auto&& it = BspLeafVobLists.begin(); it != BspLeafVobLists.end();) {
        if ( it->second.IsEmpty() ) {
            it = BspLeafVobLists.erase( it );
        } else {
            ++it;
        }
    }
}

/** Returns the new node from tha base node */
BspInfo* GothicAPI::GetNewBspNode( zCBspBase* base ) {
    return &BspLeafVobLists[base];
}

/** Sets/Gets the far-plane */
void GothicAPI::SetFarPlane( float value ) {
    auto cam = GetSceneCamera();    
    cam->SetFarPlane( value );
    cam->Activate();
}

float GothicAPI::GetFarPlane() {
    return GetSceneCamera()->GetFarPlane();
}

/** Sets/Gets the far-plane */
void GothicAPI::SetNearPlane( float value ) {
    LogWarn() << "SetNearPlane not implemented yet!";
}

float GothicAPI::GetNearPlane() {
    return GetSceneCamera()->GetNearPlane();
}

/** Get material by texture name */
zCMaterial* GothicAPI::GetMaterialByTextureName( const std::string& name ) {
    const std::string_view nameView = name;
    for ( auto const& it : LoadedMaterials ) {
        if ( it->GetTextureSingle() ) {
            const std::string_view tn = it->GetTextureSingle()->GetNameWithoutExtView();
            if ( Toolbox::EqualsIgnoreCase(nameView, tn ) )
                return it;
        }
    }

    return nullptr;
}

void GothicAPI::GetMaterialListByTextureName( const std::string& name, std::list<zCMaterial*>& list ) {
    const std::string_view nameView = name;
    for ( auto const& it : LoadedMaterials ) {
        if ( it->GetTextureSingle() ) {
            const std::string_view tn = it->GetTextureSingle()->GetNameWithoutExtView();
            if ( Toolbox::EqualsIgnoreCase(nameView, tn ) )
                list.push_back( it );
        }
    }
}

/** Returns the time since the last frame */
float GothicAPI::GetDeltaTime() {
    const zCTimer* timer = zCTimer::GetTimer();

    return timer->frameTimeFloat / 1000.0f;
}

/** Sets the current texture test bind mode status */
void GothicAPI::SetTextureTestBindMode( bool enable, const std::string& currentTexture ) {
    TextureTestBindMode = enable;

    if ( enable )
        BoundTestTexture = currentTexture;
}

/** If this returns true, the property holds the name of the currently bound texture. If that is the case, any MyDirectDrawSurfaces should not bind themselfes
to the pipeline, but rather check if there are additional textures to load */
bool GothicAPI::IsInTextureTestBindMode( std::string& currentBoundTexture ) {
    if ( TextureTestBindMode )
        currentBoundTexture = BoundTestTexture;

    return TextureTestBindMode;
}

/** Lets Gothic draw its sky */
void GothicAPI::DrawSkyGothicOriginal() {
    HookedFunctions::OriginalFunctions.original_zCWorldRender( oCGame::GetGame()->_zCSession_world, *GetSceneCamera() );
}

/** Reset's the material info that were previously gathered */
void GothicAPI::ResetMaterialInfo() {
    std::unique_lock lock( MaterialInfosMutex );
    MaterialInfos.clear();
}

static void FixUpMaterial( MaterialInfo::Buffer& buffer ) {
    if ( buffer.SpecularIntensity < 0.0f ) {
        // we abuse negative specular intensity to mark a pixel as "focused", thus materials must never have negative specular intensity.
        buffer.SpecularIntensity = 0.0f;
    }
}

MaterialInfo* GothicAPI::GetMaterialInfoFrom(void* any, std::string_view materialName) {
    // Hot path: shared lock, no allocation. Worker threads (mesh extraction) hit this concurrently with
    // the main thread's per-draw lookups, and a miss on either side rehashes the map.
    {
        std::shared_lock lock( MaterialInfosMutex );
        auto it = MaterialInfos.find( any );
        if ( it != MaterialInfos.end() ) {
            return it->second.get();
        }
    }

    // Miss. Build the entry outside the lock - LoadFromFile does file IO, which we don't want to hold
    // every other thread's lookups up for. Two threads racing on the same material both build one and
    // the loser's copy is simply dropped by try_emplace.
    auto info = std::make_unique<MaterialInfo>();
    if ( any ) {
        info->LoadFromFile( materialName );
        if ( materialName.contains( "FULLALPHA" ) ) {
            info->MaterialType = MaterialInfo::MT_FullAlpha;
        }
    }
    FixUpMaterial( info->buffer );

    std::unique_lock lock( MaterialInfosMutex );
    return MaterialInfos.try_emplace( any, std::move( info ) ).first->second.get();
}
    
MaterialInfo* GothicAPI::GetMaterialInfoFrom( zCMaterial* mat ) {
    const auto name = mat->GetNameView();
    if (!name.empty() && !name.contains( ':' )) {
        // colons are mosly used for poly <-> sector <-> texture mapping
        // such as S:ADANOS013_NW_PATHWAY_04
        // as such, we should not use the original name, but fall back to the texture
        return GetMaterialInfoFrom(mat, name);
    }

    // MaterialInfo only from the main texture.
    auto tex = mat->GetTextureSingle();
    if ( !tex ) {
        // unless its un-set...?
        tex = mat->GetAniTexture();
    }

    if (tex)
    {
        return GetMaterialInfoFrom( mat, tex->GetNameWithoutExtView() );
    }
    // maybe its not yet loaded.
    // store a dummy and maybe retry again on InitValues
    // shouldn't actually happen, but eh.
    return GetMaterialInfoFrom( mat, "MAT_DUMMY" ); 
}

/** Returns the loaded skeletal mesh vobs */
std::vector<SkeletalVobInfo*>& GothicAPI::GetSkeletalMeshVobs() {
    return SkeletalMeshVobs;
}

/** Returns the loaded skeletal mesh vobs */
std::vector<SkeletalVobInfo*>& GothicAPI::GetAnimatedSkeletalMeshVobs() {
    return AnimatedSkeletalVobs;
}

std::vector<VobInfo*>& GothicAPI::GetDynamicallyAddedVobs() {
    return DynamicallyAddedVobs;
}

std::vector<VobInfo*>& GothicAPI::GetDynamicMeshVobs() {
    return DynamicMeshVobs;
}
    
/** Returns a texture from the given surface */
zCTexture* GothicAPI::GetTextureBySurface( MyDirectDrawSurface7* surface ) {
    for ( auto const& it : LoadedMaterials ) {
        auto const texture = it->GetTextureSingle();
        if ( texture && texture->GetSurface() == surface )
            return texture;
    }

    return nullptr;
}

/** Resets all vob-stats drawn this frame */
void GothicAPI::ResetVobFrameStats( ) {
    for ( auto&& it : VobLightMap ) {
        it.second->VisibleInFrame = false;
    }
}

/** Sets the currently bound texture */
void GothicAPI::SetBoundTexture( int idx, zCTexture* tex ) {
    BoundTextures[idx] = tex;
}

zCTexture* GothicAPI::GetBoundTexture( int idx ) {
    return BoundTextures[idx];
}

/** Texture currently being cached in on this thread. Thread-local on purpose - see ScopedLoadingTexture. */
static thread_local zCTexture* s_LoadingTexture = nullptr;

void GothicAPI::SetLoadingTexture( zCTexture* tex ) {
    s_LoadingTexture = tex;
}

zCTexture* GothicAPI::GetLoadingTexture() {
    return s_LoadingTexture;
}

/** Teleports the player to the given location */
void GothicAPI::SetPlayerPosition( const XMFLOAT3& pos ) {
    if ( oCGame::GetPlayer() )
        oCGame::GetPlayer()->ResetPos( pos );
}

/** Returns the player-vob */
zCVob* GothicAPI::GetPlayerVob() {
    return oCGame::GetPlayer();
}

/** Loads resources created for this .ZEN */
void GothicAPI::LoadCustomZENResources() {
    auto gameName = GetGameName();
    std::string zenFolder; zenFolder.reserve(255);
    if ( gameName == "Original" ) {
        zenFolder.append("system\\GD3D11\\ZENResources\\");
    } else {
        zenFolder.append("system\\GD3D11\\ZENResources\\").append(gameName).append("\\");
    }
    if ( !Toolbox::FolderExists( zenFolder ) ) {
        LogInfo() << "Custom ZEN-Resources. Directory not found: " << zenFolder;
        return;
    }

    std::string& zen = zenFolder.append(LoadedWorldInfo->WorldName);

    LogInfo() << "Loading custom ZEN-Resources from: " << zen;

    // Suppressed Textures
    LoadSuppressedTextures( zen + ".spt" );

    // Load vegetation
    LoadVegetation( zen + ".veg" );
}

/** Saves resources created for this .ZEN */
void GothicAPI::SaveCustomZENResources() {
    auto gameName = GetGameName();
    std::string zenFolder; zenFolder.reserve(255);
    if ( gameName == "Original" ) {
        zenFolder.append(R"(system\GD3D11\ZENResources\)");
    } else {
        zenFolder.append(R"(system\GD3D11\ZENResources\)").append(gameName).append("\\");
    }

    bool mkDirErr = false;
    if ( !Toolbox::FolderExists( zenFolder ) ) {
        mkDirErr = !Toolbox::CreateDirectoryRecursive( zenFolder );
    }

    if ( mkDirErr ) {
        LogError() << "Could not save custom ZEN-Resources. Could not create directory: " << zenFolder;
        return;
    }

    std::string& zen = zenFolder.append(LoadedWorldInfo->WorldName);

    LogInfo() << "Saving custom ZEN-Resources to: " << zen;

    // Suppressed Textures
    SaveSuppressedTextures( zen + ".spt" );

    // Save vegetation
    SaveVegetation( zen + ".veg" );
}

/** Applys the suppressed textures */
void GothicAPI::ApplySuppressedSectionTextures() {
    for ( auto const& it : SuppressedTexturesBySection ) {
        WorldMeshSectionInfo* section = it.first;

        // Look into each mesh of this section and find the texture
        for ( auto mit = section->WorldMeshes.begin(); mit != section->WorldMeshes.end(); ) {
            bool movedToSuppressed = false;
            if (auto mat = mit->first.Material ) {
                if ( auto tx = mat->GetTextureSingle()) {
                    auto txName = tx->GetNameWithoutExtView();
                    for (const auto& i : it.second) {
                        // Is this the texture we are looking for?
                        if ( txName == i) {
                            // Yes, move it to the suppressed map
                            section->SuppressedMeshes[mit->first] = mit->second;
                            mit = section->WorldMeshes.erase( mit );
                            movedToSuppressed = true;
                            break;
                        }
                    }
                }
            }

            if ( !movedToSuppressed ) {
                ++mit;
            }
        }
    }
}

/** Resets the suppressed textures */
void GothicAPI::ResetSupressedTextures() {
    for ( auto const& it : SuppressedTexturesBySection ) {
        WorldMeshSectionInfo* section = it.first;

        // Look into each mesh of this section and find the texture
        for ( auto const& mit : section->WorldMeshes ) {
            section->WorldMeshes[mit.first] = mit.second;
        }
    }

    SuppressedTexturesBySection.clear();
}

/** Resets the vegetation */
void GothicAPI::ResetVegetation() {
    for ( auto&& it : VegetationBoxes ) {
        delete it;
    }
    VegetationBoxes.clear();
}


/** Removes the given texture from the given section and stores the supression, so we can load it next time */
void GothicAPI::SupressTexture( WorldMeshSectionInfo* section, const std::string& texture ) {
    SuppressedTexturesBySection[section].push_back( texture );

    ApplySuppressedSectionTextures(); // This is an editor only feature, so it's okay to "not be blazing fast"
}

/** Saves Suppressed textures to a file */
XRESULT GothicAPI::SaveSuppressedTextures( const std::string& file ) {
    FILE* f = fopen( file.c_str(), "wb" );

    LogInfo() << "Saving suppressed textures";

    if ( !f )
        return XR_FAILED;

    int version = 1;
    fwrite( &version, sizeof( version ), 1, f );

    size_t count = SuppressedTexturesBySection.size();
    fwrite( &count, sizeof( count ), 1, f );

    for ( auto const& it : SuppressedTexturesBySection ) {
        // Write section xy-coords
        fwrite( &it.first->WorldCoordinates, sizeof( INT2 ), 1, f );

        size_t countTX = it.second.size();
        fwrite( &countTX, sizeof( countTX ), 1, f );

        for ( size_t i = 0; i < countTX; i++ ) {
            size_t numChars = std::min<size_t>( 255, it.second[i].size() );

            // Write num of chars
            fwrite( &numChars, sizeof( numChars ), 1, f );

            // Write chars
            fwrite( &it.second[0], numChars, 1, f );
        }
    }

    fclose( f );

    return XR_SUCCESS;
}

/** Saves Suppressed textures to a file */
XRESULT GothicAPI::LoadSuppressedTextures( const std::string& file ) {
    FILE* f = fopen( file.c_str(), "rb" );

    LogInfo() << "Loading Suppressed textures";

    // Clean first
    ResetSupressedTextures();

    if ( !f )
        return XR_FAILED;

    int version;
    fread( &version, sizeof( version ), 1, f );

    size_t count;
    fread( &count, sizeof( count ), 1, f );


    for ( size_t c = 0; c < count; c++ ) {
        size_t countTX;
        fread( &countTX, sizeof( countTX ), 1, f );

        for ( size_t i = 0; i < countTX; i++ ) {
            // Read section xy-coords
            INT2 coords;
            fread( &coords, sizeof( INT2 ), 1, f );

            // Read num of chars
            size_t numChars;
            fread( &numChars, sizeof( numChars ), 1, f );

            // Read chars
            char name[256] = {};
            if ( numChars > 0 ) {
                if ( numChars > 255 ) {
                    fread( name, 255, 1, f );
                    fseek( f, static_cast<long>(numChars - 255), SEEK_CUR );
                } else {
                    fread( name, numChars, 1, f );
                }
            }

            // Add to map
            SuppressedTexturesBySection[&WorldSections[coords.x][coords.y]].push_back( std::string( name ) );
        }
    }

    fclose( f );

    // Apply the whole thing
    ApplySuppressedSectionTextures();

    return XR_SUCCESS;
}

/** Saves vegetation to a file */
XRESULT GothicAPI::SaveVegetation( const std::string& file ) {
    FILE* f = fopen( file.c_str(), "wb" );

    LogInfo() << "Saving vegetation";

    if ( !f )
        return XR_FAILED;

    int version = 1;
    fwrite( &version, sizeof( version ), 1, f );

    size_t num = VegetationBoxes.size();
    fwrite( &num, sizeof( num ), 1, f );

    for ( auto const& it : VegetationBoxes ) {
        it->SaveToFILE( f, version );
    }

    fclose( f );

    return XR_SUCCESS;
}

/** Saves vegetation to a file */
XRESULT GothicAPI::LoadVegetation( const std::string& file ) {
    LogInfo() << "Loading vegetation";

    // Reset first
    ResetVegetation();

    zFILE_VDFS::Ptr vdfsFile;
    if ( std::filesystem::path( file ).is_absolute() ) {
        vdfsFile = zFILE_VDFS::Create( file.c_str() );
    } else if ( !file.empty() && file[0] != '\\' ) {
        vdfsFile = zFILE_VDFS::Create( ("\\"+ file).c_str());
    } else {
        vdfsFile = zFILE_VDFS::Create( file.c_str() );
    }

    if ( !vdfsFile->Exists() || vdfsFile->Open( false ) != zERROR_NONE ) {
        return XR_FAILED;
    }

    int version;
    vdfsFile->Read( &version, sizeof( version ) );

    size_t num = VegetationBoxes.size();
    vdfsFile->Read( &num, sizeof( num ) );

    for ( size_t i = 0; i < num; i++ ) {
        GVegetationBox* b = new GVegetationBox;
        b->LoadFromFILE( vdfsFile.get(), version );

        AddVegetationBox( b );
    }

    vdfsFile->Close();

    return XR_SUCCESS;
}

/** Saves the users settings from the menu */
XRESULT GothicAPI::SaveMenuSettings( const std::string& file ) {
    TCHAR NPath[MAX_PATH];
    // Returns Gothic directory.
    int len = GetCurrentDirectory( MAX_PATH, NPath );
    // Get path to Gothic.Ini
    auto ini = std::string( NPath, len ).append( "\\" + file );

    LogInfo() << "Saving menu settings to " << ini;
    GothicRendererSettings& s = RendererState.RendererSettings;

    WritePrivateProfileStringA( "General", "ChangeToMode", to_string_locale_independent( s.ChangeWindowPreset ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "General", "AtmosphericScattering", to_string_locale_independent( s.AtmosphericScattering ? TRUE : FALSE ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "General", "EnableFog", to_string_locale_independent( s.DrawFog ? TRUE : FALSE ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "General", "FogRange", float_to_string( s.FogRange , 2).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "General", "EnableHDR", to_string_locale_independent( s.EnableHDR ? TRUE : FALSE ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "General", "HDRToneMap", to_string_locale_independent( s.HDRToneMap ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "General", "EnableBloom", to_string_locale_independent( s.EnableBloom ? TRUE : FALSE ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "General", "Exposure", float_to_string( s.Exposure, 2 ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "General", "AutoExposureMiddleGray", to_string_locale_independent( s.AutoExposureMiddleGray ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "General", "AutoExposureStrength", to_string_locale_independent( s.AutoExposureStrength ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "General", "AutoExposureMin", to_string_locale_independent( s.AutoExposureMin ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "General", "AutoExposureMax", to_string_locale_independent( s.AutoExposureMax ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "General", "AutoExposureSpeed", to_string_locale_independent( s.AutoExposureSpeed ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "General", "BloomThreshold", float_to_string( s.BloomThreshold, 2 ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "General", "BloomStrength", float_to_string( s.BloomStrength, 2 ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "General", "BloomKnee", float_to_string( s.BloomKnee, 2 ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "General", "BloomRadius", float_to_string( s.BloomRadius, 2 ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "General", "DefaultMaterialRoughness", float_to_string( s.DefaultMaterialRoughness, 2 ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "General", "EnableDebugLog", to_string_locale_independent( s.EnableDebugLog ? TRUE : FALSE ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "General", "EnableAutoupdates", to_string_locale_independent( s.EnableAutoupdates ? TRUE : FALSE ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "General", "EnableGodRays", to_string_locale_independent( s.EnableGodRays ? TRUE : FALSE ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "General", "EnableDoF", to_string_locale_independent( s.EnableDoF ? TRUE : FALSE ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "General", "DoFGaussBlur", to_string_locale_independent( s.DoFGaussBlur ? TRUE : FALSE ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "General", "DoFFocusDistance", float_to_string( s.DoFFocusDistance, 1 ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "General", "DoFFocusRange", float_to_string( s.DoFFocusRange, 1 ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "General", "DoFBokehRadius", float_to_string( s.DoFBokehRadius, 1 ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "General", "DoFMaxBlur", float_to_string( s.DoFMaxBlur, 1 ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "General", "AllowNormalmaps", to_string_locale_independent( s.AllowNormalmaps ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "General", "AllowNumpadKeys", to_string_locale_independent( s.AllowNumpadKeys ? TRUE : FALSE ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "General", "EnableInactiveFpsLock", to_string_locale_independent( s.EnableInactiveFpsLock ? TRUE : FALSE ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "General", "MultiThreadResourceManager", to_string_locale_independent( s.MTResoureceManager ? TRUE : FALSE ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "General", "CompressBackBuffer", to_string_locale_independent( s.CompressBackBuffer ? TRUE : FALSE ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "General", "AnimateStaticVobs", to_string_locale_independent( s.AnimateStaticVobs ? TRUE : FALSE ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "General", "DrawWorldSectionIntersections", to_string_locale_independent( s.DrawSectionIntersections ? TRUE : FALSE ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "General", "DrawWorldOccluders", to_string_locale_independent( s.DrawWorldOccluders ? TRUE : FALSE ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "General", "SunLightStrength", to_string_locale_independent( s.SunLightStrength ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "General", "SortedTransparency", to_string_locale_independent( s.SortedTransparency ? TRUE : FALSE ).c_str(), ini.c_str() );
#ifdef BUILD_GOTHIC_1_08k
    WritePrivateProfileStringA( "General", "DrawG1ForestPortals", to_string_locale_independent( s.DrawG1ForestPortals ? TRUE : FALSE ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "General", "G1HighlightInteractiveFocus", to_string_locale_independent( s.G1HighlightInteractiveFocus ? TRUE : FALSE ).c_str(), ini.c_str() );
#endif
    WritePrivateProfileStringA( "General", "DrawRainThroughTransformFeedback", to_string_locale_independent( s.DrawRainThroughTransformFeedback ? TRUE : FALSE ).c_str(), ini.c_str() );

    /*
    * Draw-distance is saved on a per World basis using SaveRendererWorldSettings
    */

    WritePrivateProfileStringA( "General", "EnableOcclusionCulling", to_string_locale_independent( s.EnableOcclusionCulling ? TRUE : FALSE ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "General", "EnablePortalCulling", to_string_locale_independent( s.EnablePortalCulling ? TRUE : FALSE ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "General", "PortalCullingNearRadius", float_to_string( s.PortalCullingNearRadius, 1 ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "General", "EnablePortalShadowSkip", to_string_locale_independent( s.EnablePortalShadowSkip ? TRUE : FALSE ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "General", "EnableHorizonCulling", to_string_locale_independent( s.EnableHorizonCulling ? TRUE : FALSE ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "General", "EnableMeshOptimization", to_string_locale_independent( s.EnableMeshOptimization ? TRUE : FALSE ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "General", "EnableShadowIndexBuffers", to_string_locale_independent( s.EnableShadowIndexBuffers ? TRUE : FALSE ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "General", "FpsLimit", to_string_locale_independent( s.FpsLimit ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "General", "PausedFpsLimit", to_string_locale_independent( s.PausedFpsLimit ).c_str(), ini.c_str() );
    
    auto res = Engine::GraphicsEngine->GetBackbufferResolution();
    WritePrivateProfileStringA( "Display", "TextureQuality", to_string_locale_independent( s.textureMaxSize ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "Display", "Width", to_string_locale_independent( res.x ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "Display", "Height", to_string_locale_independent( res.y ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "Display", "ResolutionScale", to_string_locale_independent( s.ResolutionScalePercent ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "Display", "Upscaler", to_string_locale_independent( static_cast<int>(s.Upscaler) ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "Display", "VSync", to_string_locale_independent( s.EnableVSync ? TRUE : FALSE ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "Display", "ForceFOV", to_string_locale_independent( s.ForceFOV ? TRUE : FALSE ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "Display", "FOVHoriz", to_string_locale_independent( static_cast<int>(s.FOVHoriz) ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "Display", "FOVVert", to_string_locale_independent( static_cast<int>(s.FOVVert) ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "Display", "Gamma", to_string_locale_independent( s.GammaValue ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "Display", "Brightness", to_string_locale_independent( s.BrightnessValue ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "Display", "DisplayFlip", to_string_locale_independent( s.DisplayFlip ? TRUE : FALSE ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "Display", "LowLatency", to_string_locale_independent( s.LowLatency ? TRUE : FALSE ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "Display", "HDR_Monitor", to_string_locale_independent( s.HDR_Monitor ? TRUE : FALSE ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "Display", "HDR_AutoMaxBrightness", to_string_locale_independent( s.HDR_AutoMaxBrightness ? TRUE : FALSE ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "Display", "HDR_MaxBrightness", float_to_string( s.HDR_MaxBrightness, 1 ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "Display", "HDR_PaperWhite", float_to_string( s.HDR_PaperWhite, 1 ).c_str(), ini.c_str() );

    WritePrivateProfileStringA( "Display", "StretchWindow", to_string_locale_independent( s.StretchWindow ? TRUE : FALSE ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "Display", "UIScale", to_string_locale_independent( s.GothicUIScale ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "Display", "Rain", to_string_locale_independent( s.EnableRain ? TRUE : FALSE ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "Display", "RainEffects", to_string_locale_independent( s.EnableRainEffects ? TRUE : FALSE ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "Display", "LimitLightIntesity", to_string_locale_independent( s.LimitLightIntesity ? TRUE : FALSE ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "Display", "TiledLighting", to_string_locale_independent( s.EnableTiledLighting ? TRUE : FALSE ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "Display", "RendererMode", to_string_locale_independent( static_cast<int>(s.RendererMode) ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "Display", "GraphicsAPI", s.GraphicsAPI == GothicRendererSettings::GRAPHICS_API_D3D12 ? "D3D12" : "D3D11", ini.c_str() );
    WritePrivateProfileStringA( "Display", "MSAASamples", to_string_locale_independent( s.MSAASamples ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "Display", "WindQuality", to_string_locale_independent( s.WindQuality ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "Display", "WindStrength", to_string_locale_independent( s.GlobalWindStrength ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "Display", "WaterWaveAnimation", to_string_locale_independent( s.EnableWaterAnimation ? TRUE : FALSE ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "Display", "WaterSSRQuality", to_string_locale_independent( (int)s.WaterSSRQuality ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "Display", "WaterReflectionMode", to_string_locale_independent( (int)s.WaterReflectionMode ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "Display", "HeroAffectsObjects", to_string_locale_independent( s.HeroAffectsObjects ? TRUE : FALSE ).c_str(), ini.c_str() );
    

    WritePrivateProfileStringA( "Shadows", "EnableShadows", to_string_locale_independent( s.EnableShadows ? TRUE : FALSE ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "Shadows", "ShadowFilterMode", to_string_locale_independent( static_cast<int>(s.ShadowFilterMode) ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "Shadows", "ShadowMapSize", to_string_locale_independent( s.ShadowMapSize ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "Shadows", "WorldShadowRangeScale", to_string_locale_independent( s.WorldShadowRangeScale ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "Shadows", "NumShadowCascades", to_string_locale_independent( s.NumShadowCascades ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "Shadows", "ShadowCascadePCFLimit", to_string_locale_independent( s.ShadowCascadePCFLimit ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "Shadows", "ShadowFrustumCullingMode", to_string_locale_independent( static_cast<int>(s.ShadowFrustumCullingMode) ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "Shadows", "PointlightShadows", to_string_locale_independent( s.EnablePointlightShadows ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "Shadows", "EnableDynamicLighting", to_string_locale_independent( s.EnableDynamicLighting ? TRUE : FALSE ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "Shadows", "SmoothCameraUpdate", to_string_locale_independent( s.SmoothShadowCameraUpdate ? TRUE : FALSE ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "Shadows", "SmoothShadowFrequency", to_string_locale_independent( s.SmoothShadowFrequency ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "Shadows", "ShadowStrength", to_string_locale_independent( s.ShadowStrength ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "Shadows", "ShadowSoftness", to_string_locale_independent( s.ShadowSoftness ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "Shadows", "ShadowAOStrength", to_string_locale_independent( s.ShadowAOStrength ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "Shadows", "WorldAOStrength", to_string_locale_independent( s.WorldAOStrength ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "Shadows", "SkyIblIntensity", to_string_locale_independent( s.SkyIblIntensity ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "Shadows", "SkyOcclusionStrength", to_string_locale_independent( s.SkyOcclusionStrength ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "Shadows", "SkyIblNightFloor", to_string_locale_independent( s.SkyIblNightFloor ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "Shadows", "ShadowDepthSlopeBias", to_string_locale_independent( s.DebugSettings.ShadowCascades.ShadowDepthSlopeBias ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "Shadows", "FirstLodCascade", to_string_locale_independent( s.DebugSettings.ShadowCascades.FirstLodCascade ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "Shadows", "CasterMinTexels", to_string_locale_independent( s.DebugSettings.ShadowCascades.CasterMinTexels ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "Shadows", "AllowSelfShadowingPointlights", to_string_locale_independent( s.AllowSelfShadowingPointlights ? TRUE : FALSE ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "Shadows", "DisableStaticPointlights", to_string_locale_independent( s.DisableStaticPointlights ? TRUE : FALSE ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "Shadows", "SpecularHighlightsFlags", to_string_locale_independent( s.SpecularHighlightsFlags ).c_str(), ini.c_str() );

    // WritePrivateProfileStringA( "SMAA", "Enabled", to_string_locale_independent( s.EnableSMAA ? TRUE : FALSE ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "General", "AntiAliasing", to_string_locale_independent( (int)s.AntiAliasingMode ).c_str(), ini.c_str() );

    WritePrivateProfileStringA( "SMAA", "SharpenFactor", to_string_locale_independent( s.SharpenFactor ).c_str(), ini.c_str() );

    WritePrivateProfileStringA( "HBAO", "Enabled", to_string_locale_independent( s.HbaoSettings.Enabled ? TRUE : FALSE ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "HBAO", "Bias", to_string_locale_independent( s.HbaoSettings.Bias ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "HBAO", "Radius", to_string_locale_independent( s.HbaoSettings.Radius ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "HBAO", "PowerExponent", to_string_locale_independent( s.HbaoSettings.PowerExponent ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "HBAO", "BlurSharpness", to_string_locale_independent( s.HbaoSettings.BlurSharpness ).c_str(), ini.c_str() );
    //WritePrivateProfileStringA( "HBAO", "EnableDualLayerAO", to_string_locale_independent( s.HbaoSettings.EnableDualLayerAO ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "HBAO", "EnableBlur", to_string_locale_independent( s.HbaoSettings.EnableBlur ? TRUE : FALSE ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "HBAO", "SsaoBlurRadius", to_string_locale_independent( s.HbaoSettings.SsaoBlurRadius ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "HBAO", "SsaoStepCount", to_string_locale_independent( s.HbaoSettings.SsaoStepCount ).c_str(), ini.c_str() );

    WritePrivateProfileStringA( "AO", "Mode", to_string_locale_independent( static_cast<int>(s.AoMode) ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "SAO", "Radius", to_string_locale_independent( s.SaoSettings.Radius ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "SAO", "Bias", to_string_locale_independent( s.SaoSettings.Bias ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "SAO", "Intensity", to_string_locale_independent( s.SaoSettings.Intensity ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "SAO", "NumSamples", to_string_locale_independent( s.SaoSettings.NumSamples ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "SAO", "BlurSharpness", to_string_locale_independent( s.SaoSettings.BlurSharpness ).c_str(), ini.c_str() );

    // XeGTAO (D3D12's AO_ASSAO implementation)
    WritePrivateProfileStringA( "GTAO", "QualityLevel", to_string_locale_independent( s.GtaoSettings.QualityLevel ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "GTAO", "DenoisePasses", to_string_locale_independent( s.GtaoSettings.DenoisePasses ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "GTAO", "Radius", to_string_locale_independent( s.GtaoSettings.Radius ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "GTAO", "RadiusMultiplier", to_string_locale_independent( s.GtaoSettings.RadiusMultiplier ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "GTAO", "FalloffRange", to_string_locale_independent( s.GtaoSettings.FalloffRange ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "GTAO", "SampleDistributionPower", to_string_locale_independent( s.GtaoSettings.SampleDistributionPower ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "GTAO", "ThinOccluderCompensation", to_string_locale_independent( s.GtaoSettings.ThinOccluderCompensation ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "GTAO", "FinalValuePower", to_string_locale_independent( s.GtaoSettings.FinalValuePower ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "GTAO", "DepthMIPSamplingOffset", to_string_locale_independent( s.GtaoSettings.DepthMIPSamplingOffset ).c_str(), ini.c_str() );

    WritePrivateProfileStringA( "FontRendering", "Enable", to_string_locale_independent( s.EnableCustomFontRendering ? TRUE : FALSE ).c_str(), ini.c_str() );

    WritePrivateProfileStringA( "Inventory", "FastInventoryRendering", to_string_locale_independent( s.FastInventoryRendering ? TRUE : FALSE ).c_str(), ini.c_str() );

    WritePrivateProfileStringA( "Debug", "ThreadedShadowCulling", to_string_locale_independent( s.ThreadedShadowCulling ? TRUE : FALSE ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "Debug", "GpuVobCulling", to_string_locale_independent( s.GpuVobCulling ? TRUE : FALSE ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "Debug", "GpuVobOcclusionCulling", to_string_locale_independent( s.GpuVobOcclusionCulling ? TRUE : FALSE ).c_str(), ini.c_str() );
    // Persisted because it is not a live toggle: MorphGpu::IsActive() freezes it at load (it decides how the
    // morph vertex buffers get created), so the only way to turn it off is for the NEXT run.
    WritePrivateProfileStringA( "Debug", "GpuMorphFold", to_string_locale_independent( s.UseGpuMorphFold ? TRUE : FALSE ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "Debug", "UseShadowAtlas", to_string_locale_independent( s.DebugSettings.FeatureSet.UseShadowAtlas ? TRUE : FALSE ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "Debug", "UseScreenSpaceShadowMask", to_string_locale_independent( s.DebugSettings.FeatureSet.UseScreenSpaceShadowMask ? TRUE : FALSE ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "Debug", "GenerateAONormalsFromDepth", to_string_locale_independent( s.DebugSettings.FeatureSet.GenerateAONormalsFromDepth ? TRUE : FALSE ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "Debug", "ForceFeatureLevel10", to_string_locale_independent( s.DebugSettings.FeatureSet.ForceFeatureLevel10 ? TRUE : FALSE ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "Debug", "EnableDriverExtensions", to_string_locale_independent( s.DebugSettings.FeatureSet.EnableDriverExtensions ? TRUE : FALSE ).c_str(), ini.c_str() );

    return XR_SUCCESS;
}

/** Loads the users settings from the menu */
XRESULT GothicAPI::LoadMenuSettings( const std::string& file ) {
    TCHAR NPath[MAX_PATH];
    // Returns Gothic directory.
    int len = GetCurrentDirectory( MAX_PATH, NPath );
    // Get path to Gothic.Ini
    auto ini = std::string( NPath, len ).append( "\\" + file );

    GothicRendererSettings& s = RendererState.RendererSettings;
    if ( Toolbox::FileExists( ini ) ) {
        LogInfo() << "Loading menu settings from " << ini;
    
        GothicRendererSettings defaultRendererSettings{};
        defaultRendererSettings.SetDefault();
        const GothicRendererSettings& ds = defaultRendererSettings;

        s.ChangeWindowPreset = GetPrivateProfileIntA( "General", "ChangeToMode", 0, ini.c_str() );
        s.DrawFog = GetPrivateProfileBoolA( "General", "EnableFog", ds.DrawFog, ini );
        s.FogRange = GetPrivateProfileFloatA( "General", "FogRange", ds.FogRange, ini.c_str() );
        s.AtmosphericScattering = GetPrivateProfileBoolA( "General", "AtmosphericScattering", ds.AtmosphericScattering, ini );
        s.EnableHDR = GetPrivateProfileBoolA( "General", "EnableHDR", ds.EnableHDR, ini );
        s.HDRToneMap = GothicRendererSettings::E_HDRToneMap( GetPrivateProfileIntA( "General", "HDRToneMap", ds.HDRToneMap, ini.c_str() ) );
        s.EnableBloom = GetPrivateProfileBoolA( "General", "EnableBloom", ds.EnableBloom, ini );
        s.Exposure = GetPrivateProfileFloatA( "General", "Exposure", ds.Exposure, ini );
        s.AutoExposureMiddleGray = GetPrivateProfileFloatA( "General", "AutoExposureMiddleGray", ds.AutoExposureMiddleGray, ini );
        s.AutoExposureStrength = GetPrivateProfileFloatA( "General", "AutoExposureStrength", ds.AutoExposureStrength, ini );
        s.AutoExposureMin = GetPrivateProfileFloatA( "General", "AutoExposureMin", ds.AutoExposureMin, ini );
        s.AutoExposureMax = GetPrivateProfileFloatA( "General", "AutoExposureMax", ds.AutoExposureMax, ini );
        s.AutoExposureSpeed = GetPrivateProfileFloatA( "General", "AutoExposureSpeed", ds.AutoExposureSpeed, ini );
        s.BloomThreshold = GetPrivateProfileFloatA( "General", "BloomThreshold", ds.BloomThreshold, ini );
        s.BloomStrength = GetPrivateProfileFloatA( "General", "BloomStrength", ds.BloomStrength, ini );
        s.BloomKnee = GetPrivateProfileFloatA( "General", "BloomKnee", ds.BloomKnee, ini );
        s.BloomRadius = GetPrivateProfileFloatA( "General", "BloomRadius", ds.BloomRadius, ini );
        // Snap to a step the D3D12 backend actually built a 1x1 ORM texture for — the ini is hand-editable.
        s.DefaultMaterialRoughness = DefaultRoughness::ForStep( DefaultRoughness::StepFor(
            GetPrivateProfileFloatA( "General", "DefaultMaterialRoughness", ds.DefaultMaterialRoughness, ini ) ) );
        s.EnableDebugLog = GetPrivateProfileBoolA( "General", "EnableDebugLog", ds.EnableDebugLog, ini );
        s.EnableAutoupdates = GetPrivateProfileBoolA( "General", "EnableAutoupdates", ds.EnableAutoupdates, ini );
        s.EnableGodRays = GetPrivateProfileBoolA( "General", "EnableGodRays", ds.EnableGodRays, ini );
        s.EnableDoF = GetPrivateProfileBoolA( "General", "EnableDoF", ds.EnableDoF, ini );
        s.DoFGaussBlur = GetPrivateProfileBoolA( "General", "DoFGaussBlur", ds.DoFGaussBlur, ini );
        s.DoFFocusDistance = GetPrivateProfileFloatA( "General", "DoFFocusDistance", ds.DoFFocusDistance, ini );
        s.DoFFocusRange = GetPrivateProfileFloatA( "General", "DoFFocusRange", ds.DoFFocusRange, ini );
        s.DoFBokehRadius = GetPrivateProfileFloatA( "General", "DoFBokehRadius", ds.DoFBokehRadius, ini );
        s.DoFMaxBlur = GetPrivateProfileFloatA( "General", "DoFMaxBlur", ds.DoFMaxBlur, ini );
        s.AllowNormalmaps = GetPrivateProfileIntA( "General", "AllowNormalmaps", ds.AllowNormalmaps, ini.c_str() );
        s.AllowNumpadKeys = GetPrivateProfileBoolA( "General", "AllowNumpadKeys", ds.AllowNumpadKeys, ini );
        s.EnableInactiveFpsLock = GetPrivateProfileBoolA( "General", "EnableInactiveFpsLock", ds.EnableInactiveFpsLock, ini );
        s.MTResoureceManager = GetPrivateProfileBoolA( "General", "MultiThreadResourceManager", ds.MTResoureceManager, ini );
        s.CompressBackBuffer = GetPrivateProfileBoolA( "General", "CompressBackBuffer", ds.CompressBackBuffer, ini );
        s.AnimateStaticVobs = GetPrivateProfileBoolA( "General", "AnimateStaticVobs", ds.AnimateStaticVobs, ini );
        s.DrawSectionIntersections = GetPrivateProfileBoolA( "General", "DrawWorldSectionIntersections", ds.DrawSectionIntersections, ini );
        s.DrawWorldOccluders = GetPrivateProfileBoolA( "General", "DrawWorldOccluders", ds.DrawWorldOccluders, ini );
        s.SunLightStrength = GetPrivateProfileFloatA( "General", "SunLightStrength", ds.SunLightStrength, ini );
        s.SortedTransparency = GetPrivateProfileBoolA( "General", "SortedTransparency", ds.SortedTransparency, ini );
#ifdef BUILD_GOTHIC_1_08k
        s.DrawG1ForestPortals = GetPrivateProfileBoolA( "General", "DrawG1ForestPortals", ds.DrawG1ForestPortals, ini );
        s.G1HighlightInteractiveFocus = GetPrivateProfileBoolA( "General", "G1HighlightInteractiveFocus", ds.G1HighlightInteractiveFocus, ini );
#endif
        s.DrawRainThroughTransformFeedback = GetPrivateProfileBoolA( "General", "DrawRainThroughTransformFeedback", ds.DrawRainThroughTransformFeedback, ini );

        /*
        * Draw-distance is Loaded on a per World basis using LoadRendererWorldSettings
        */

        s.EnableOcclusionCulling = GetPrivateProfileBoolA( "General", "EnableOcclusionCulling", ds.EnableOcclusionCulling, ini );
        s.EnablePortalCulling = GetPrivateProfileBoolA( "General", "EnablePortalCulling", ds.EnablePortalCulling, ini );
        s.PortalCullingNearRadius = GetPrivateProfileFloatA( "General", "PortalCullingNearRadius", ds.PortalCullingNearRadius, ini );
        s.EnablePortalShadowSkip = GetPrivateProfileBoolA( "General", "EnablePortalShadowSkip", ds.EnablePortalShadowSkip, ini );
        s.EnableHorizonCulling = GetPrivateProfileBoolA( "General", "EnableHorizonCulling", ds.EnableHorizonCulling, ini );
        s.EnableMeshOptimization = GetPrivateProfileBoolA( "General", "EnableMeshOptimization", ds.EnableMeshOptimization, ini );
        s.EnableShadowIndexBuffers = GetPrivateProfileBoolA( "General", "EnableShadowIndexBuffers", ds.EnableShadowIndexBuffers, ini );
        s.FpsLimit = GetPrivateProfileIntA( "General", "FpsLimit", 0, ini.c_str() );
        s.PausedFpsLimit = GetPrivateProfileIntA( "General", "PausedFpsLimit", ds.PausedFpsLimit, ini.c_str() );
        // Not optional: an unthrottled paused loop crashes drivers, so the ini can only pick a value
        // inside the allowed band (0/garbage included) - it can't switch the cap off.
        s.PausedFpsLimit = std::clamp( s.PausedFpsLimit,
            GothicRendererSettings::PausedFpsLimitMin, GothicRendererSettings::PausedFpsLimitMax );

        // override INI settings with GMP minimum values.
        if ( GMPModeActive ) {
            s.OutdoorVobDrawRadius = std::max( 20000.f, s.OutdoorVobDrawRadius );
            s.OutdoorSmallVobDrawRadius = std::max( 20000.f, s.OutdoorSmallVobDrawRadius );
            s.SectionDrawRadius = std::max( 3, s.SectionDrawRadius );
        }

        s.EnableShadows = GetPrivateProfileBoolA( "Shadows", "EnableShadows", ds.EnableShadows, ini );
        s.ShadowFilterMode = static_cast<GothicRendererSettings::E_ShadowFilterMode>(
            GetPrivateProfileIntA( "Shadows", "ShadowFilterMode",
                static_cast<int>(ds.ShadowFilterMode), ini.c_str() ));
        s.ShadowMapSize = GetPrivateProfileIntA( "Shadows", "ShadowMapSize", ds.ShadowMapSize, ini.c_str() );
        s.EnablePointlightShadows = GothicRendererSettings::EPointLightShadowMode( GetPrivateProfileIntA( "Shadows", "PointlightShadows", GothicRendererSettings::EPointLightShadowMode::PLS_STATIC_ONLY, ini.c_str() ) );
        s.WorldShadowRangeScale = GetPrivateProfileFloatA( "Shadows", "WorldShadowRangeScale", ds.WorldShadowRangeScale, ini );
        s.NumShadowCascades = GetPrivateProfileIntA( "Shadows", "NumShadowCascades", ds.NumShadowCascades, ini.c_str() );
        s.ShadowCascadePCFLimit = GetPrivateProfileIntA( "Shadows", "ShadowCascadePCFLimit", ds.ShadowCascadePCFLimit, ini.c_str() );
        s.ShadowFrustumCullingMode = static_cast<GothicRendererSettings::E_ShadowFrustumCulling>(GetPrivateProfileIntA( "Shadows", "ShadowFrustumCullingMode", ds.ShadowFrustumCullingMode, ini.c_str() ));
        s.EnableDynamicLighting = GetPrivateProfileBoolA( "Shadows", "EnableDynamicLighting", ds.EnableDynamicLighting, ini );
        s.SmoothShadowCameraUpdate = GetPrivateProfileBoolA( "Shadows", "SmoothCameraUpdate", ds.SmoothShadowCameraUpdate, ini );
        s.SmoothShadowFrequency = GetPrivateProfileFloatA( "Shadows", "SmoothShadowFrequency", ds.SmoothShadowFrequency, ini );
        s.ShadowStrength = GetPrivateProfileFloatA( "Shadows", "ShadowStrength", ds.ShadowStrength, ini );
        s.ShadowSoftness = GetPrivateProfileFloatA( "Shadows", "ShadowSoftness", ds.ShadowSoftness, ini );
        s.ShadowAOStrength = GetPrivateProfileFloatA( "Shadows", "ShadowAOStrength", ds.ShadowAOStrength, ini );
        s.WorldAOStrength = GetPrivateProfileFloatA( "Shadows", "WorldAOStrength", ds.WorldAOStrength, ini );
        s.SkyIblIntensity = GetPrivateProfileFloatA( "Shadows", "SkyIblIntensity", ds.SkyIblIntensity, ini );
        s.SkyOcclusionStrength = GetPrivateProfileFloatA( "Shadows", "SkyOcclusionStrength", ds.SkyOcclusionStrength, ini );
        s.SkyIblNightFloor = GetPrivateProfileFloatA( "Shadows", "SkyIblNightFloor", ds.SkyIblNightFloor, ini );
        s.DebugSettings.ShadowCascades.ShadowDepthSlopeBias = GetPrivateProfileFloatA( "Shadows", "ShadowDepthSlopeBias", ds.DebugSettings.ShadowCascades.ShadowDepthSlopeBias, ini );
        s.DebugSettings.ShadowCascades.FirstLodCascade = std::clamp( GetPrivateProfileSignedIntA( "Shadows", "FirstLodCascade", ds.DebugSettings.ShadowCascades.FirstLodCascade, ini ), -1, MAX_CSM_CASCADES );
        s.DebugSettings.ShadowCascades.CasterMinTexels = std::clamp( GetPrivateProfileFloatA( "Shadows", "CasterMinTexels", ds.DebugSettings.ShadowCascades.CasterMinTexels, ini ), 0.0f, 32.0f );
        s.AllowSelfShadowingPointlights = GetPrivateProfileBoolA( "Shadows", "AllowSelfShadowingPointlights", ds.AllowSelfShadowingPointlights, ini );
        s.DisableStaticPointlights = GetPrivateProfileBoolA( "Shadows", "DisableStaticPointlights", ds.DisableStaticPointlights, ini );
        s.SpecularHighlightsFlags = GetPrivateProfileSignedIntA( "Shadows", "SpecularHighlightsFlags", ds.SpecularHighlightsFlags, ini );

        INT2 res = {};
        RECT desktopRect;
        GetClientRect( GetDesktopWindow(), &desktopRect );
        s.textureMaxSize = std::max<int>( 32, GetPrivateProfileIntA( "Display", "TextureQuality", 16384, ini.c_str() ) );
        res.x = GetPrivateProfileIntA( "Display", "Width", desktopRect.right, ini.c_str() );
        res.y = GetPrivateProfileIntA( "Display", "Height", desktopRect.bottom, ini.c_str() );
        s.ResolutionScalePercent = std::clamp<int>( GetPrivateProfileIntA( "Display", "ResolutionScale", ds.ResolutionScalePercent, ini.c_str() ), 25, 200 );
        s.Upscaler = (GothicRendererSettings::E_Upscaler)std::clamp<int>( GetPrivateProfileIntA( "Display", "Upscaler", ds.Upscaler, ini.c_str() ), 0, GothicRendererSettings::E_Upscaler::_UPSCALER_NUM_MODES - 1 );
        s.EnableVSync = GetPrivateProfileBoolA( "Display", "VSync", ds.EnableVSync, ini );
        s.ForceFOV = GetPrivateProfileBoolA( "Display", "ForceFOV", ds.ForceFOV, ini );
        s.FOVHoriz = GetPrivateProfileIntA( "Display", "FOVHoriz", 90, ini.c_str() );
        s.FOVVert = GetPrivateProfileIntA( "Display", "FOVVert", 90, ini.c_str() );
        s.GammaValue = GetPrivateProfileFloatA( "Display", "Gamma", 1.0f, ini );
        s.BrightnessValue = GetPrivateProfileFloatA( "Display", "Brightness", 1.0f, ini );
        s.DisplayFlip = GetPrivateProfileBoolA( "Display", "DisplayFlip", ds.DisplayFlip, ini );
        s.LowLatency = GetPrivateProfileBoolA( "Display", "LowLatency", ds.LowLatency, ini );
        s.HDR_Monitor = GetPrivateProfileBoolA( "Display", "HDR_Monitor", false, ini );
        s.HDR_AutoMaxBrightness = GetPrivateProfileBoolA( "Display", "HDR_AutoMaxBrightness", ds.HDR_AutoMaxBrightness, ini );
        s.HDR_MaxBrightness = std::clamp( GetPrivateProfileFloatA( "Display", "HDR_MaxBrightness", ds.HDR_MaxBrightness, ini ), 100.0f, 10000.0f );
        s.HDR_PaperWhite = std::clamp( GetPrivateProfileFloatA( "Display", "HDR_PaperWhite", ds.HDR_PaperWhite, ini ), 50.0f, 1000.0f );
        s.StretchWindow = GetPrivateProfileBoolA( "Display", "StretchWindow", ds.StretchWindow, ini );
        s.GothicUIScale = GetPrivateProfileFloatA( "Display", "UIScale", 1.0f, ini );
        s.EnableRain = GetPrivateProfileBoolA( "Display", "Rain", ds.EnableRain, ini );
        s.EnableRainEffects = GetPrivateProfileBoolA( "Display", "RainEffects", ds.EnableRainEffects, ini );
        s.LimitLightIntesity = GetPrivateProfileBoolA( "Display", "LimitLightIntesity", ds.LimitLightIntesity, ini );

        s.EnableTiledLighting = GetPrivateProfileBoolA( "Display", "TiledLighting", s.EnableTiledLighting, ini );
        // s.RendererMode = static_cast<GothicRendererSettings::E_RendererMode>(GetPrivateProfileIntA( "Display", "RendererMode", s.RendererMode, ini.c_str() ) );
        // Force experimental settings OFF
        s.RendererMode = GothicRendererSettings::E_RendererMode::RM_Deferred;
        // ....

        // Backend selection is applied at engine creation (read directly there, before this load
        // runs). Mirror it into the settings struct so the settings UI reflects the current value.
        s.GraphicsAPI = _stricmp( GetPrivateProfileStringA( "Display", "GraphicsAPI", "D3D11", ini ).c_str(), "D3D12" ) == 0
            ? GothicRendererSettings::GRAPHICS_API_D3D12
            : GothicRendererSettings::GRAPHICS_API_D3D11;

        {
            // MSAA is only valid for the Forward+ renderer; clamp to the nearest supported power-of-two (1/2/4/8).
            int msaaSamples = GetPrivateProfileIntA( "Display", "MSAASamples", ds.MSAASamples, ini.c_str() );
            if ( msaaSamples >= 8 ) msaaSamples = 8;
            else if ( msaaSamples >= 4 ) msaaSamples = 4;
            else if ( msaaSamples >= 2 ) msaaSamples = 2;
            else msaaSamples = 1;
            s.MSAASamples = msaaSamples;
        }

        s.WindQuality = GetPrivateProfileIntA( "Display", "WindQuality", 0, ini.c_str() );
        s.GlobalWindStrength = GetPrivateProfileFloatA( "Display", "WindStrength", ds.GlobalWindStrength, ini );
        s.EnableWaterAnimation = GetPrivateProfileBoolA( "Display", "WaterWaveAnimation", ds.EnableWaterAnimation, ini );
        // Backward compat: legacy [Display]/WaterSSR bool maps to Medium/Disabled when the
        // new WaterSSRQuality key is absent.
        s.WaterSSRQuality = static_cast<GothicRendererSettings::E_WaterSSRQuality>(std::clamp<INT>(GetPrivateProfileIntA("Display", "WaterSSRQuality", ds.WaterSSRQuality, ini.c_str()), 0, 3));
        // Clamped to the enum's range here; whether RAYTRACED is actually usable (D3D12 + Tier 1.1 GPU) is
        // re-checked every frame in D3D12Water.cpp, so a config carried over to an unsupported GPU/backend
        // just silently behaves as SCREENSPACE rather than failing to load.
        s.WaterReflectionMode = static_cast<GothicRendererSettings::E_WaterReflectionMode>(std::clamp<INT>(GetPrivateProfileIntA("Display", "WaterReflectionMode", ds.WaterReflectionMode, ini.c_str()), 0, 1));
        s.HeroAffectsObjects = GetPrivateProfileBoolA( "Display", "HeroAffectsObjects", ds.HeroAffectsObjects, ini );

        if ( GetPrivateProfileBoolA( "SMAA", "Enabled", false, ini ) ) {
            s.AntiAliasingMode = GothicRendererSettings::E_AntiAliasingMode::AA_SMAA;
        }

        s.SharpenFactor = GetPrivateProfileFloatA( "SMAA", "SharpenFactor", 0.30f, ini );
        s.AntiAliasingMode = (GothicRendererSettings::E_AntiAliasingMode)GetPrivateProfileIntA( "General", "AntiAliasing", (int)ds.AntiAliasingMode, ini.c_str() );

        const HBAOSettings& defaultHBAOSettings = ds.HbaoSettings;
        s.HbaoSettings.Enabled = GetPrivateProfileBoolA( "HBAO", "Enabled", defaultHBAOSettings.Enabled, ini );
        s.HbaoSettings.Bias = GetPrivateProfileFloatA( "HBAO", "Bias", defaultHBAOSettings.Bias, ini );
        s.HbaoSettings.Radius = GetPrivateProfileFloatA( "HBAO", "Radius", defaultHBAOSettings.Radius, ini );
        s.HbaoSettings.PowerExponent = GetPrivateProfileFloatA( "HBAO", "PowerExponent", defaultHBAOSettings.PowerExponent, ini );
        s.HbaoSettings.BlurSharpness = GetPrivateProfileFloatA( "HBAO", "BlurSharpness", defaultHBAOSettings.BlurSharpness, ini );
        //s.HbaoSettings.EnableDualLayerAO = GetPrivateProfileIntA( "HBAO", "EnableDualLayerAO", defaultHBAOSettings.EnableDualLayerAO, ini.c_str() );
        s.HbaoSettings.EnableBlur = GetPrivateProfileBoolA( "HBAO", "EnableBlur", defaultHBAOSettings.EnableBlur, ini );
        s.HbaoSettings.SsaoBlurRadius = GetPrivateProfileIntA( "HBAO", "SsaoBlurRadius", defaultHBAOSettings.SsaoBlurRadius, ini.c_str() );
        s.HbaoSettings.SsaoStepCount = GetPrivateProfileIntA( "HBAO", "SsaoStepCount", defaultHBAOSettings.SsaoStepCount, ini.c_str() );

        // Migrate legacy HBAO Enabled setting to AoMode
        int defaultAoMode = static_cast<int>(s.HbaoSettings.Enabled ? AOMode::AO_HBAO : AOMode::AO_NONE);
        s.AoMode = static_cast<AOMode>(GetPrivateProfileIntA( "AO", "Mode", defaultAoMode, ini.c_str() ));

        const SAOSettings& defaultSAOSettings = ds.SaoSettings;
        s.SaoSettings.Radius = GetPrivateProfileFloatA( "SAO", "Radius", defaultSAOSettings.Radius, ini );
        s.SaoSettings.Bias = GetPrivateProfileFloatA( "SAO", "Bias", defaultSAOSettings.Bias, ini );
        s.SaoSettings.Intensity = GetPrivateProfileFloatA( "SAO", "Intensity", defaultSAOSettings.Intensity, ini );
        s.SaoSettings.NumSamples = GetPrivateProfileIntA( "SAO", "NumSamples", defaultSAOSettings.NumSamples, ini.c_str() );
        s.SaoSettings.BlurSharpness = GetPrivateProfileFloatA( "SAO", "BlurSharpness", defaultSAOSettings.BlurSharpness, ini );

        // XeGTAO (D3D12's AO_ASSAO implementation)
        const GTAOSettings& dGtao = ds.GtaoSettings;
        s.GtaoSettings.QualityLevel = GetPrivateProfileIntA( "GTAO", "QualityLevel", dGtao.QualityLevel, ini.c_str() );
        s.GtaoSettings.DenoisePasses = GetPrivateProfileIntA( "GTAO", "DenoisePasses", dGtao.DenoisePasses, ini.c_str() );
        s.GtaoSettings.Radius = GetPrivateProfileFloatA( "GTAO", "Radius", dGtao.Radius, ini );
        s.GtaoSettings.RadiusMultiplier = GetPrivateProfileFloatA( "GTAO", "RadiusMultiplier", dGtao.RadiusMultiplier, ini );
        s.GtaoSettings.FalloffRange = GetPrivateProfileFloatA( "GTAO", "FalloffRange", dGtao.FalloffRange, ini );
        s.GtaoSettings.SampleDistributionPower = GetPrivateProfileFloatA( "GTAO", "SampleDistributionPower", dGtao.SampleDistributionPower, ini );
        s.GtaoSettings.ThinOccluderCompensation = GetPrivateProfileFloatA( "GTAO", "ThinOccluderCompensation", dGtao.ThinOccluderCompensation, ini );
        s.GtaoSettings.FinalValuePower = GetPrivateProfileFloatA( "GTAO", "FinalValuePower", dGtao.FinalValuePower, ini );
        s.GtaoSettings.DepthMIPSamplingOffset = GetPrivateProfileFloatA( "GTAO", "DepthMIPSamplingOffset", dGtao.DepthMIPSamplingOffset, ini );
        s.GtaoSettings.QualityLevel = std::clamp( s.GtaoSettings.QualityLevel, 0, 3 );
        s.GtaoSettings.DenoisePasses = std::clamp( s.GtaoSettings.DenoisePasses, 0, 3 );

        s.EnableCustomFontRendering = GetPrivateProfileBoolA( "FontRendering", "Enable", ds.EnableCustomFontRendering, ini );

        s.FastInventoryRendering = GetPrivateProfileBoolA( "Inventory", "FastInventoryRendering", ds.FastInventoryRendering, ini );

        s.ThreadedShadowCulling = GetPrivateProfileBoolA( "Debug", "ThreadedShadowCulling", ds.ThreadedShadowCulling, ini );
        s.GpuVobCulling = GetPrivateProfileBoolA( "Debug", "GpuVobCulling", ds.GpuVobCulling, ini );
        s.GpuVobOcclusionCulling = GetPrivateProfileBoolA( "Debug", "GpuVobOcclusionCulling", ds.GpuVobOcclusionCulling, ini );
        s.UseGpuMorphFold = GetPrivateProfileBoolA( "Debug", "GpuMorphFold", ds.UseGpuMorphFold, ini );
        s.DebugSettings.FeatureSet.UseShadowAtlas = GetPrivateProfileBoolA( "Debug", "UseShadowAtlas", ds.DebugSettings.FeatureSet.UseShadowAtlas, ini );
        s.DebugSettings.FeatureSet.UseScreenSpaceShadowMask = GetPrivateProfileBoolA( "Debug", "UseScreenSpaceShadowMask", ds.DebugSettings.FeatureSet.UseScreenSpaceShadowMask, ini );
        s.DebugSettings.FeatureSet.GenerateAONormalsFromDepth = GetPrivateProfileBoolA( "Debug", "GenerateAONormalsFromDepth", ds.DebugSettings.FeatureSet.GenerateAONormalsFromDepth, ini );
        s.DebugSettings.FeatureSet.ForceFeatureLevel10 = GetPrivateProfileBoolA( "Debug", "ForceFeatureLevel10", ds.DebugSettings.FeatureSet.ForceFeatureLevel10, ini );
        s.DebugSettings.FeatureSet.EnableDriverExtensions = GetPrivateProfileBoolA( "Debug", "EnableDriverExtensions", ds.DebugSettings.FeatureSet.EnableDriverExtensions, ini );

        // Fix the resolution if the players maximum resolution got lower
        /*RECT r;
        GetClientRect( GetDesktopWindow(), &r );
        if ( res.x > r.right || res.y > r.bottom ) {
            LogInfo() << "Reducing resolution from (" << res.x << ", " << res.y << " to (" << r.right << ", " << r.bottom << ") because users desktop resolution got lowered";
            res = INT2( r.right, r.bottom );
        }*/

        res.x = std::max<int>( res.x, 800 );
        res.y = std::max<int>( res.y, 600 );
        s.LoadedResolution = res;
    }

    LogInfo() << "Applying Commandline-Overrides ...";
    // Override Settings from Commandline Parameters
    if ( Engine::GAPI->HasCommandlineParameter( "ZMAXFPS" ) ) {
        s.FpsLimit = std::stoi( zCOption::GetOptions()->ParameterValue( "ZMAXFPS" ) );
        LogInfo() << "-> FpsLimit: " << s.FpsLimit;
    }

    if ( Engine::GAPI->HasCommandlineParameter( "game" ) ) {
        auto gameIni = zCOption::GetOptions()->ParameterValue( "game" );
        auto nLastDot = gameIni.find_last_of( '.' );
        if ( gameIni != "GOTHICGAME.INI" && nLastDot != std::string::npos ) {
            Engine::GAPI->SetGameName( gameIni.substr( 0, nLastDot ) );
            LogInfo() << "-> Game: " << Engine::GAPI->GetGameName();
#ifdef BUILD_SPACER_NET
            if ( Engine::GAPI->GetGameName() == "SPACER_NET" ) {
                LogInfo() << "-> Running in Spacer.NET";
                s.RunInSpacerNet = true;
            }
#endif
        } else {
            Engine::GAPI->SetGameName( "Original" );
            LogInfo() << "-> Game: Original";
        }
    } else {
        Engine::GAPI->SetGameName( "Original" );
        LogInfo() << "-> Game: Original";
    }

    if ( s.ChangeWindowPreset ) {
        WritePrivateProfileStringA( "General", "ChangeToMode", "0", ini.c_str() );
        switch ( s.ChangeWindowPreset ) {
            case WINDOW_MODE_FULLSCREEN_EXCLUSIVE:
            {
                s.DisplayFlip = false;
                s.StretchWindow = true;
                zSTRING section( "VIDEO" ); zSTRING defValue( "0" );
                zCOption::GetOptions()->WriteString( section, "zStartupWindowed", defValue );
                WritePrivateProfileStringA( "Display", "DisplayFlip", "0", ini.c_str() );
                WritePrivateProfileStringA( "Display", "LowLatency", "0", ini.c_str() );
                WritePrivateProfileStringA( "Display", "StretchWindow", "1", ini.c_str() );
                break;
            }
            case WINDOW_MODE_FULLSCREEN_BORDERLESS: {
                s.DisplayFlip = true;
                s.LowLatency = false;
                s.StretchWindow = true;
                WritePrivateProfileStringA( "Display", "DisplayFlip", "1", ini.c_str() );
                WritePrivateProfileStringA( "Display", "LowLatency", "0", ini.c_str() );
                WritePrivateProfileStringA( "Display", "StretchWindow", "1", ini.c_str() );
                break;
            }
            case WINDOW_MODE_FULLSCREEN_LOWLATENCY: {
                s.DisplayFlip = true;
                s.LowLatency = true;
                s.StretchWindow = true;
                WritePrivateProfileStringA( "Display", "DisplayFlip", "1", ini.c_str() );
                WritePrivateProfileStringA( "Display", "LowLatency", "1", ini.c_str() );
                WritePrivateProfileStringA( "Display", "StretchWindow", "1", ini.c_str() );
                break;
            }
            case WINDOW_MODE_WINDOWED: {
                s.DisplayFlip = true;
                s.StretchWindow = false;
                zSTRING section( "VIDEO" ); zSTRING defValue( "1" );
                zCOption::GetOptions()->WriteString( section, "zStartupWindowed", defValue );
                WritePrivateProfileStringA( "Display", "DisplayFlip", "0", ini.c_str() );
                WritePrivateProfileStringA( "Display", "LowLatency", "0", ini.c_str() );
                WritePrivateProfileStringA( "Display", "StretchWindow", "0", ini.c_str() );
                break;
            }
        }
        s.ChangeWindowPreset = 0;
    }

    return XR_SUCCESS;
}

/** Returns the main-thread id */
DWORD GothicAPI::GetMainThreadID() {
    return MainThreadID;
}

/** Returns the current cursor position, in pixels */
POINT GothicAPI::GetCursorPosition() {
    POINT p;
    GetCursorPos( &p );
    ScreenToClient( OutputWindow, &p );

    RECT r;
    GetClientRect( OutputWindow, &r );

    float x = static_cast<float>(p.x) / static_cast<float>(r.right);
    float y = static_cast<float>(p.y) / static_cast<float>(r.bottom);

    p.x = static_cast<long>(x * static_cast<float>(Engine::GraphicsEngine->GetBackbufferResolution().x));
    p.y = static_cast<long>(y * static_cast<float>(Engine::GraphicsEngine->GetBackbufferResolution().y));

    return p;
}

/** Adds a staging texture to the list of the staging textures for this frame */
void GothicAPI::AddStagingTexture( GfxTexture* gfx, UINT mip, const Microsoft::WRL::ComPtr<ID3D11Texture2D>& stagingTexture,
    const Microsoft::WRL::ComPtr<ID3D11Texture2D>& texture ) {
    Engine::GAPI->EnterResourceCriticalSection();
    FrameStagingTextures.push_back( DeferredMipUpload{ gfx, mip, stagingTexture, texture } );
    Engine::GAPI->LeaveResourceCriticalSection();
}

/** Adds a mip map generation deferred command */
void GothicAPI::AddMipMapGeneration( GfxTexture* texture ) {
    Engine::GAPI->EnterResourceCriticalSection();
    FrameMipMapGenerations.push_back( texture );
    Engine::GAPI->LeaveResourceCriticalSection();
}

/** Drops any pending deferred commands referencing this texture */
void GothicAPI::RemovePendingTextureCommands( GfxTexture* texture ) {
    Engine::GAPI->EnterResourceCriticalSection();
    // Usually empty, and never long: this only holds what one frame's worth of loading queued up.
    std::erase( FrameMipMapGenerations, texture );
    std::erase_if( FrameStagingTextures, [texture]( const DeferredMipUpload& u ) {
        return u.Texture == texture;
    } );
    Engine::GAPI->LeaveResourceCriticalSection();
}

/** Adds a texture to the list of the loaded textures for this frame */
void GothicAPI::AddFrameLoadedTexture( MyDirectDrawSurface7* srf ) {
    srf->AddRef();

    Engine::GAPI->EnterResourceCriticalSection();
    FrameLoadedTextures.push_back( srf );
    Engine::GAPI->LeaveResourceCriticalSection();
}

/** Sets loaded textures of this frame ready */
void GothicAPI::SetFrameProcessedTexturesReady() {
    for ( MyDirectDrawSurface7* srf : FrameLoadedTextures ) {
        srf->SetReady( true );
        srf->Release();
    }

    FrameLoadedTextures.clear();
}

/** Draws a morphmesh */
void GothicAPI::DrawMorphMesh( zCMorphMesh* msh, std::map<zCMaterial*, std::vector<std::unique_ptr<MeshInfo>>>& meshes ) {
    zCProgMeshProto* morphMesh = msh->GetMorphMesh();
    if ( !morphMesh )
        return;
        
    // Ensure to call `WorldConverter::UpdateMorphMeshVisual( ... );` once per frame for this mesh to update the vertex buffers before drawing.

    D3D11GraphicsEngine* g = AsD3D11Engine(Engine::GraphicsEngine);

    const bool isZPrepass = g->GetRenderingStage() == DES_Z_PRE_PASS;
    const bool bindShader = g->GetRenderingStage() == DES_MAIN || isZPrepass;

    zCTexture* lastTex = nullptr;
    for ( int i = 0; i < morphMesh->GetNumSubmeshes(); i++ ) {
        zCSubMesh* s = morphMesh->GetSubmesh( i );
        if ( zCTexture* texture = s->Material->GetAniTexture() ) {
            if ( lastTex != texture ) {
                lastTex = texture;
                if ( isZPrepass ) {
                    // Texture still streaming in (async load)? Draw depth with a black placeholder
                    // (opaque, no alpha cutout) instead of skipping, so it doesn't vanish for a frame.
                    if ( texture->CacheIn( 0.6f ) == zRES_CACHED_IN ) {
                        texture->GetSurface()->GetEngineTexture()->BindToPixelShader( 0 );
                    } else {
                        g->GetBlackTexture()->BindToPixelShader( 0 );
                    }
                } else if ( !g->BindTextureNRFX( s->Material, bindShader ) ) {
                    continue;
                }
            }
        }

        for ( auto const& it : meshes ) {
            for ( auto& mi : it.second ) {
                if ( mi->MeshIndex == i ) {
                    Engine::GraphicsEngine->DrawVertexBufferIndexed( mi->GetMeshVertexBuffer(), mi->GetMeshIndexBuffer(), mi->Indices.size() );
                    goto Out_Of_Nested_Loop;
                }
            }
        }
        Out_Of_Nested_Loop:;
    }
}

void GothicAPI::DrawMorphMesh_Layered( zCMorphMesh* msh, std::map<zCMaterial*, std::vector<std::unique_ptr<MeshInfo>>>& meshes ) {
    // layered draw always has WhiteTexture bound to PS Slot 0
    // no need to bind a texture! Just used for shadows
    zCProgMeshProto* morphMesh = msh->GetMorphMesh();
    if ( !morphMesh )
        return;

    // Ensure to call `WorldConverter::UpdateMorphMeshVisual( ... );` once per frame for this mesh to update the vertex buffers before drawing.

    D3D11GraphicsEngine* g = AsD3D11Engine(Engine::GraphicsEngine);

    GfxTexture* whiteTexture = g->GetWhiteTexture();
    void* lastTex = whiteTexture;

    for ( int i = 0; i < morphMesh->GetNumSubmeshes(); i++ ) {
        for ( auto const& it : meshes ) {
            zCTexture* texture;
            if ( it.first && (texture = it.first->GetAniTexture()) != nullptr ) {
                if ( texture->CacheIn( 0.6f ) != zRES_CACHED_IN ) {
                    continue; // we cant determine if we need to draw this, alpha data is only available after loading a texture.
                }

                const bool needTex = texture != lastTex
                    && (texture->HasAlphaChannel() || it.first->HasAlphaTest());

                if ( needTex ) {
                    texture->GetSurface()->GetEngineTexture()->BindToPixelShader( 0 );
                    lastTex = texture;
                } else if ( lastTex != whiteTexture ) {
                    whiteTexture->BindToPixelShader( 0 );
                    lastTex = whiteTexture;
                }
            }

            for ( auto& mi : it.second ) {
                if ( mi->MeshIndex == i ) {
                    g->DrawVertexBufferInstancedIndexed( mi->GetMeshVertexBuffer(), mi->GetMeshIndexBuffer(), mi->Indices.size(), 6 );
                    goto Out_Of_Nested_Loop;
                }
            }
        }
    Out_Of_Nested_Loop:;
    }
}

/** Add particle effect */
void GothicAPI::AddParticleEffect( zCVob* vob ) {
    if ( zCParticleFX* particle = reinterpret_cast<zCParticleFX*>(vob->GetVisual()) ) {
        if ( zCParticleEmitter* emitter = particle->GetEmitter() ) {
            if ( emitter->GetVisShpType() == 5 ) {
                if ( zCModel* model = emitter->GetVisShpModel() ) {
                    MeshVisualInfo* mi = ParticleEffectProgMeshes.emplace(vob, std::make_unique<MeshVisualInfo>()).first->second.get();
                    // Same rationale as the prog-mesh branch below: don't hitch the frame a model-shaped
                    // PFX burst spawns on.
                    WorldConverter::ExtractProgMeshProtoFromModelAsync( model, mi );
                } else if ( zCProgMeshProto* progMesh = emitter->GetVisShpProgMesh() ) {
                    MeshVisualInfo* mi = ParticleEffectProgMeshes.emplace(vob, std::make_unique<MeshVisualInfo>()).first->second.get();
                    // Same rationale as GothicAPI::OnAddVob: spawning many PFX with a prog-mesh particle
                    // shape at once (e.g. a fire/smoke burst) shouldn't hitch the frame it happens on.
                    WorldConverter::Extract3DSMeshFromVisual2Async( progMesh, progMesh, mi );
                } else if ( zCMesh* mesh = emitter->GetVisShpMesh() ) {
                    MeshVisualInfo* mi = ParticleEffectProgMeshes.emplace(vob, std::make_unique<MeshVisualInfo>()).first->second.get();
                    WorldConverter::ExtractProgMeshProtoFromMesh( mesh, mi );
                }
            }
        }
    }
}

/** Destroy particle effect */
void GothicAPI::DestroyParticleEffect( zCVob* vob ) {
    auto it = ParticleEffectProgMeshes.find(vob);
    if ( it != ParticleEffectProgMeshes.end() ) {
        ParticleEffectProgMeshes.erase( it );
    }
}

/** Removes the given quadmark */
void GothicAPI::RemoveQuadMark( zCQuadMark* mark ) {
    QuadMarks.erase( mark );
}

/** Returns the quadmark info for the given mark */
QuadMarkInfo* GothicAPI::GetQuadMarkInfo( zCQuadMark* mark ) {
    return &QuadMarks[mark];
}

/** Returns all quad marks */
const std::unordered_map<zCQuadMark*, QuadMarkInfo>& GothicAPI::GetQuadMarks() {
    return QuadMarks;
}

/** Add new zCFlash object */
void GothicAPI::AddFlash( zCFlash* flash, zCVob* vob ) {
    FlashVisuals[flash] = vob;
}

/** Remove zCFlash object */
void GothicAPI::RemoveFlash( zCFlash* flash ) {
    FlashVisuals.erase( flash );
}

/** Add this frame thunder poly strip */
void GothicAPI::AddThunderPolyStrip( zCPolyStrip* polyStrip ) {
    FrameThunderPolyStrips.emplace_back(polyStrip);
}

/** Returns wether the camera is underwater or not */
bool GothicAPI::IsUnderWater() {
    if ( oCGame* ogame = oCGame::GetGame() ) {
        if ( zCWorld* world = ogame->_zCSession_world ) {
            if ( zCSkyController_Outdoor* skyController = world->GetSkyControllerOutdoor() ) {
                return skyController->GetUnderwaterFX() != 0;
            }
        }
    }

    return false;
}

/** Returns if the given vob is registered in the world */
SkeletalVobInfo* GothicAPI::GetSkeletalVobByVob( zCVob* vob ) {
    auto sit = SkeletalVobMap.find( vob );
    if ( sit != SkeletalVobMap.end() ) {
        return sit->second;
    }
    return nullptr;
}

/** Returns true if the given string can be found in the commandline */
bool GothicAPI::HasCommandlineParameter( const std::string& param ) {
    return zCOption::GetOptions()->IsParameter( param );
}

/** Reloads all textures */
void GothicAPI::ReloadTextures() {
    zCResourceManager* resman = zCResourceManager::GetResourceManager();

    LogInfo() << "Reloading textures...";

    // This throws all texture out of the cache
    if ( resman )
        resman->PurgeCaches( 0 );
}

/** Gets the int-param from the ini. String must be UPPERCASE. */
int GothicAPI::GetIntParamFromConfig( const std::string& param ) {
    return ConfigIntValues[param];
}

/** Sets the given int param into the internal ini-cache. That does not set the actual value for the game! */
void GothicAPI::SetIntParamFromConfig( const std::string& param, int value ) {
    ConfigIntValues[param] = value;
}

/** Returns the frame particle info collected from all DrawParticleFX-Calls */
std::map<zCTexture*, ParticleRenderInfo>& GothicAPI::GetFrameParticleInfo() {
    return FrameParticleInfo;
}

/** Checks if the normalmaps are right */
bool GothicAPI::CheckNormalmapFilesOld() {
    /** If the directory is empty, FindFirstFile() will only find the entry for
        the directory itself (".") and FindNextFile() will fail with ERROR_FILE_NOT_FOUND. **/

    WIN32_FIND_DATAA data;
    HANDLE f = FindFirstFile(R"(system\GD3D11\Textures\Replacements\*.dds)", &data );
    if ( !FindNextFile( f, &data ) ) {
        /*
                // Inform the user that he is missing normalmaps
                MessageBoxA(nullptr, "You don't seem to have any normalmaps installed. Please make sure you have put all DDS-Files from the package into the right folder:\n"
                                  "system\\GD3D11\\Textures\\Replacements\n\n"
                                  "If you don't know where to get the package, you can download them from:\n"
                                  "http://www.gothic-dx11.de/download/replacements_dds.7z\n\n"
                                  "The link has been copied to your clipboard.", "Normalmaps missing", MB_OK | MB_ICONINFORMATION);

                // Put the link into the clipboard
                clipput("http://www.gothic-dx11.de/download/replacements_dds.7z\n\n");*/

        return false;
    }

    FindClose( f );

    // Inform the user that Normalmaps are in another folder since X16. 
    // Also quickly copy them over to the new location so they don't have to redownload everything
    MessageBox( nullptr, "Normalmaps are now handled differently. They are now stored in 'GD3D11\\Textures\\replacements\\Normalmaps_MODNAME' and "
        "will automatically be downloaded from our servers in the game.\n"
        "\n"
        "The old normalmaps will be moved to the new location. You should however go and delete everything in the replacements-folder"
        " if you have installed normalmaps for any Mod (Like L'Hiver) and let GD3D11 download them again for you.", "Something has changed...", MB_OK | MB_TOPMOST );

    system(R"(mkdir system\GD3D11\Textures\Replacements\Normalmaps_Original)");
    system(R"(move /Y system\GD3D11\Textures\Replacements\*.dds system\GD3D11\Textures\Replacements\Normalmaps_Original)");

    return true;
}

/** Returns the gamma value from the ingame menu */
float GothicAPI::GetGammaValue() {
    return RendererState.RendererSettings.GammaValue;
    //return zCRndD3D::GetRenderer()->GetGammaValue();
}

/** Returns the brightness value from the ingame menu */
float GothicAPI::GetBrightnessValue() {
    return RendererState.RendererSettings.BrightnessValue;
}

/** Puts the custom-polygons into the bsp-tree */
void GothicAPI::PutCustomPolygonsIntoBspTree() {
    PutCustomPolygonsIntoBspTreeRec( &BspLeafVobLists[LoadedWorldInfo->BspTree->GetRootNode()] );
}

void GothicAPI::PutCustomPolygonsIntoBspTreeRec( BspInfo* base ) {
    if ( !base || !base->OriginalNode )
        return;

    if ( base->OriginalNode->IsLeaf() ) {
        // Get all sections this nodes intersects with
        std::vector<WorldMeshSectionInfo*> sections;
        GetIntersectingSections( base->OriginalNode->BBox3D.Min, base->OriginalNode->BBox3D.Max, sections );

        for (auto& section : sections) {
            for (auto poly : section->SectionPolygons) {
                if ( !poly->GetMaterial() || // Skip stuff with alpha-channel or not set material
                    poly->GetMaterial()->HasAlphaTest() )
                    continue;

                // Check all triangles
                for ( int v = 0; v < poly->GetNumPolyVertices(); v++ ) {
                    // Check if one vertex is inside the node // TODO: This will fail for very large triangles!
                    zCVertex** vx = poly->getVertices();

                    if ( Toolbox::PositionInsideBox( vx[v]->Position,
                        base->OriginalNode->BBox3D.Min,
                        base->OriginalNode->BBox3D.Max ) ) {
                        base->NodePolygons.push_back( poly );
                        break;
                    }
                }
            }
        }
    } else {
        PutCustomPolygonsIntoBspTreeRec( base->Front );
        PutCustomPolygonsIntoBspTreeRec( base->Back );
    }
}

/** Returns the sections intersecting the given boundingboxes */
void GothicAPI::GetIntersectingSections( const XMFLOAT3& min, const XMFLOAT3& max, std::vector<WorldMeshSectionInfo*>& sections ) {
    for (auto& valx : Engine::GAPI->GetWorldSections() | std::views::values) {
        for (auto& section : valx | std::views::values) {
            if ( Toolbox::AABBsOverlapping( section.BoundingBox.Min, section.BoundingBox.Max, min, max ) ) {
                sections.push_back( &section );
            }
        }
    }
}

/** Generates zCPolygons for the loaded sections */
void GothicAPI::CreatezCPolygonsForSections() {
    for (auto& sectionsX : Engine::GAPI->GetWorldSections() | std::views::values) {
        for (auto& section : sectionsX | std::views::values) {
            for ( auto& [first, second] : section.WorldMeshes) {
                if ( !first.Material ||
                    first.Material->HasAlphaTest() )
                    continue;

                first.Material->SetAlphaFunc( zMAT_ALPHA_FUNC_NONE );

                // The world mesh only keeps WorldVertexCPU now, so rebuild the full vertices this wants.
                // Per-vertex normals come out zero; zCPolygon::CalcNormal() still derives the polygon plane
                // from the positions, which is what the BSP/collision side of this path uses.
                std::vector<ExVertexStruct> rebuilt( second->CpuVertices.size() );
                for ( size_t v = 0; v < second->CpuVertices.size(); ++v ) {
                    rebuilt[v].Position = second->CpuVertices[v].Position;
                    rebuilt[v].TexCoord = second->CpuVertices[v].TexCoord;
                    rebuilt[v].TexCoord2 = second->CpuVertices[v].TexCoord2;
                }

                WorldConverter::ConvertExVerticesTozCPolygons( rebuilt, second->Indices, first.Material, section.SectionPolygons );
            }
        }
    }
}

/** Collects polygons in the given AABB */
void GothicAPI::CollectPolygonsInAABB( const zTBBox3D& bbox, zCPolygon**& polyList, int& numFound ) {
    static std::vector<zCPolygon*> list; // This function is defined to only temporary hold the found polygons in the game. 
                                         // This is ugly, but that's how they do it.
    list.clear();

    CollectPolygonsInAABBRec( &BspLeafVobLists[LoadedWorldInfo->BspTree->GetRootNode()], bbox, list );

    // Give out data to calling function
    polyList = &list[0];
    numFound = list.size();
}

/** Collects polygons in the given AABB */
void GothicAPI::CollectPolygonsInAABBRec( BspInfo* base, const zTBBox3D& bbox, std::vector<zCPolygon*>& list ) {
    zCBspNode* node = static_cast<zCBspNode*>(base->OriginalNode);

    while ( node ) {
        if ( node->IsLeaf() ) {
            zCBspLeaf* leaf = reinterpret_cast<zCBspLeaf*>(node);
            if ( leaf->NumPolys > 0 ) {
                // Cancel search in this subtree if this doesn't overlap with our AABB
                if ( !Toolbox::AABBsOverlapping( bbox.Min, bbox.Max, leaf->BBox3D.Min, leaf->BBox3D.Max ) )
                    return;

                // Insert all polygons we got here
                list.insert( list.end(), base->NodePolygons.begin(), base->NodePolygons.end() );
            }

            // Got all the polygons and this is a leaf, don't need to do tests for more searches
            return;
        }

        // Get next tree to look at
        int sides = bbox.ClassifyToPlane( node->Plane.Distance, node->PlaneSignbits );

        switch ( sides ) {
        case zTBBox3D::zPLANE_INFRONT:
            node = static_cast<zCBspNode*>(node->Front);
            base = base->Front;
            break;

        case zTBBox3D::zPLANE_BEHIND:
            node = static_cast<zCBspNode*>(node->Back);
            base = base->Back;
            break;

        case zTBBox3D::zPLANE_SPANNING:
            if ( base->Front )
                CollectPolygonsInAABBRec( base->Front, bbox, list );

            node = static_cast<zCBspNode*>(node->Back);
            base = base->Back;
            break;
        }
    }
}

/** Returns our bsp-root-node */
BspInfo* GothicAPI::GetNewRootNode() {
    return &BspLeafVobLists[LoadedWorldInfo->BspTree->GetRootNode()];
}

/** Prints a message to the screen for the given amount of time */
void GothicAPI::PrintMessageTimed( const INT2& position, const std::string& strMessage, float time, DWORD color ) {
    zCView* view = oCGame::GetGame()->GetGameView();
    if ( view ) {
        zSTRING message( strMessage.c_str() );
        view->PrintTimed( position.x, position.y, message, time, &color );
        message.Delete();
    }
}

/** Prints information about the mod to the screen for a couple of seconds */
void GothicAPI::PrintModInfo() {
    std::string version = std::string( VERSION_STRING );
    std::string gpu = Engine::GraphicsEngine->GetGraphicsDeviceName();
    PrintMessageTimed( INT2( 5, 5 ), "GD3D11 - " + version, 8000.0f );
    PrintMessageTimed( INT2( 5, 180 ), "Device: " + gpu, 8000.0f );
}

/** Returns the current weight of the rain-fx. The bigger value of ours and gothics is returned. */
float GothicAPI::GetRainFXWeight() {
    float myRainFxWeight = RendererState.RendererSettings.RainSceneWettness;
    float gRainFxWeight = 0.0f;

    if ( oCGame* ogame = oCGame::GetGame() ) {
        if ( zCWorld* world = ogame->_zCSession_world ) {
            if ( zCSkyController_Outdoor* skyController = world->GetSkyControllerOutdoor() ) {
                if ( skyController->GetWeatherType() == zTWeather::zTWEATHER_RAIN
                    || skyController->GetWeatherType() == zTWeather::zTWEATHER_SNOW ) {
                    gRainFxWeight = skyController->GetRainFXWeight();
                }
            }
        }
    }

    // This doesn't seem to go as high as 1 or just very slowly. Scale it so it does go up quicker.
    gRainFxWeight = std::min( gRainFxWeight / 0.85f, 1.0f );

    // Return the higher of the two, so we get the chance to overwrite it
    return std::max( myRainFxWeight, gRainFxWeight );
}

/** Returns true if gothic's current outdoor weather is snow */
bool GothicAPI::IsSnowingWeather() {
    if ( oCGame* ogame = oCGame::GetGame() ) {
        if ( zCWorld* world = ogame->_zCSession_world ) {
            if ( zCSkyController_Outdoor* skyController = world->GetSkyControllerOutdoor() ) {
                return skyController->GetWeatherType() == zTWeather::zTWEATHER_SNOW;
            }
        }
    }
    return false;
}

/** Returns the wetness of the scene. Lasts longer than RainFXWeight */
float GothicAPI::GetSceneWetness() {
    // Snow drives the same particle-fx weight as rain (see GetRainFXWeight), but snow must not wet the
    // ground - no darkening, no ripples, no wet specular. Only our own manual override still counts here,
    // so the wetness slider keeps working while it snows. Note this decays through the branch below
    // instead of returning early, so wetness fades out normally when the weather switches to snow.
    float rain = IsSnowingWeather()
        ? RendererState.RendererSettings.RainSceneWettness
        : GetRainFXWeight();
    static DWORD s_rainStopTime = Toolbox::timeSinceStartMs();

    if ( rain >= SceneWetness ) {
        SceneWetness = rain; // Rain is starting or still going
        s_rainStopTime = Toolbox::timeSinceStartMs(); // Just querry this until we fall into the else-branch some time
    } else {
        // Rain has just stopped, get time of how long the rain isn't going anymore
        DWORD rainStoppedFor = Toolbox::timeSinceStartMs() - s_rainStopTime;

        // Get ratio between duration and that time. This value is near 1 when we almost reached the duration
        float ratio = rainStoppedFor / static_cast<float>(SCENE_WETNESS_DURATION_MS);

        // clamp at 1.0f so the whole thing doesn't start over when reaching 0
        if ( ratio >= 1.0f )
            ratio = 1.0f;

        // make the wetness last longer by applying a pow, then inverse it so 1 means that the scene is actually wet
        SceneWetness = std::max( 0.0f, 1.0f - pow( ratio, 8.0f ) );

        // Just force to 0 when this reached a tiny amount so we can switch the shaders
        if ( SceneWetness < 0.00001f )
            SceneWetness = 0.0f;
    }

    return SceneWetness;
}

/** Adds a future to the internal buffer */
void GothicAPI::AddFuture( std::future<void>& future ) {
    FutureList.push_back( std::move( future ) );
}

/** Checks which futures are ready and cleans them */
void GothicAPI::CleanFutures() {
    for ( auto it = FutureList.begin(); it != FutureList.end();) {
        if ( it->valid() ) {
            // If the thread was completed, get its "returnvalue" and delete it.
            it->get();
            it = FutureList.erase( it );
        } else {
            ++it;
        }
    }
}

/** Reset gothic render states so the engine will set them anew */
void GothicAPI::ResetRenderStates() {
    if ( zCRndD3D* renderer = zCRndD3D::GetRenderer() ) {
        renderer->ResetRenderState();
    }
}

/** Get sky timescale variable */
float GothicAPI::GetSkyTimeScale() {
    return SkyRenderer->GetAtmoshpereSettings().SkyTimeScale;
}

/** Processes vobs and lights in a single BSP leaf node that has already passed distance and frustum tests. */
static void CollectLeafVobs(
    BspInfo* base,
    float leafDistSq,
    const RndCullContext& ctx,
    DirectX::ContainmentType clipResult,
    BspTreeVobVisitor* visitor
) {
    const float vobIndoorDistSq = ctx.drawDistancesSq.IndoorVobs;
    const float vobOutdoorDistSq = ctx.drawDistancesSq.OutdoorVobs;
    const float vobOutdoorSmallDistSq = ctx.drawDistancesSq.OutdoorVobsSmall;
    const float visualFXDrawRadius = ctx.drawDistances.VisualFX;
    const float visualFXDrawRadiusSq = ctx.drawDistancesSq.VisualFX;
    const XMVECTOR cameraPosition = XMLoadFloat3( &ctx.cameraPosition );
    const bool collectIndoorVobs = ctx.drawFlags.CollectIndoorVobs;
    const bool collectMobs = ctx.drawFlags.CollectMobs;
    const bool collectLights = ctx.drawFlags.CollectLights;
    const auto& rendererSettings = Engine::GAPI->GetRendererState().RendererSettings;
    auto& VobLightMap = Engine::GAPI->VobLightMap;

    zCBspLeaf* leaf = static_cast<zCBspLeaf*>(base->OriginalNode);
    std::vector<LeafVobEntry>& listA = base->IndoorVobs;
    std::vector<LeafVobEntry>& listB = base->SmallVobs;
    std::vector<LeafVobEntry>& listC = base->Vobs;
    std::vector<SkeletalVobInfo*>& listD = base->Mobs;

    if ( ctx.drawFlags.DrawVOBs ) {
        if ( collectIndoorVobs && leafDistSq < vobIndoorDistSq ) {
            // Portal culling: a room the camera cannot see into through any chain of portals has
            // none of its VOBs collected at all. Leafs outside every sector pass through untouched.
            if ( !ctx.portalCuller || ctx.portalCuller->IsLeafVisible( *base ) ) {
                const auto distSq = XMVectorReplicate( vobIndoorDistSq );
                CVVH_AddNotDrawnVobToList( distSq, listA, ctx, clipResult, visitor,
                    ctx.portalCuller ? base : nullptr );
            }
        }

        if ( leafDistSq < vobOutdoorSmallDistSq ) {
            const auto distSq = XMVectorReplicate( vobOutdoorSmallDistSq );
            CVVH_AddNotDrawnVobToList( distSq, listB, ctx, clipResult, visitor );
        }

        if ( leafDistSq < vobOutdoorDistSq ) {
            const auto distSq = XMVectorReplicate( vobOutdoorDistSq );
            CVVH_AddNotDrawnVobToList( distSq, listC, ctx, clipResult, visitor );
        }
    }

    if ( collectMobs
        && ctx.drawFlags.DrawMobs && leafDistSq < vobOutdoorSmallDistSq ) {
        CVVH_AddNotDrawnVobToList( listD, vobOutdoorDistSq, ctx, clipResult, visitor );
    }

    if ( collectLights
            && ctx.drawFlags.EnableDynamicLighting && leafDistSq < visualFXDrawRadiusSq ) {

        // Add dynamic lights.
        // A light is registered in every leaf its range touches, so this body runs many times per
        // light per frame. Everything below is therefore ordered cheapest-first, and the dedup runs
        // before the range and frustum tests so a light pays for those exactly once per frame.
        const int numLights = leaf->LightVobList.NumInArray;

        // base->Lights mirrors LightVobList index-for-index (filled in BuildBspVobMapCacheHelper).
        // It can go stale when the game adds or removes a light at runtime, so each entry is checked
        // with one pointer compare; a miss falls back to the map and repairs the slot.
        const bool mirrorUsable = static_cast<int>(base->Lights.size()) == numLights;

        const bool dropStaticLights = Engine::GAPI->GetRendererState().RendererSettings.DisableStaticPointlights;

        // Portal culling for ambient lights: IsStatic() (== IsStaticVobLight) marks ZenGin's
        // pre-placed atmospheric fills - they never move, so a room no chain of active portals
        // reaches this frame is safe to skip outright; it picks back up the instant a door opens.
        // Dynamic/PFX lights (torches, spell effects, the player's own light) are NOT gated here:
        // they can be carried across sectors and their range test below already bounds the cost.
        const bool leafPortalVisible = !ctx.portalCuller || ctx.portalCuller->IsLeafVisible( *base );

        for ( int i = 0; i < numLights; i++ ) {
            zCVobLight* vob = leaf->LightVobList.Array[i];

            if ( dropStaticLights && vob->IsStatic() ) continue;

            if ( !leafPortalVisible && vob->IsStatic() ) continue;

            // Range test first - it needs nothing but the zCVobLight, and keeping it ahead of the
            // resolve below preserves the original rule that an out-of-range light never causes a
            // VobLightInfo (and possibly a shadow cube) to be allocated.
            // "distance + range < radius", squared to avoid a sqrt.
            const float lightRange = vob->GetLightRange();
            const float maxDist = visualFXDrawRadius - lightRange;
            if ( maxDist <= 0.0f )
                continue;

            float lightCameraDistSq;
            XMStoreFloat( &lightCameraDistSq, XMVector3LengthSq( cameraPosition - vob->GetPositionWorldXM() ) );
            if ( lightCameraDistSq >= maxDist * maxDist )
                continue;

            VobLightInfo* vi = nullptr;
            if ( mirrorUsable ) {
                VobLightInfo* cached = base->Lights[i];
                if ( cached && cached->Vob == vob )
                    vi = cached;
            }

            if ( !vi ) {
                auto vit = VobLightMap.find( vob );
                if ( vit == VobLightMap.end() ) {
                    // Add if not. This light must have been added during gameplay
                    VobLightInfo* nvi = new VobLightInfo;
                    nvi->Vob = vob;
                    bool PFXVobLight = false;

                    if ( zCVob* parent = vob->GetVobParent(); parent ) {
                        if ( auto visFx = parent->As<oCVisualFX>() ) {
                            PFXVobLight = true;
                            if (auto origin = visFx->GetOrigin()) {
                                // any PFX that stems from an ITEM should be counted as simple light.
                                PFXVobLight = !origin->As<oCItem>();
                            }
                        }
                    }

                    nvi->IsPFXVobLight = PFXVobLight;
                    nvi->UpdateShadows = true;
                    vit = VobLightMap.emplace( vob, nvi ).first;

                    // Create shadow-buffers for these lights since it was dynamically added to the world.
                    // PFX lights (candles/torches/campfires) get one too - they used to be excluded here
                    // outright because their origin can be anywhere in the vob tree (including NPCs/the
                    // player) with no reliable way to self-exclude it, which caused huge phantom shadows.
                    // D3D11PointLight now sidesteps that by restricting PFX casters to world mesh only (see
                    // RenderStaticShadowPass), so they can safely get shadows instead of none at all - the
                    // previous exclusion meant candles/torches never cast any shadow and their light bled
                    // straight through walls.
                    if ( rendererSettings.EnablePointlightShadows >= GothicRendererSettings::PLS_STATIC_ONLY ) {
                        BaseShadowedPointLight* bpl = nullptr;
                        Engine::GraphicsEngine->CreateShadowedPointLight( &bpl, nvi, true ); // Also flag as dynamic
                        nvi->LightShadowBuffers.reset(bpl);
                    }
                }
                vi = vit->second;

                // Only the main camera pass collects lights (every shadow path sets
                // CollectLights=false), so repairing the mirror here cannot race a worker thread.
                if ( mirrorUsable )
                    base->Lights[i] = vi;
            }

            // Dedup BEFORE the frustum test: that test is leaf-independent, so running it once per
            // light instead of once per (light, leaf) pair is pure profit. Safe because a leaf with
            // clipResult==CONTAINS holds the light's centre inside the frustum anyway, so the test
            // it skips could not have rejected the light either.
            if ( !visitor->Visit( vi ) ) continue;


            // Cull any lights that are not visible even though they are in range
            if ( clipResult != ContainmentType::CONTAINS) {
                BoundingSphere lightSphere;
                lightSphere.Center = vob->GetPositionWorld();
                lightSphere.Radius = lightRange;
                if ( !ctx.frustum.Intersects( lightSphere ) ) continue;
            }

            ctx.queue->PushLightVob( vi );
        }
    }
}

static void CollectVisibleVobsHelper( BspInfo* base,
    zTBBox3D boxCell,
    const RndCullContext& ctx,
    BspTreeVobVisitor* visitor,
    DirectX::ContainmentType inheritedContainment,
    float yMaxWorld
) {
    const float vobOutdoorDist = ctx.drawDistances.OutdoorVobs;
    const XMFLOAT3 camPos = ctx.cameraPosition;
    const XMVECTOR cameraPosition = XMLoadFloat3( &camPos );
    const bool enableOcclusionCulling = ctx.drawFlags.EnableOcclusionCulling;
    // See the identical note in CollectVisibleVobsWithLeafCache: with GPU culling the frustum may not reject
    // nodes. Forcing INTERSECTS (never CONTAINS) keeps CollectLeafVobs' per-light sphere test alive.
    const bool skipVobFrustumCull = ctx.drawFlags.SkipVobFrustumCull;
    while ( base->OriginalNode ) {
        // Check for occlusion-culling
        if ( enableOcclusionCulling && !base->OcclusionInfo.VisibleLastFrame ) {
            return;
        }

        zTBBox3D nodeBox = base->OriginalNode->BBox3D;
        float nodeYMax = std::min( yMaxWorld, camPos.y );
        nodeYMax = std::max( nodeYMax, base->OriginalNode->BBox3D.Max.y );
        nodeBox.Max.y = nodeYMax;

        float dist = Toolbox::ComputePointAABBDistance( camPos, base->OriginalNode->BBox3D.Min, base->OriginalNode->BBox3D.Max );
        ContainmentType clipResult = inheritedContainment;
        if ( dist < vobOutdoorDist ) {
            if ( skipVobFrustumCull ) {
                clipResult = ContainmentType::INTERSECTS;
            } else if ( !enableOcclusionCulling ) {
                if ( clipResult != ContainmentType::CONTAINS ) {
                    clipResult = ctx.frustum.Contains( Frustum::BBoxFromzTBBox3D( nodeBox ) );
                }
            } else {
                // If we are using occlusion-clipping, this test has already been done
                switch (static_cast<zTCam_ClipType>(base->OcclusionInfo.LastCameraClipType))
                {
                case zTCam_ClipType::ZTCAM_CLIPTYPE_IN:
                    clipResult = ContainmentType::CONTAINS; 
                    break;
                case zTCam_ClipType::ZTCAM_CLIPTYPE_CROSSING:
                    clipResult = ContainmentType::INTERSECTS; 
                    break;
                case zTCam_ClipType::ZTCAM_CLIPTYPE_OUT:
                    clipResult = ContainmentType::DISJOINT; 
                    break;
                }
            }

            if ( clipResult == ContainmentType::DISJOINT ) {
                return; // Nothig to see here. Discard this node and the subtree}
            }
        } else {
            // Too far
            return;
        }

        if ( base->OriginalNode->IsLeaf() ) {
            CollectLeafVobs( base, dist * dist, ctx, clipResult, visitor );
            return;
        } else {
            zCBspNode* node = static_cast<zCBspNode*>(base->OriginalNode);

            int	planeAxis = node->PlaneSignbits;

            boxCell.Min.y = node->BBox3D.Min.y;
            boxCell.Max.y = node->BBox3D.Min.y;

            zTBBox3D tmpbox = boxCell;
            float plane_normal;
            XMStoreFloat( &plane_normal, XMVector3Dot( XMLoadFloat3( &node->Plane.Normal ), cameraPosition ) );
            if ( plane_normal > node->Plane.Distance ) {
                if ( node->Front ) {
                    reinterpret_cast<float*>(&tmpbox.Min)[planeAxis] = node->Plane.Distance;
                    CollectVisibleVobsHelper( base->Front, tmpbox, ctx,
                        visitor,
                        clipResult,
                        yMaxWorld );
                }

                reinterpret_cast<float*>(&boxCell.Max)[planeAxis] = node->Plane.Distance;
                base = base->Back;
                inheritedContainment = clipResult;
            } else {
                if ( node->Back ) {
                    reinterpret_cast<float*>(&tmpbox.Max)[planeAxis] = node->Plane.Distance;
                    CollectVisibleVobsHelper( base->Back, tmpbox, ctx,
                        visitor,
                        clipResult,
                        yMaxWorld );
                }

                reinterpret_cast<float*>(&boxCell.Min)[planeAxis] = node->Plane.Distance;
                base = base->Front;
                inheritedContainment = clipResult;
            }
        }
    }
}
    
#ifdef __AVX2__
/** Batch-tests all pre-indexed BSP leaf AABBs against the current frustum using 8-wide AVX2 SIMD.
 *  Uses the p-vertex (positive-vertex) method: for each plane, the corner of the AABB most
 *  aligned with the plane normal is tested. If that corner is outside the plane, the whole
 *  AABB is outside. All 8 leaves are tested in parallel; surviving leaves are processed
 *  with CollectLeafVobs.  Requires a perspective (plane-cached) Frustum — checked by the caller.
 */
static void CollectVisibleVobsWithLeafCache(
    const RndCullContext& ctx,
    BspTreeVobVisitor* visitor
) {
    const BspLeafLinearCache& cache = Engine::GAPI->LeafLinearCache;
    if ( cache.Count == 0 ) return;

    const auto& planes = ctx.frustum.GetPlanes();

    // Broadcast all 6 plane components across 8 AVX2 lanes
    __m256 pNX[6], pNY[6], pNZ[6], pD[6];
    for ( int p = 0; p < 6; ++p ) {
        pNX[p] = _mm256_set1_ps( planes[p].x );
        pNY[p] = _mm256_set1_ps( planes[p].y );
        pNZ[p] = _mm256_set1_ps( planes[p].z );
        pD[p]  = _mm256_set1_ps( planes[p].w );
    }

    const XMFLOAT3& cp = ctx.cameraPosition;
    const float cpX = cp.x;
    const float cpY = cp.y;
    const float cpZ = cp.z;
    const __m256 vCamX = _mm256_set1_ps( cp.x );
    const __m256 vCamY = _mm256_set1_ps( cp.y );
    const __m256 vCamZ = _mm256_set1_ps( cp.z );

    const __m256 vDistSqThresh = _mm256_set1_ps( ctx.drawDistancesSq.OutdoorVobs );
    const __m256 vZero = _mm256_setzero_ps();

    const float* pMinX = cache.MinX.data();
    const float* pMinY = cache.MinY.data();
    const float* pMinZ = cache.MinZ.data();
    const float* pMaxX = cache.MaxX.data();
    const float* pMaxY = cache.MaxY.data();
    const float* pMaxZ = cache.MaxZ.data();

    const bool enableOcclusionCulling = ctx.drawFlags.EnableOcclusionCulling;
    // SkipVobFrustumCull (D3D12 GPU culling): leaves must NOT be rejected by the frustum any more — the whole
    // point is to hand the backend every in-range static VOB. Only the distance term below survives. Lights and
    // skeletal MOBs inside those leaves keep their own per-object frustum tests in CollectLeafVobs.
    const bool skipVobFrustumCull = ctx.drawFlags.SkipVobFrustumCull;
    const uint32_t padded = (cache.Count + 7u) & ~7u;

    for ( uint32_t i = 0; i < padded; i += 8 ) {
        // Load 8 leaf AABBs from the 32-byte-aligned SoA arrays
        const __m256 vMinX = _mm256_load_ps( pMinX + i );
        const __m256 vMinY = _mm256_load_ps( pMinY + i );
        const __m256 vMinZ = _mm256_load_ps( pMinZ + i );
        const __m256 vMaxX = _mm256_load_ps( pMaxX + i );
        const __m256 vMaxY = _mm256_load_ps( pMaxY + i );
        const __m256 vMaxZ = _mm256_load_ps( pMaxZ + i );

        // Frustum cull: n-vertex test across all 6 planes, plus the p-vertex test that upgrades a
        // surviving leaf from INTERSECTS to CONTAINS.
        // DirectX cached planes have OUTWARD-facing normals (positive dot = outside frustum),
        // matching FastIntersectAxisAlignedBoxPlane: Outside = (Dist > Radius).
        // blendv_ps(a, b, mask): MSB=0 -> a, MSB=1 (negative) -> b.
        //   n-vertex (MINIMUM dot) = blendv(Min, Max, n); dot > 0 => whole AABB outside, leaf rejected.
        //   p-vertex (MAXIMUM dot) = the same with the operands swapped; dot <= 0 for all six planes =>
        //     the leaf is fully CONTAINED and nothing inside it needs its own frustum test.
        __m256 vOutside = vZero;
        __m256 vNotContained = vZero;
        if ( !skipVobFrustumCull ) {
            for ( int p = 0; p < 6; ++p ) {
                const __m256 vNX = _mm256_blendv_ps( vMinX, vMaxX, pNX[p] );
                const __m256 vNY = _mm256_blendv_ps( vMinY, vMaxY, pNY[p] );
                const __m256 vNZ = _mm256_blendv_ps( vMinZ, vMaxZ, pNZ[p] );
                const __m256 vNDot = _mm256_fmadd_ps( pNX[p], vNX,
                                     _mm256_fmadd_ps( pNY[p], vNY,
                                     _mm256_fmadd_ps( pNZ[p], vNZ, pD[p] ) ) );
                vOutside = _mm256_or_ps( vOutside, _mm256_cmp_ps( vNDot, vZero, _CMP_GT_OQ ) );

                // The p-vertex differs only in which extent each axis picks, so it reuses the already-loaded
                // Min/Max registers.
                const __m256 vPX = _mm256_blendv_ps( vMaxX, vMinX, pNX[p] );
                const __m256 vPY = _mm256_blendv_ps( vMaxY, vMinY, pNY[p] );
                const __m256 vPZ = _mm256_blendv_ps( vMaxZ, vMinZ, pNZ[p] );
                const __m256 vPDot = _mm256_fmadd_ps( pNX[p], vPX,
                                     _mm256_fmadd_ps( pNY[p], vPY,
                                     _mm256_fmadd_ps( pNZ[p], vPZ, pD[p] ) ) );
                vNotContained = _mm256_or_ps( vNotContained, _mm256_cmp_ps( vPDot, vZero, _CMP_GT_OQ ) );
            }
        } else {
            // GPU culling: no frustum rejection at all, and deliberately never CONTAINS - CollectLeafVobs'
            // per-light sphere test has to stay alive (see the note above).
            vNotContained = _mm256_cmp_ps( vZero, vZero, _CMP_EQ_OQ );
        }

        // Distance cull: squared AABB-to-point distance
        const __m256 vDX = _mm256_max_ps( vZero, _mm256_max_ps(
            _mm256_sub_ps( vMinX, vCamX ), _mm256_sub_ps( vCamX, vMaxX ) ) );
        const __m256 vDY = _mm256_max_ps( vZero, _mm256_max_ps(
            _mm256_sub_ps( vMinY, vCamY ), _mm256_sub_ps( vCamY, vMaxY ) ) );
        const __m256 vDZ = _mm256_max_ps( vZero, _mm256_max_ps(
            _mm256_sub_ps( vMinZ, vCamZ ), _mm256_sub_ps( vCamZ, vMaxZ ) ) );
        const __m256 vDistSq = _mm256_fmadd_ps( vDX, vDX,
                               _mm256_fmadd_ps( vDY, vDY,
                               _mm256_mul_ps(   vDZ, vDZ ) ) );
        vOutside = _mm256_or_ps( vOutside, _mm256_cmp_ps( vDistSq, vDistSqThresh, _CMP_GE_OQ ) );

        const int cullMask = _mm256_movemask_ps( vOutside );
        if ( cullMask == 0xFF ) continue; // All 8 culled — skip scalar work

        // Bit set => that lane's leaf is only partially inside, so its contents keep their own tests.
        const int partialMask = _mm256_movemask_ps( vNotContained );

        // Store the already-computed distances instead of recomputing them per surviving lane.
        alignas( 32 ) float laneDistSq[8];
        _mm256_store_ps( laneDistSq, vDistSq );

        // Process surviving lanes with full scalar logic
        for ( int lane = 0; lane < 8; ++lane ) {
            if ( cullMask & (1 << lane) ) continue;

            const uint32_t idx = i + static_cast<uint32_t>( lane );
            if ( idx >= cache.Count ) break; // past padding sentinel entries

            BspInfo* leaf = cache.Leaves[idx];
            if ( !leaf ) continue;

            // Occlusion culling gate (GPU query result from prior frame)
            if ( enableOcclusionCulling && !leaf->OcclusionInfo.VisibleLastFrame )
                continue;

            // CONTAINS when the leaf box is fully inside all six planes: everything in it then skips its own
            // frustum test in CollectLeafVobs. A VOB poking out of a contained leaf is kept, not dropped.
            const ContainmentType containment = ( partialMask & (1 << lane) )
                ? ContainmentType::INTERSECTS
                : ContainmentType::CONTAINS;

            CollectLeafVobs( leaf, laneDistSq[lane], ctx, containment, visitor );
        }
    }
}
#endif // __AVX2__

void GothicAPI::CollectVisibleVobs( const RndCullContext& ctx ) {
    zCBspTree* tree = LoadedWorldInfo->BspTree;

    zCBspBase* rootBsp = tree->GetRootNode();
    BspInfo* root = &BspLeafVobLists[rootBsp];

    thread_local BspTreeVobVisitor bspVobVisitor{};

    // Use the flat SIMD leaf cache when available (perspective frustum + cache built at world load).
    // Falls back to the pointer-chasing recursive tree walk for sphere/OBB frustums (shadow cubemaps,
    // orthographic shadow maps) or before the first world is loaded.
#ifdef __AVX2__
    if ( LeafLinearCache.Count > 0 && ctx.frustum.UsesPlaneFrustum() ) {
        ZoneScopedN( "GothicAPI::CollectVisibleVobsWithLeafCache" );
        CollectVisibleVobsWithLeafCache( ctx, &bspVobVisitor );
        ZoneText( "vobs", std::size( "vobs" ) - 1 );
        ZoneValue( bspVobVisitor.GetSeenVobs() );
        ZoneText( "mobs", std::size( "mobs" ) - 1 );
        ZoneValue( bspVobVisitor.GetSeenMobs() );
        ZoneText( "lights", std::size( "lights" ) - 1 );
        ZoneValue( bspVobVisitor.GetSeenLights() );
    } else
#endif
    {
        ZoneScopedN( "GothicAPI::CollectVisibleVobsHelper" );
        // Recursively go through the tree and draw all nodes
        CollectVisibleVobsHelper( root, root->OriginalNode->BBox3D,
            ctx,
            &bspVobVisitor,
            ContainmentType::INTERSECTS,
            Engine::GAPI->GetLoadedWorldInfo()->BspTree->GetRootNode()->BBox3D.Max.y
        );
        ZoneText( "vobs", std::size( "vobs" ) - 1 );
        ZoneValue( bspVobVisitor.GetSeenVobs() );
        ZoneText( "mobs", std::size( "mobs" ) - 1 );
        ZoneValue( bspVobVisitor.GetSeenMobs() );
        ZoneText( "lights", std::size( "lights" ) - 1 );
        ZoneValue( bspVobVisitor.GetSeenLights() );
    }

    FXMVECTOR camPos = XMLoadFloat3( &ctx.cameraPosition );
    const float vobIndoorDist = ctx.drawDistances.IndoorVobs;
    const float vobOutdoorDist = ctx.drawDistances.OutdoorVobs;
    const float vobOutdoorSmallDist = ctx.drawDistances.OutdoorVobsSmall;
    const float vobSmallSize = RendererState.RendererSettings.SmallVobSize;
    bool collectIndoor = ctx.stage != RenderStage::STAGE_DRAW_SHADOWS;
    // SkipVobFrustumCull (D3D12 GPU culling): distance-only, like CVVH_AddNotDrawnVobToList above.
    auto cullingEnabled = RendererState.RendererSettings.DebugSettings.Culling.CullVobs
        && !ctx.drawFlags.SkipVobFrustumCull;

    // Add visible dynamically added vobs
    if ( RendererState.RendererSettings.DrawVOBs ) {
        float dist;
        for ( VobInfo* it : DynamicallyAddedVobs ) {
            if ( !bspVobVisitor.Visit( it ) ) continue;

            // Get distance to this vob
            XMStoreFloat( &dist, XMVector3Length( camPos - it->Vob->GetPositionWorldXM() ) );
            // Draw, if in range
            if ( it->VisualInfo && (
                (dist < vobIndoorDist && it->IsIndoorVob && collectIndoor)
                || (!it->IsIndoorVob && (
                    (dist < vobOutdoorSmallDist && it->VisualInfo->MeshSize < vobSmallSize)
                    || (dist < vobOutdoorDist)
                    )
                    )
                ) ) {

                if ( !it->Vob->GetShowVisual() ) {
                    continue;
                }

                if ( cullingEnabled && !ctx.frustum.Intersects( it->Vob->GetBBox() ) ) {
                    continue;
                }

                if ( it->Vob->GetVisualAlpha() ) {
                    ctx.queue->PushTransparencyVob( TransparencyVobInfo{ dist, it->Vob->GetVobTransparency(), nullptr, it } );
                    continue;
                }

                ctx.queue->PushStaticVob( it );
            }
        }
    }

    bspVobVisitor.ClearForReuse();
}
