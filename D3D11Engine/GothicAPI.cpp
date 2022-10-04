#include <Windows.h>
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
#include <dinput.h>
#include "BaseAntTweakBar.h"
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
#include "GOcean.h"
#include "zCVobLight.h"
#include "zCQuadMark.h"
#include "zCOption.h"
#include "zCRndD3D.h"
#include "win32ClipboardWrapper.h"
#include "zCSoundSystem.h"
#include "zCView.h"

// Duration how long the scene will stay wet, in MS
const DWORD SCENE_WETNESS_DURATION_MS = 30 * 1000;

// Draw ghost from back to front of our camera
auto CompareGhostDistance = []( std::pair<float, SkeletalVobInfo*>& a, std::pair<float, SkeletalVobInfo*>& b ) -> bool { return a.first < b.first; };

/** Writes this info to a file */
void MaterialInfo::WriteToFile( const std::string& name ) {
    FILE* f = fopen( ("system\\GD3D11\\textures\\infos\\" + name + ".mi").c_str(), "wb" );

    if ( !f ) {
        LogError() << "Failed to open file '" << ("system\\GD3D11\\textures\\infos\\" + name + ".mi") << "' for writing! Make sure the game runs in Admin mode "
            " to get the rights to write to that directory!";

        return;
    }

    // Write the version first
    fwrite( &MATERIALINFO_VERSION, sizeof( MATERIALINFO_VERSION ), 1, f );

    // Then the data
    fwrite( &buffer, sizeof( MaterialInfo::Buffer ), 1, f );
#if ENABLE_TESSELATION > 0
    fwrite( &TextureTesselationSettings.buffer, sizeof( VisualTesselationSettings::Buffer ), 1, f );
#endif

    fclose( f );
}

/** Loads this info from a file */
void MaterialInfo::LoadFromFile( const std::string& name ) {
    FILE* f = fopen( ("system\\GD3D11\\textures\\infos\\" + name + ".mi").c_str(), "rb" );

    if ( !f )
        return;

#if ENABLE_TESSELATION > 0
    char ReadBuffer[sizeof( int ) + sizeof( MaterialInfo::Buffer ) + sizeof( VisualTesselationSettings::Buffer )];
#else
    char ReadBuffer[sizeof( int ) + sizeof( MaterialInfo::Buffer )];
#endif
    fread( ReadBuffer, 1, sizeof( ReadBuffer ), f );

    // Write the version first
    int version;
    memcpy( &version, ReadBuffer, sizeof( int ) );

    // Then the data
    ZeroMemory( &buffer, sizeof( MaterialInfo::Buffer ) );
    memcpy( &buffer, ReadBuffer + sizeof( int ), sizeof( MaterialInfo::Buffer ) );

    if ( version < 2 ) {
        if ( buffer.DisplacementFactor == 0.0f ) {
            buffer.DisplacementFactor = 0.7f;
        }
    }

#if ENABLE_TESSELATION > 0
    if ( version >= 4 ) {
        memcpy( &TextureTesselationSettings.buffer, ReadBuffer + sizeof( int ) + sizeof( MaterialInfo::Buffer ), sizeof( VisualTesselationSettings::Buffer ) );
    }
#endif

    fclose( f );

    buffer.Color = float4( 1, 1, 1, 1 );

    UpdateConstantbuffer();
#if ENABLE_TESSELATION > 0
    TextureTesselationSettings.UpdateConstantbuffer();
#endif
}

/** creates/updates the constantbuffer */
void MaterialInfo::UpdateConstantbuffer() {
    if ( Constantbuffer ) {
        Constantbuffer->UpdateBuffer( &buffer );
    } else {
        Engine::GraphicsEngine->CreateConstantBuffer( &Constantbuffer, &buffer, sizeof( buffer ) );
    }
}

GothicAPI::GothicAPI() {
    OriginalGothicWndProc = 0;

    TextureTestBindMode = false;

    ZeroMemory( BoundTextures, sizeof( BoundTextures ) );

    CameraReplacementPtr = nullptr;
    WrappedWorldMesh = nullptr;
    Ocean = nullptr;
    CurrentCamera = nullptr;

    MainThreadID = GetCurrentThreadId();

    _canRain = false;
    _canClearVobsByVisual = false;
}

GothicAPI::~GothicAPI() {
    //ResetWorld(); // Just let it leak for now. // TODO: Do this properly
    SAFE_DELETE( WrappedWorldMesh );
}

float GetPrivateProfileFloatA(
    const LPCSTR lpAppName,
    const LPCSTR lpKeyName,
    const float nDefault,
    const std::string& lpFileName
) {
    const int float_str_max = 30;
    TCHAR nFloat[float_str_max];
    if ( GetPrivateProfileStringA( lpAppName, lpKeyName, nullptr, nFloat, float_str_max, lpFileName.c_str() ) ) {
        try {
            return std::stof( std::string( nFloat ) );
        } catch ( const std::exception& ) {
            return nDefault;
        }
    }
    return nDefault;
}

std::string GetPrivateProfileStringA(
    const LPCSTR lpAppName,
    const LPCSTR lpKeyName,
    const std::string& lpcstrDefault,
    const std::string& lpFileName ) {
    char buffer[MAX_PATH];
    GetPrivateProfileStringA( lpAppName, lpKeyName, lpcstrDefault.c_str(), buffer, MAX_PATH, lpFileName.c_str() );
    return std::string( buffer );
}

bool GetPrivateProfileBoolA(
    const LPCSTR lpAppName,
    const LPCSTR lpKeyName,
    const bool nDefault,
    const std::string& lpFileName ) {
    return GetPrivateProfileIntA( lpAppName, lpKeyName, nDefault, lpFileName.c_str() ) ? true : false;
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

    // It can lead to dead-lock so it is force-disabled until it is investigated
    if ( zCResourceManager* rsm = zCResourceManager::GetResourceManager() ) {
        rsm->SetThreadingEnabled( false );
        //rsm->SetThreadingEnabled( RendererState.RendererSettings.MTResoureceManager );
    }
}

/** Called to update the texture quality */
void GothicAPI::UpdateTextureMaxSize() {
    reinterpret_cast<void( __cdecl* )(int)>(GothicMemoryLocations::zCResourceManager::RefreshTexMaxSize)(RendererState.RendererSettings.textureMaxSize);
    if ( zCResourceManager* rsm = zCResourceManager::GetResourceManager() ) {
        rsm->PurgeCaches( GothicMemoryLocations::zCClassDef::zCTexture );
    }
}

/** Called to update the world, before rendering */
void GothicAPI::OnWorldUpdate() {
#if BUILD_SPACER
    zCBspBase* rootBsp = oCGame::GetGame()->_zCSession_world->GetBspTree()->GetRootNode();
    BspInfo* root = &BspLeafVobLists[rootBsp];

    if ( !root->OriginalNode )
        Engine::GAPI->OnWorldLoaded();
#endif
#if BUILD_SPACER_NET
    if ( RendererState.RendererSettings.RunInSpacerNet ) {
        zCBspBase* rootBsp = oCGame::GetGame()->_zCSession_world->GetBspTree()->GetRootNode();
        BspInfo* root = &BspLeafVobLists[rootBsp];

        if ( !root->OriginalNode )
            Engine::GAPI->OnWorldLoaded();
    }
#endif

    RendererState.RendererInfo.Reset();
    RendererState.RendererInfo.FPS = GetFramesPerSecond();
    RendererState.GraphicsState.FF_Time = GetTimeSeconds();

    if ( zCCamera* camera = zCCamera::GetCamera() ) {
        RendererState.RendererInfo.FarPlane = camera->GetFarPlane();
        RendererState.RendererInfo.NearPlane = camera->GetNearPlane();

        //zCCamera::GetCamera()->Activate();
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

        if( !RendererState.RendererSettings.EnableRain || !outdoor ) {
            #ifdef BUILD_GOTHIC_1_08k
            #ifdef BUILD_1_12F
            int skyEffects = *reinterpret_cast<int*>(0x887EDC);
            *reinterpret_cast<int*>(0x887EDC) = 0;
            skyController->ProcessRainFX();
            *reinterpret_cast<int*>(0x887EDC) = skyEffects;
            #else
            int skyEffects = *reinterpret_cast<int*>(0x8422A0);
            *reinterpret_cast<int*>(0x8422A0) = 0;
            skyController->ProcessRainFX();
            *reinterpret_cast<int*>(0x8422A0) = skyEffects;
            #endif
            #endif
            #ifdef BUILD_GOTHIC_2_6_fix
            int skyEffects = *reinterpret_cast<int*>(0x8A5DB0);
            *reinterpret_cast<int*>(0x8A5DB0) = 0;
            skyController->ProcessRainFX();
            *reinterpret_cast<int*>(0x8A5DB0) = skyEffects;
            #endif
        } else {
            #ifdef BUILD_GOTHIC_1_08k
            #ifdef BUILD_1_12F
            int skyEffects = *reinterpret_cast<int*>(0x887EDC);
            *reinterpret_cast<int*>(0x887EDC) = 1;
            skyController->ProcessRainFX();
            *reinterpret_cast<int*>(0x887EDC) = skyEffects;
            #else
            int skyEffects = *reinterpret_cast<int*>(0x8422A0);
            *reinterpret_cast<int*>(0x8422A0) = 1;
            skyController->ProcessRainFX();
            *reinterpret_cast<int*>(0x8422A0) = skyEffects;
            #endif
            #endif
            #ifdef BUILD_GOTHIC_2_6_fix
            int skyEffects = *reinterpret_cast<int*>(0x8A5DB0);
            *reinterpret_cast<int*>(0x8A5DB0) = 1;
            skyController->ProcessRainFX();
            *reinterpret_cast<int*>(0x8A5DB0) = skyEffects;
            #endif
        }
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
    if ( !oCGame::GetGame() || !oCGame::GetGame()->_zCSession_camVob || !oCGame::GetGame()->_zCSession_camVob->GetGroundPoly() )
        return false;

    return oCGame::GetGame()->_zCSession_camVob->GetGroundPoly()->GetLightmap() != nullptr;
}

/** Returns total time */
float GothicAPI::GetTotalTime() {
    if ( zCTimer::GetTimer() )
        return zCTimer::GetTimer()->totalTimeFloat;

    return 0.0f;
}

/** Returns global time */
float GothicAPI::GetTimeSeconds() {
#ifdef BUILD_GOTHIC_1_08k
    if ( zCTimer::GetTimer() )
        return zCTimer::GetTimer()->totalTimeFloat / 1000.0f; // Gothic 1 has this in seconds
#else
    if ( zCTimer::GetTimer() )
        return zCTimer::GetTimer()->totalTimeFloatSecs;
#endif

    return 0.0f;
}

/** Returns the current frame time */
float GothicAPI::GetFrameTimeSec() {
#ifdef BUILD_GOTHIC_1_08k
    if ( zCTimer::GetTimer() )
        return zCTimer::GetTimer()->frameTimeFloat / 1000.0f;
#else
    if ( zCTimer::GetTimer() )
        return zCTimer::GetTimer()->frameTimeFloatSecs;
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
    WorldSections.clear();

    ResetVobs();

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

    ParticleEffectVobs.clear();
    RegisteredVobs.clear();
    BspLeafVobLists.clear();
    DynamicallyAddedVobs.clear();
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
    SkeletalMeshVisuals.clear();

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

    // Delete light vobs
    for ( auto const& it : VobLightMap ) {
        Engine::GraphicsEngine->OnVobRemovedFromWorld( it.first );
        delete it.second;
    }
    VobLightMap.clear();
}

/** Called when the game loaded a new level */
void GothicAPI::OnGeometryLoaded( zCPolygon** polys, unsigned int numPolygons ) {
    LogInfo() << "Extracting world";

    ResetWorld();
    ResetMaterialInfo();

    bool indoorLocation = (LoadedWorldInfo->BspTree->GetBspTreeMode() == zBSP_MODE_INDOOR);
    std::string worldStr = "system\\GD3D11\\meshes\\WLD_" + LoadedWorldInfo->WorldName + ".obj";
    // Convert world to our own format
#ifdef BUILD_GOTHIC_2_6_fix
    WorldConverter::ConvertWorldMesh( polys, numPolygons, &WorldSections, LoadedWorldInfo.get(), &WrappedWorldMesh, indoorLocation );
#else
    if ( Toolbox::FileExists( worldStr ) ) {
        WorldConverter::LoadWorldMeshFromFile( worldStr, &WorldSections, LoadedWorldInfo.get(), &WrappedWorldMesh );
        LoadedWorldInfo->CustomWorldLoaded = true;
    } else {
        WorldConverter::ConvertWorldMesh( polys, numPolygons, &WorldSections, LoadedWorldInfo.get(), &WrappedWorldMesh, indoorLocation );
    }
#endif
    LogInfo() << "Done extracting world!";

#if ENABLE_TESSELATION > 0
    // Apply tesselation
    for ( auto const& it : LoadedMaterials ) {
        MaterialInfo* info = GetMaterialInfoFrom( it->GetTexture() );
        if ( info ) {
            ApplyTesselationSettingsForAllMeshPartsUsing( info, info->TextureTesselationSettings.buffer.VT_TesselationFactor > 1.0f ? 2 : 1 );
        }
    }
#endif
}

/** Called when the game is about to load a new level */
void GothicAPI::OnLoadWorld( const std::string& levelName, int loadMode ) {
    _canClearVobsByVisual = true;
    if ( (loadMode == 0 || loadMode == 2) && !levelName.empty() ) {
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

    LoadRendererWorldSettings( RendererState.RendererSettings );
    SaveRendererWorldSettings( RendererState.RendererSettings );
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

void GothicAPI::LoadRendererWorldSettings( GothicRendererSettings& s ) {
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

    s.FogColorMod = float3::FromColor(
        GetPrivateProfileIntA( "Atmoshpere", "FogColorModR", static_cast<int>(s.FogColorMod.x * 255.0f), ini.c_str() ),
        GetPrivateProfileIntA( "Atmoshpere", "FogColorModG", static_cast<int>(s.FogColorMod.y * 255.0f), ini.c_str() ),
        GetPrivateProfileIntA( "Atmoshpere", "FogColorModB", static_cast<int>(s.FogColorMod.z * 255.0f), ini.c_str() )
    );

    if ( !GMPModeActive ) {
        s.VisualFXDrawRadius = GetPrivateProfileFloatA( "General", "VisualFXDrawRadius", s.VisualFXDrawRadius, ini );
        s.OutdoorVobDrawRadius = GetPrivateProfileFloatA( "General", "OutdoorVobDrawRadius", s.OutdoorVobDrawRadius, ini );
        s.OutdoorSmallVobDrawRadius = GetPrivateProfileFloatA( "General", "OutdoorSmallVobDrawRadius", s.OutdoorSmallVobDrawRadius, ini );
        s.SectionDrawRadius = GetPrivateProfileFloatA( "General", "SectionDrawRadius", s.SectionDrawRadius, ini );
    }

    s.ReplaceSunDirection = GetPrivateProfileBoolA( "Atmoshpere", "ReplaceSunDirection", s.ReplaceSunDirection, ini );

    AtmosphereSettings& aS = GetSky()->GetAtmoshpereSettings();

    aS.LightDirection = XMFLOAT3(
        GetPrivateProfileFloatA( "Atmoshpere", "LightDirectionX", aS.LightDirection.x, ini ),
        GetPrivateProfileFloatA( "Atmoshpere", "LightDirectionY", aS.LightDirection.y, ini ),
        GetPrivateProfileFloatA( "Atmoshpere", "LightDirectionZ", aS.LightDirection.z, ini )
    );
}

void GothicAPI::SaveRendererWorldSettings( const GothicRendererSettings& s ) {
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

    WritePrivateProfileStringA( "Fog", "Height", std::to_string( s.FogHeight ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "Fog", "HeightFalloff", std::to_string( s.FogHeightFalloff ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "Fog", "GlobalDensity", std::to_string( s.FogGlobalDensity ).c_str(), ini.c_str() );

    WritePrivateProfileStringA( "Atmoshpere", "SunLightColorR", std::to_string( static_cast<int>(s.SunLightColor.x * 255.0f) ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "Atmoshpere", "SunLightColorG", std::to_string( static_cast<int>(s.SunLightColor.y * 255.0f) ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "Atmoshpere", "SunLightColorB", std::to_string( static_cast<int>(s.SunLightColor.z * 255.0f) ).c_str(), ini.c_str() );

    WritePrivateProfileStringA( "Atmoshpere", "FogColorModR", std::to_string( static_cast<int>(s.FogColorMod.x * 255.0f) ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "Atmoshpere", "FogColorModG", std::to_string( static_cast<int>(s.FogColorMod.y * 255.0f) ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "Atmoshpere", "FogColorModB", std::to_string( static_cast<int>(s.FogColorMod.z * 255.0f) ).c_str(), ini.c_str() );

    WritePrivateProfileStringA( "General", "VisualFXDrawRadius", std::to_string( s.VisualFXDrawRadius ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "General", "OutdoorVobDrawRadius", std::to_string( s.OutdoorVobDrawRadius ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "General", "OutdoorSmallVobDrawRadius", std::to_string( s.OutdoorSmallVobDrawRadius ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "General", "SectionDrawRadius", std::to_string( s.SectionDrawRadius ).c_str(), ini.c_str() );

    WritePrivateProfileStringA( "Atmoshpere", "ReplaceSunDirection", std::to_string( s.ReplaceSunDirection ? TRUE : FALSE ).c_str(), ini.c_str() );

    AtmosphereSettings& aS = GetSky()->GetAtmoshpereSettings();

    WritePrivateProfileStringA( "Atmoshpere", "LightDirectionX", std::to_string( aS.LightDirection.x ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "Atmoshpere", "LightDirectionY", std::to_string( aS.LightDirection.y ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "Atmoshpere", "LightDirectionZ", std::to_string( aS.LightDirection.z ).c_str(), ini.c_str() );
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

/** Draws the world-mesh */
void GothicAPI::DrawWorldMeshNaive() {
    if ( !zCCamera::GetCamera() || !oCGame::GetGame() )
        return;

    static float setfovH = RendererState.RendererSettings.FOVHoriz;
    static float setfovV = RendererState.RendererSettings.FOVVert;

/*
#ifdef BUILD_GOTHIC_1_08k
    if ( RendererState.RendererSettings.ForceFOV ) {
        setfovH = RendererState.RendererSettings.FOVHoriz;
        setfovV = RendererState.RendererSettings.FOVVert;

        // Fix camera FOV-Bug
        zCCamera::GetCamera()->SetFOV( RendererState.RendererSettings.FOVHoriz, (Engine::GraphicsEngine->GetResolution().y / static_cast<float>(Engine::GraphicsEngine->GetResolution().x)) * RendererState.RendererSettings.FOVVert );

        CurrentCamera = zCCamera::GetCamera();
    }
#else
*/
#if defined(BUILD_GOTHIC_1_08k) || defined(BUILD_1_12F) || defined(BUILD_GOTHIC_2_6_fix)
    if ( RendererState.RendererSettings.ForceFOV ) {
        zCCamera* camera = zCCamera::GetCamera();
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

    START_TIMING();
    Engine::GraphicsEngine->DrawWorldMesh();
    STOP_TIMING( GothicRendererTiming::TT_WorldMesh );

    for ( auto const& vegetationBox : VegetationBoxes ) {
        vegetationBox->RenderVegetation( GetCameraPosition() );
    }

    START_TIMING();
    if ( RendererState.RendererSettings.DrawSkeletalMeshes ) {
        // Set up frustum for the camera
        RendererState.RasterizerState.SetDefault();
        RendererState.RasterizerState.SetDirty();
        zCCamera::GetCamera()->Activate();

        for ( const auto& vobInfo : AnimatedSkeletalVobs ) {
            // Don't render if sleeping and has skeletal meshes available
            if ( !vobInfo->VisualInfo ) continue;

            float dist;
            XMStoreFloat( &dist, XMVector3Length( vobInfo->Vob->GetPositionWorldXM() - GetCameraPositionXM() ) );
            if ( dist > RendererState.RendererSettings.SkeletalMeshDrawRadius )
                continue; // Skip out of range

            const zTBBox3D bb = vobInfo->Vob->GetBBoxLocal();
            zCCamera::GetCamera()->SetTransform( zCCamera::ETransformType::TT_WORLD, *vobInfo->Vob->GetWorldMatrixPtr() );

            //Engine::GraphicsEngine->GetLineRenderer()->AddAABBMinMax(bb.Min, bb.Max, XMFLOAT4(1, 1, 1, 1));

            int clipFlags = 15; // No far clip
            if ( zCCamera::GetCamera()->BBox3DInFrustum( bb, clipFlags ) == ZTCAM_CLIPTYPE_OUT )
                continue;

            // Indoor?
            vobInfo->IndoorVob = vobInfo->Vob->IsIndoorVob();

            zCModel* model = static_cast<zCModel*>(vobInfo->Vob->GetVisual());
            if ( !model )
                continue; // Gothic fortunately sets this to 0 when it throws the model out of the cache

            // This is important, because gothic only lerps between animation when this distance is set and below ~2000
            model->SetDistanceToCamera( dist );

            // Schedule for drawing in later stage if this vob is ghost
            if ( oCNPC* npc = vobInfo->Vob->As<oCNPC>() ) {
                if ( npc->HasFlag( NPC_FLAG_GHOST ) ) {
                    GhostSkeletalVobs.emplace_back( dist, vobInfo );
                    std::push_heap( GhostSkeletalVobs.begin(), GhostSkeletalVobs.end(), CompareGhostDistance );
                    continue;
                }
            }

            DrawSkeletalMeshVob( vobInfo, dist );
            if( RendererState.RendererSettings.ShowSkeletalVertexNormals )
                VNSkeletalVobs.emplace_back( vobInfo );
        }
    }
    STOP_TIMING( GothicRendererTiming::TT_SkeletalMeshes );

    // Draw vobs in view
    Engine::GraphicsEngine->DrawVOBs();

    //DebugDrawBSPTree();

    ResetWorldTransform();
}

/** Draws particles, in a simple way */
void GothicAPI::DrawParticlesSimple() {
    ParticleFrameData data;

    if ( RendererState.RendererSettings.DrawParticleEffects ) {
        std::vector<zCVob*> renderedParticleFXs;
        zCCamera::GetCamera()->Activate();
        GetVisibleParticleEffectsList( renderedParticleFXs );

        // now it is save to render
        for ( auto const& it : renderedParticleFXs ) {
            const zCVisual* vis = it->GetVisual();
            if ( vis ) {
                DrawParticleFX( it, reinterpret_cast<zCParticleFX*>(const_cast<zCVisual*>(vis)), data );
            }
        }

        Engine::GraphicsEngine->DrawFrameParticleMeshes( ParticleEffectProgMeshes );
        Engine::GraphicsEngine->DrawFrameParticles( FrameParticles, FrameParticleInfo );
    }
}

// Converts poly strip visuals to render ready geometry
void GothicAPI::CalcPolyStripMeshes() {

    ExVertexStruct polyFan[4];
    PolyStripInfos.clear();

    for ( const auto& pStrip : PolyStripVisuals ) {

        if ( !pStrip ) return;

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
            }
#endif

            //Convert list of quads to list of triangles
            WorldConverter::TriangleFanToList( &polyFan[0], 4, &PolyStripInfos[tx].vertices );
            PolyStripInfos[tx].material = mat;
        }

    }
};

/** Returns a list of visible particle-effects */
void GothicAPI::GetVisibleParticleEffectsList( std::vector<zCVob*>& pfxList ) {
    if ( RendererState.RendererSettings.DrawParticleEffects ) {
        FXMVECTOR camPos = GetCameraPositionXM();

        // now it is save to render
        float dist;
        for ( auto const& it : ParticleEffectVobs ) {
            XMStoreFloat( &dist, XMVector3Length( it->GetPositionWorldXM() - camPos ) );
            if ( dist > RendererState.RendererSettings.VisualFXDrawRadius )
                continue;

            int flags = 15; // Frustum check, no farplane
            if ( zCCamera::GetCamera()->BBox3DInFrustum( it->GetBBox(), flags ) == ZTCAM_CLIPTYPE_OUT ) {
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
    FXMVECTOR camPos = GetCameraPositionXM();
    static std::vector<std::pair<zCVob*, float>> decalDistances; // Static to get around reallocations

    float dist;
    for ( auto const& it : DecalVobs ) {
        XMStoreFloat( &dist, XMVector3Length( it->GetPositionWorldXM() - camPos ) );
        if ( dist > RendererState.RendererSettings.VisualFXDrawRadius )
            continue;

        int flags = 15; // Frustum check, no farplane
        if ( zCCamera::GetCamera()->BBox3DInFrustum( it->GetBBox(), flags ) == ZTCAM_CLIPTYPE_OUT ) {
            continue;
        }

        if ( it->GetVisual() && it->GetShowVisual() ) {
            decalDistances.push_back( std::make_pair( it, dist ) );
        }
    }

    // Sort back to front
    std::sort( decalDistances.begin(), decalDistances.end(), DecalSortcmpFunc );

    // Put into output list
    for ( auto const& it : decalDistances ) {
        decals.push_back( it.first );
    }

    decalDistances.clear();
}

/** Called when a material got removed */
void GothicAPI::OnMaterialDeleted( zCMaterial* mat ) {
    LoadedMaterials.erase( mat );
    if ( !mat )
        return;
    for ( auto&& it : SkeletalMeshVisuals ) {
        it.second->Meshes.erase( mat );
        it.second->SkeletalMeshes.erase( mat );
    }
}

/** Called when a material got created */
void GothicAPI::OnMaterialCreated( zCMaterial* mat ) {
    LoadedMaterials.insert( mat );
}

/** Returns if the material is currently active */
bool GothicAPI::IsMaterialActive( zCMaterial* mat ) {
    std::set<zCMaterial*>::iterator it = LoadedMaterials.find( mat );
    if ( it != LoadedMaterials.end() ) {
        return true;
    }

    return false;
}

/** Called when a vob moved */
void GothicAPI::OnVobMoved( zCVob* vob ) {
    auto checkMatrix = []( XMMATRIX& a, XMMATRIX& b ) -> bool {
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

        vi->UpdateVobConstantBuffer();
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
        }
    }
}

/** Called when a visual got removed */
void GothicAPI::OnVisualDeleted( zCVisual* visual ) {
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
        for ( auto it = PolyStripVisuals.begin(); it != PolyStripVisuals.end(); it++ ) {
            if ( *it == reinterpret_cast<zCPolyStrip*>(visual) ) {
                PolyStripVisuals.erase( *it );
            }
        }
    }

    // Check every extension
    for ( unsigned int i = 0; i < extv.size(); i++ ) {
        std::string ext = extv[i];

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
                std::string str = zmodel->GetVisualName();
                if ( str.empty() ) { // Happens when the model has no skeletal-mesh
                    zSTRING mds = zmodel->GetModelName();
                    str = mds.ToChar();
                    mds.Delete();
                }

                auto it = SkeletalMeshVisuals.find( str );
                if ( it != SkeletalMeshVisuals.end() ) {
                    // Find vobs using this visual
                    for ( SkeletalVobInfo* vobInfo : SkeletalMeshVobs ) {
                        if ( vobInfo->VisualInfo == it->second ) {
                            vobInfo->VisualInfo = nullptr;
                        }
                    }
                }

                delete SkeletalMeshVisuals[str];
                SkeletalMeshVisuals.erase( str );
            }
            break;
        }
    }

    // Clear
    std::list<BaseVobInfo*> list = VobsByVisual[visual];
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
            LogInfo() << std::string( className ) << " had " + std::to_string( list.size() ) << " vobs";
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
        Engine::GraphicsEngine->DrawVertexBuffer( msh->MeshVertexBuffer, msh->Vertices.size() );
    } else {
        Engine::GraphicsEngine->DrawVertexBufferIndexed( msh->MeshVertexBuffer, msh->MeshIndexBuffer, msh->Indices.size() );
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

/** Called when a VOB got removed from the world */
void GothicAPI::OnRemovedVob( zCVob* vob, zCWorld* world ) {
    //LogInfo() << "Removing vob: " << vob;
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
    std::list<BaseVobInfo*>& list = VobsByVisual[vob->GetVisual()];
    for ( auto& vit = list.begin(); vit != list.end(); ++vit ) {
        if ( (*vit)->Vob == vob ) {
            list.erase( vit );
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

    // Tell all dynamic lights that we removed a vob they could have cached
    for ( auto& vlit : VobLightMap ) {
        if ( vi && vlit.second->LightShadowBuffers )
            vlit.second->LightShadowBuffers->OnVobRemovedFromWorld( vi );

        if ( svi && vlit.second->LightShadowBuffers )
            vlit.second->LightShadowBuffers->OnVobRemovedFromWorld( svi );
    }

    VobLightInfo* li = VobLightMap[static_cast<zCVobLight*>(vob)];

    // Erase it from the particle-effect list
    auto pit = std::find( ParticleEffectVobs.begin(), ParticleEffectVobs.end(), vob );
    if ( pit != ParticleEffectVobs.end() ) {
        DestroyParticleEffect( *pit );
        *pit = ParticleEffectVobs.back();
        ParticleEffectVobs.pop_back();
    }
    auto dit = std::find( DecalVobs.begin(), DecalVobs.end(), vob );
    if ( dit != DecalVobs.end() ) {
        *dit = DecalVobs.back();
        DecalVobs.pop_back();
    }

    // Erase it from the list of lights
    VobLightMap.erase( static_cast<zCVobLight*>(vob) );

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
                for ( auto bit = node->IndoorVobs.begin(); bit != node->IndoorVobs.end(); ++bit ) {
                    if ( (*bit) == vi ) {
                        (*bit) = node->IndoorVobs.back();
                        node->IndoorVobs.pop_back();
                        break;
                    }
                }

                for ( auto bit = node->Vobs.begin(); bit != node->Vobs.end(); ++bit ) {
                    if ( (*bit) == vi ) {
                        (*bit) = node->Vobs.back();
                        node->Vobs.pop_back();
                        break;
                    }
                }

                for ( auto bit = node->SmallVobs.begin(); bit != node->SmallVobs.end(); ++bit ) {
                    if ( (*bit) == vi ) {
                        (*bit) = node->SmallVobs.back();
                        node->SmallVobs.pop_back();
                        break;
                    }
                }
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
    for ( auto& vit = SkeletalMeshVobs.begin(); vit != SkeletalMeshVobs.end(); ++vit ) {
        if ( (*vit)->Vob == vob ) {
            //SkeletalVobInfo* vi = *vit;
            SkeletalMeshVobs.erase( vit );
            break;
        }
    }

    for ( auto& vit = AnimatedSkeletalVobs.begin(); vit != AnimatedSkeletalVobs.end(); ++vit ) {
        if ( (*vit)->Vob == vob ) {
            //SkeletalVobInfo* vi = *it;
            AnimatedSkeletalVobs.erase( vit );
            break;
        }
    }

    // Erase it from dynamically loaded vobs
    for ( auto& vit = DynamicallyAddedVobs.begin(); vit != DynamicallyAddedVobs.end(); ++vit ) {
        if ( (*vit)->Vob == vob ) {
            DynamicallyAddedVobs.erase( vit );
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
        OnRemovedVob( vob, vob->GetHomeWorld() );
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

    for ( unsigned int i = 0; i < extv.size(); i++ ) {
        std::string ext = extv[i];

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

                WorldConverter::Extract3DSMeshFromVisual2( pm, mi );
                StaticMeshVisuals[pm] = mi;
            }

            INT2 section = WorldConverter::GetSectionOfPos( vob->GetPositionWorld() );

            VobInfo* vi = new VobInfo;
            vi->Vob = vob;
            vi->VisualInfo = StaticMeshVisuals[pm];

            // Add to map
            VobsByVisual[vob->GetVisual()].push_back( vi );

            // Check for mainworld
            if ( world == oCGame::GetGame()->_zCSession_world ) {
                VobMap[vob] = vi;
                WorldSections[section.x][section.y].Vobs.push_back( vi );

                vi->VobSection = &WorldSections[section.x][section.y];

                // Create this constantbuffer only for non-inventory vobs because it would be recreated for each vob every frame
                Engine::GraphicsEngine->CreateConstantBuffer( &vi->VobConstantBuffer, nullptr, sizeof( VS_ExConstantBuffer_PerInstance ) );
                vi->UpdateVobConstantBuffer();

                if ( !BspLeafVobLists.empty() ) { // Check if this is the initial loading
                    // It's not, chose this as a dynamically added vob
                    DynamicallyAddedVobs.push_back( vi );
                }
            } else {
                // Must be inventory
                Inventory->OnAddVob( vi, world );
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

            std::string str = static_cast<zCModel*>(vob->GetVisual())->GetVisualName();
            if ( str.empty() ) { // Happens when the model has no skeletal-mesh
                zSTRING mds = static_cast<zCModel*>(vob->GetVisual())->GetModelName();
                str = mds.ToChar();
                mds.Delete();
            }

            // TODO: HAMMEL_BODY seems to have fucked up bones, but only this model! Replace with usual sheep before I fix this
            if ( str == "HAMMEL_BODY" ) {
                str = "SHEEP_BODY";
                if ( !SkeletalMeshVisuals[str] ) {
                    RegisteredVobs.erase( vob );
                    SkeletalMeshVisuals.erase( str );
                    return; // Just don't load it here!
                }
            }

            // Load the model or get it from cache if already done
            SkeletalMeshVisualInfo* mi = LoadzCModelData( static_cast<zCModel*>(vob->GetVisual()) );

            // Add vob to the skeletal list
            SkeletalVobInfo* vi = new SkeletalVobInfo;
            vi->Vob = vob;
            vi->VisualInfo = SkeletalMeshVisuals[str];

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
    std::string str = model->GetVisualName();
    if ( str.empty() ) { // Happens when the model has no skeletal-mesh
        zSTRING mds = model->GetModelName();
        str = mds.ToChar();
        mds.Delete();
    }

    SkeletalMeshVisualInfo* mi = SkeletalMeshVisuals[str];
    if ( !mi || mi->Meshes.empty() ) {
        // Load the new visual
        if ( !mi ) mi = new SkeletalMeshVisualInfo;

        WorldConverter::ExtractSkeletalMeshFromVob( model, mi );
        mi->Visual = model;

        SkeletalMeshVisuals[str] = mi;
    }
    return mi;
}

// TODO: REMOVE THIS!
#include "D3D11GraphicsEngine.h"

/** Called to update the compress backbuffer state */
void GothicAPI::UpdateCompressBackBuffer() {
    reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine)->OnResetBackBuffer();
}

/** Draws a skeletal mesh-vob */
void GothicAPI::DrawSkeletalMeshVob( SkeletalVobInfo* vi, float distance, bool updateState ) {
    // TODO: Put this into the renderer!!
    D3D11GraphicsEngine* g = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);

    zCModel* model = static_cast<zCModel*>(vi->Vob->GetVisual());
    SkeletalMeshVisualInfo* visual = static_cast<SkeletalMeshVisualInfo*>(vi->VisualInfo);

    if ( !model || !vi->VisualInfo )
        return; // Gothic fortunately sets this to 0 when it throws the model out of the cache

    model->SetIsVisible( true );

#ifndef BUILD_GOTHIC_1_08k // does not work in G1
    if ( !vi->Vob->GetShowVisual() )
        return;
#endif

    if ( model->GetDrawHandVisualsOnly() )
        return; // Not supported yet

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

    XMMATRIX world = vi->Vob->GetWorldMatrixXM() * scale;

    zCCamera::GetCamera()->SetTransformXM( zCCamera::TT_WORLD, world );

    XMMATRIX view = GetViewMatrixXM();
    SetWorldViewTransform( world, view );

    float fatness = model->GetModelFatness();

    // Get the bone transforms
    std::vector<XMFLOAT4X4> transforms;
    model->GetBoneTransforms( &transforms, vi->Vob );

    if ( updateState ) {
        // Update attachments
        model->UpdateAttachedVobs();
        model->UpdateMeshLibTexAniState();
    }

    if ( !static_cast<SkeletalMeshVisualInfo*>(vi->VisualInfo)->SkeletalMeshes.empty() ) {
        Engine::GraphicsEngine->DrawSkeletalMesh( vi, transforms, modelColor, fatness );
    } else {
        if ( model->GetMeshSoftSkinList()->NumInArray > 0 ) {
            // Just in case somehow we end up without skeletal meshes and they are available
            WorldConverter::ExtractSkeletalMeshFromVob( model, static_cast<SkeletalMeshVisualInfo*>(vi->VisualInfo) );
        }
    }

    if ( g->GetRenderingStage() == DES_SHADOWMAP_CUBE )
        g->SetActiveVertexShader( "VS_ExNodeCube" );
    else
        g->SetActiveVertexShader( "VS_ExMode" );

    // Set up instance info
    VS_ExConstantBuffer_PerInstanceNode instanceInfo;
    instanceInfo.Color = modelColor;

    // Init the constantbuffer if not already done
    if ( !vi->VobConstantBuffer )
        vi->UpdateVobConstantBuffer();

    g->SetupVS_ExMeshDrawCall();
    g->SetupVS_ExConstantBuffer();

    std::map<int, std::vector<MeshVisualInfo*>>& nodeAttachments = vi->NodeAttachments;
    for ( unsigned int i = 0; i < transforms.size(); i++ ) {
        // Check for new visual
        zCModel* mvis = static_cast<zCModel*>(vi->Vob->GetVisual());
        zCModelNodeInst* node = mvis->GetNodeList()->Array[i];

        if ( !node->NodeVisual )
            continue; // Happens when you pull your sword for example

        // Check if this is loaded
        if ( node->NodeVisual && nodeAttachments.find( i ) == nodeAttachments.end() ) {
            // It's not, extract it
            WorldConverter::ExtractNodeVisual( i, node, nodeAttachments );
        }

        // Check for changed visual
        if ( nodeAttachments[i].size() && node->NodeVisual != nodeAttachments[i][0]->Visual ) {
            // Check for deleted attachment
            if ( !node->NodeVisual ) {
                // Remove attachment
                delete nodeAttachments[i][0];
                nodeAttachments[i].clear();

                LogInfo() << "Removed attachment from model " << vi->VisualInfo->VisualName;

                continue; // Go to next attachment
            }
            // Load the new one
            WorldConverter::ExtractNodeVisual( i, node, nodeAttachments );
        }

        if ( nodeAttachments.find( i ) != nodeAttachments.end() ) {
            // Go through all attachments this node has
            for ( MeshVisualInfo* mvi : nodeAttachments[i] ) {
                XMMATRIX curTransform = XMLoadFloat4x4( &transforms[i] );
                SetWorldViewTransform( world * curTransform, view );

                if ( !mvi->Visual ) {
                    LogWarn() << "Attachment without visual on model: " << model->GetVisualName();
                    continue;
                }

                // Setup pixel shader here so that we get correct normals
                // Somehow BindShaderForTexture make normals to be inversed
                if ( g->GetRenderingStage() == DES_MAIN ) {
                    g->SetActivePixelShader( "PS_DiffuseAlphaTest" );
                    g->BindActivePixelShader();
                }

                // Update animated textures
                bool isMMS = std::string( mvi->Visual->GetFileExtension( 0 ) ) == ".MMS";
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

                auto& VShader = g->GetActiveVS();
                if ( distance < 1000 && isMMS ) {
                    zCMorphMesh* mm = reinterpret_cast<zCMorphMesh*>(mvi->Visual);
                    // Only draw this as a morphmesh when rendering the main scene or when rendering as ghost
                    if ( g->GetRenderingStage() == DES_MAIN || g->GetRenderingStage() == DES_GHOST ) {
                        // Update constantbuffer
                        instanceInfo.World = RendererState.TransformState.TransformWorld;
                        VShader->GetConstantBuffer()[1]->UpdateBuffer( &instanceInfo );
                        VShader->GetConstantBuffer()[1]->BindToVertexShader( 1 );

                        if ( updateState ) {
                            mm->AdvanceAnis();
                            mm->CalcVertexPositions();
                        }
                        DrawMorphMesh( mm, mvi->Meshes );
                        continue;
                    }
                }

                instanceInfo.World = RendererState.TransformState.TransformWorld;
                VShader->GetConstantBuffer()[1]->UpdateBuffer( &instanceInfo );
                VShader->GetConstantBuffer()[1]->BindToVertexShader( 1 );

                // Go through all materials registered here
                for ( auto const& itm : mvi->Meshes ) {
                    zCTexture* texture;
                    if ( itm.first && (texture = itm.first->GetAniTexture()) != nullptr ) { // TODO: Crash here!
                        if ( !g->BindTextureNRFX( texture, (g->GetRenderingStage() == DES_MAIN) ) )
                            continue;
                    }

                    // Go through all meshes using that material
                    for ( unsigned int m = 0; m < itm.second.size(); m++ ) {
                        DrawMeshInfo( itm.first, itm.second[m] );
                    }
                }
            }
        }
    }

    RendererState.RendererInfo.FrameDrawnVobs++;
}

void GothicAPI::DrawSkeletalGhosts() {
    D3D11GraphicsEngine* g = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    if ( !GhostSkeletalVobs.empty() ) {
        // Setup alpha blending
        RendererState.RasterizerState.SetDefault();
        RendererState.RasterizerState.SetDirty();
        RendererState.BlendState.SetAlphaBlending();
        RendererState.BlendState.SetDirty();
        RendererState.DepthState.SetDefault();
        RendererState.DepthState.SetDirty();
    }
    while ( !GhostSkeletalVobs.empty() ) {
        auto const& GhostInfo = GhostSkeletalVobs.front();

        // We need to do Z-prepass first
        g->UnbindActivePS();
        g->GetContext()->PSSetShader( nullptr, nullptr, 0 );
        DrawSkeletalMeshVob( GhostInfo.second, GhostInfo.first );
        RendererState.RendererInfo.FrameDrawnVobs--; // Don't calculate prepass as drawn vob

        // Now actually draw mesh using ghost pixel shader
        g->SetActivePixelShader( "PS_Ghost" );
        g->BindActivePixelShader();

        // Update ghost alpha information
        GhostAlphaConstantBuffer gacb;
        #ifdef BUILD_GOTHIC_2_6_fix
        gacb.GA_Alpha = *reinterpret_cast<float*>(0xAB26A4); // Cached GhostAlpha value
        #else
        gacb.GA_Alpha = 0.3f;
        #endif
        g->GetActivePS()->GetConstantBuffer()[0]->UpdateBuffer( &gacb );
        g->GetActivePS()->GetConstantBuffer()[0]->BindToPixelShader( 0 );
        DrawSkeletalMeshVob( GhostInfo.second, GhostInfo.first, false );

        std::pop_heap( GhostSkeletalVobs.begin(), GhostSkeletalVobs.end(), CompareGhostDistance );
        GhostSkeletalVobs.pop_back();
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

        D3D11GraphicsEngine* g = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);

        zCModel* model = static_cast<zCModel*>(vi->Vob->GetVisual());
        SkeletalMeshVisualInfo* visual = static_cast<SkeletalMeshVisualInfo*>(vi->VisualInfo);

        if ( model && vi->VisualInfo ) {
            XMMATRIX scale = XMMatrixScalingFromVector( model->GetModelScaleXM() );

            XMMATRIX world = vi->Vob->GetWorldMatrixXM() * scale;

            zCCamera::GetCamera()->SetTransformXM( zCCamera::TT_WORLD, world );

            XMMATRIX view = GetViewMatrixXM();
            SetWorldViewTransform( world, view );

            float fatness = model->GetModelFatness();

            // Get the bone transforms
            std::vector<XMFLOAT4X4> transforms;
            model->GetBoneTransforms( &transforms, vi->Vob );

            if ( !static_cast<SkeletalMeshVisualInfo*>(vi->VisualInfo)->SkeletalMeshes.empty() ) {
                g->DrawSkeletalVertexNormals( vi, transforms, 0xFFFFFF, fatness );
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
    // Update effects time
    fx->UpdateTime();

    // Maybe create more emitters?
    fx->CheckDependentEmitter();

    zTParticle* pfx = fx->GetFirstParticle();
    if ( pfx ) {
        // Get texture
        zCTexture* texture = nullptr;
        if ( zCParticleEmitter* emitter = fx->GetEmitter() ) {
            if ( emitter->GetVisShpType() == 5 && ParticleEffectProgMeshes.find(source) == ParticleEffectProgMeshes.end() ) {
                AddParticleEffect( source );
            }
            if ( (texture = emitter->GetVisTexture()) != nullptr ) {
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

        int i = 0;
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
            ii.scale = XMFLOAT2( p->Size.x, p->Size.y );
            ii.drawMode = 0;

            // Construct world matrix
            int alignment = fx->GetEmitter()->GetVisAlignment();
            if ( alignment == zPARTICLE_ALIGNMENT_XY ) {
                ii.drawMode = 2;
            } else if ( alignment == zPARTICLE_ALIGNMENT_VELOCITY || alignment == zPARTICLE_ALIGNMENT_VELOCITY_3D ) {
                ii.drawMode = 3;
            } // TODO: Y-Locked!

            if ( !fx->GetEmitter()->GetVisIsQuadPoly() ) {
                ii.scale.x *= 0.5f;
                ii.scale.y *= 0.5f;
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

            fx->UpdateParticle( p );

            i++;
        }
    }
    /*
        Liker@WoG:
11.12.2020 14:58	https://forum.worldofplayers.de/forum/threads/1546222-Yet-Another-D3D11-Renderer?p=26626374&viewfull=1#post26626374
11.12.2020 16:19	https://forum.worldofplayers.de/forum/threads/1546222-Yet-Another-D3D11-Renderer?p=26626530&viewfull=1#post26626530
14.12.2020 20:25	https://forum.worldofplayers.de/forum/threads/1546222-Yet-Another-D3D11-Renderer?p=26628056&viewfull=1#post26628056
    */

    // Create new particles?
    fx->CreateParticlesUpdateDependencies();

    if ( fx->GetVisualDied() ) {
        if ( zCVob* connectedVob = fx->GetConnectedVob() ) {
            // delete FX, it will be invalid after this call!
            connectedVob->GetHomeWorld()->RemoveVob( connectedVob );
        }
    } else {
        fx->GetStaticPFXList()->TouchPfx( fx );
    }
}

/** Debugging */
void GothicAPI::DrawTriangle( float3 pos = { 0.0f,0.0f,0.0f } ) {
    D3D11VertexBuffer* vxb;
    Engine::GraphicsEngine->CreateVertexBuffer( &vxb );
    vxb->Init( nullptr, 6 * sizeof( ExVertexStruct ), D3D11VertexBuffer::EBindFlags::B_VERTEXBUFFER, D3D11VertexBuffer::EUsageFlags::U_DYNAMIC, D3D11VertexBuffer::CA_WRITE );

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

    for ( int i = 0; i < 6; i++ ) {
        vx[i].Position.x += pos.x;
        vx[i].Position.y += pos.y;
        vx[i].Position.z += pos.z;
    }

    vxb->UpdateBuffer( vx );

    Engine::GraphicsEngine->DrawVertexBuffer( vxb, 6 );

    delete vxb;
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

    for ( auto it = VobMap.begin(); it != VobMap.end(); ++it ) {
        XMMATRIX world = XMMatrixTranspose( XMLoadFloat4x4( it->first->GetWorldMatrixPtr() ) );
        XMStoreFloat3( &min, XMVector3TransformCoord( XMLoadFloat3( &it->second->VisualInfo->BBox.Min ), world ) );
        XMStoreFloat3( &max, XMVector3TransformCoord( XMLoadFloat3( &it->second->VisualInfo->BBox.Max ), world ) );

        float t = 0;
        if ( Toolbox::IntersectBox( min, max, origin, dir, t ) ) {
            if ( t < closest ) {
                closest = t;
                hitBBs.push_back( it->second );
            }
        }
    }

    // Now trace the actual triangles to find the real hit

    closest = FLT_MAX;
    zCMaterial* closestMaterial = nullptr;
    VobInfo* closestVob = nullptr;
    XMFLOAT3 localOrigin;
    XMFLOAT3 localDir;

    for ( auto it = hitBBs.begin(); it != hitBBs.end(); ++it ) {
        XMMATRIX invWorld = XMMatrixInverse( nullptr, XMMatrixTranspose( XMLoadFloat4x4( (*it)->Vob->GetWorldMatrixPtr() ) ) );
        XMStoreFloat3( &localOrigin, XMVector3TransformCoord( XMLoadFloat3( &origin ), invWorld ) );
        XMStoreFloat3( &localDir, XMVector3TransformNormal( XMLoadFloat3( &dir ), invWorld ) );

        zCMaterial* hitMat = nullptr;
        float t = TraceVisualInfo( localOrigin, localDir, (*it)->VisualInfo, &hitMat );
        if ( t > 0.0f && t < closest ) {
            closest = t;
            closestVob = (*it);
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
            MeshInfo* mesh = it.second[m];

            for ( unsigned int i = 0; i < mesh->Indices.size(); i += 3 ) {
                if ( Toolbox::IntersectTri( *mesh->Vertices[mesh->Indices[i]].Position.toXMFLOAT3(),
                    *mesh->Vertices[mesh->Indices[i + 1]].Position.toXMFLOAT3(),
                    *mesh->Vertices[mesh->Indices[i + 2]].Position.toXMFLOAT3(),
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
        for ( std::map<MeshKey, WorldMeshInfo*>::iterator it = bit.first->WorldMeshes.begin(); it != bit.first->WorldMeshes.end(); ++it ) {
            float u, v, t;

            for ( unsigned int i = 0; i < it->second->Indices.size(); i += 3 ) {
                if ( Toolbox::IntersectTri( *it->second->Vertices[it->second->Indices[i]].Position.toXMFLOAT3(),
                    *it->second->Vertices[it->second->Indices[i + 1]].Position.toXMFLOAT3(),
                    *it->second->Vertices[it->second->Indices[i + 2]].Position.toXMFLOAT3(),
                    origin, dir, u, v, t ) ) {
                    if ( t > 0 && t < closest ) {
                        closest = t;

                        if ( hitTriangle ) {
                            hitTriangle[0] = *it->second->Vertices[it->second->Indices[i]].Position.toXMFLOAT3();
                            hitTriangle[1] = *it->second->Vertices[it->second->Indices[i + 1]].Position.toXMFLOAT3();
                            hitTriangle[2] = *it->second->Vertices[it->second->Indices[i + 2]].Position.toXMFLOAT3();
                        }

                        if ( hitMesh ) {
                            *hitMesh = it->second;
                        }

                        if ( hitMaterial ) {
                            *hitMaterial = it->first.Material;
                        }

                        if ( hitTextureName && it->first.Material && it->first.Material->GetTexture() )
                            *hitTextureName = it->first.Material->GetTexture()->GetNameWithoutExt();
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
void XM_CALLCONV GothicAPI::UnprojectXM( FXMVECTOR p, XMVECTOR& worldPos, XMVECTOR& worldDir ) {
    auto cam = zCCamera::GetCamera();
    XMMATRIX proj = XMMatrixTranspose( XMLoadFloat4x4( &cam->trafoProjection ) );
    XMMATRIX invView = XMMatrixTranspose( XMLoadFloat4x4( &cam->trafoViewInv ) );

    // Convert to screenspace
    auto res = Engine::GraphicsEngine->GetResolution();
    XMFLOAT2 fP; XMStoreFloat2( &fP, p );
    FXMVECTOR u = XMVectorSet(
        (((2 * fP.x) / res.x) - 1) / proj.r[0].m128_f32[0],
        -(((2 * fP.y) / res.y) - 1) / proj.r[1].m128_f32[1],
        1,
        0 );

    // Transform and output
    worldPos = XMVector3TransformCoord( u, invView );
    worldDir = XMVector3TransformCoord( XMVector3Normalize( u ), invView );
}

/** Unprojects the current cursor */
XMVECTOR GothicAPI::UnprojectCursorXM() {
    XMVECTOR mPos, mDir;
    POINT p = GetCursorPosition();

    Engine::GAPI->UnprojectXM( XMVectorSet( static_cast<float>(p.x), static_cast<float>(p.y), 0, 0 ), mPos, mDir );

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
FXMVECTOR GothicAPI::GetCameraPositionXM() {
    if ( !oCGame::GetGame()->_zCSession_camVob )
        return g_XMZero;

    if ( CameraReplacementPtr )
        return XMLoadFloat3( &CameraReplacementPtr->PositionReplacement );

    return oCGame::GetGame()->_zCSession_camVob->GetPositionWorldXM();
}


/** Returns the view matrix */
void GothicAPI::GetViewMatrix( XMFLOAT4X4* view ) {
    if ( CameraReplacementPtr ) {
        *view = CameraReplacementPtr->ViewReplacement;
        return;
    }

    *view = zCCamera::GetCamera()->GetTransformDX( zCCamera::ETransformType::TT_VIEW );
}

/** Returns the view matrix */
XMMATRIX GothicAPI::GetViewMatrixXM() {
    if ( CameraReplacementPtr ) {
        return XMLoadFloat4x4( &CameraReplacementPtr->ViewReplacement );
    }
    return XMLoadFloat4x4( &zCCamera::GetCamera()->GetTransformDX( zCCamera::ETransformType::TT_VIEW ) );
}

/** Returns the view matrix */
void GothicAPI::GetInverseViewMatrixXM( XMFLOAT4X4* invView ) {
    if ( CameraReplacementPtr ) {
        XMStoreFloat4x4( invView, XMMatrixInverse( nullptr, XMLoadFloat4x4( &CameraReplacementPtr->ViewReplacement ) ) );
        return;
    }

    *invView = zCCamera::GetCamera()->GetTransformDX( zCCamera::ETransformType::TT_VIEW_INV );
}

/** Returns the projection-matrix */
XMFLOAT4X4& GothicAPI::GetProjectionMatrix() {
    if ( CameraReplacementPtr ) {
        return CameraReplacementPtr->ProjectionReplacement;
    }

    // Reverse depth buffer
    float NearZ = RendererState.RendererSettings.SectionDrawRadius * WORLD_SECTION_SIZE;
    float FarZ = 1.0f;
    float zRange = FarZ / (FarZ - NearZ);
    RendererState.TransformState.TransformProj._33 = zRange;
    RendererState.TransformState.TransformProj._34 = -zRange * NearZ;
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

/** Returns the fog-color */
FXMVECTOR GothicAPI::GetFogColor() {
    zCSkyController_Outdoor* sc = oCGame::GetGame()->_zCSession_world->GetSkyControllerOutdoor();

    FXMVECTOR FogColorMod = XMLoadFloat3( RendererState.RendererSettings.FogColorMod.toXMFLOAT3() );

    // Only give the overridden color out if the flag is set
    if ( !sc || !sc->GetOverrideFlag() )

        return FogColorMod;

    XMVECTOR color = XMLoadFloat3( &sc->GetOverrideColor() );

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
    if ( !oCGame::GetGame() )
        return true;

    return oCGame::GetGame()->GetSingleStep();
}

/** Returns true if the game is overwriting the fog color with a fog-zone */
float GothicAPI::GetFogOverride() {
    zCSkyController_Outdoor* sc = oCGame::GetGame()->_zCSession_world->GetSkyControllerOutdoor();

    // Catch invalid controller
    if ( !sc )
        return 0.0f;
    float veclenght;
    XMStoreFloat( &veclenght, XMVector3Length( XMLoadFloat3( &sc->GetOverrideColor() ) ) );
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
        case VK_F11:
            if ( ( GetAsyncKeyState( VK_CONTROL ) & 0x8000 ) && !GMPModeActive ) {
                if ( reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine)->HasSettingsWindow() )
                    Engine::GraphicsEngine->OnUIEvent( BaseGraphicsEngine::EUIEvent::UI_OpenSettings );

                Engine::AntTweakBar->SetActive( !Engine::AntTweakBar->GetActive() );
                SetEnableGothicInput( !Engine::AntTweakBar->GetActive() );
            } else {
                if ( Engine::AntTweakBar->GetActive() ) {
                    Engine::AntTweakBar->SetActive( false );
                    SetEnableGothicInput( true );
                }
                Engine::GraphicsEngine->OnUIEvent( BaseGraphicsEngine::EUIEvent::UI_OpenSettings );
            }
            break;

        case VK_ESCAPE:
            if ( Engine::AntTweakBar->GetActive() ) {
                Engine::AntTweakBar->SetActive( false );
                SetEnableGothicInput( true );
            }
            if ( reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine)->HasSettingsWindow() )
                Engine::GraphicsEngine->OnUIEvent( BaseGraphicsEngine::EUIEvent::UI_OpenSettings );
            break;

        case VK_NUMPAD1:
            if ( !Engine::AntTweakBar->GetActive() && !GMPModeActive && Engine::GAPI->GetRendererState().RendererSettings.AllowNumpadKeys )
                SpawnVegetationBoxAt( GetCameraPosition() );
            break;

        case VK_NUMPAD2:
#ifdef PUBLIC_RELEASE
            if ( !Engine::AntTweakBar->GetActive() && !GMPModeActive && Engine::GAPI->GetRendererState().RendererSettings.AllowNumpadKeys )
#endif
                Ocean->AddWaterPatchAt( static_cast<unsigned int>(GetCameraPosition().x / OCEAN_PATCH_SIZE), static_cast<unsigned int>(GetCameraPosition().z / OCEAN_PATCH_SIZE) );
            break;

        case VK_NUMPAD3:
#ifdef PUBLIC_RELEASE
            if ( !Engine::AntTweakBar->GetActive() && !GMPModeActive && Engine::GAPI->GetRendererState().RendererSettings.AllowNumpadKeys )
#endif
            {
                for ( int x = -1; x <= 1; x++ ) {
                    for ( int y = -1; y <= 1; y++ ) {
                        Ocean->AddWaterPatchAt( static_cast<unsigned int>((GetCameraPosition().x / OCEAN_PATCH_SIZE) + x), static_cast<unsigned int>((GetCameraPosition().z / OCEAN_PATCH_SIZE) + y) );
                    }
                }
            }
            break;
        }
        break;

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
    Engine::AntTweakBar->OnWindowMessage( hWnd, msg, wParam, lParam );
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
    while ( base ) {
        if ( clipFlags > 0 ) {
            float yMaxWorld = Engine::GAPI->GetLoadedWorldInfo()->BspTree->GetRootNode()->BBox3D.Max.y;

            zTBBox3D nodeBox = base->BBox3D;
            float nodeYMax = std::min( yMaxWorld, Engine::GAPI->GetCameraPosition().y );
            nodeYMax = std::max( nodeYMax, base->BBox3D.Max.y );
            nodeBox.Max.y = nodeYMax;

            zTCam_ClipType nodeClip = zCCamera::GetCamera()->BBox3DInFrustum( nodeBox, clipFlags );

            if ( nodeClip == ZTCAM_CLIPTYPE_OUT )
                return; // Nothig to see here. Discard this node and the subtree
        }

        if ( base->IsLeaf() ) {
            // Check if this leaf is inside the frustum
            if ( clipFlags > 0 ) {
                if ( zCCamera::GetCamera()->BBox3DInFrustum( base->BBox3D, clipFlags ) == ZTCAM_CLIPTYPE_OUT )
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
            float plane_normal;
            XMStoreFloat( &plane_normal, XMVector3Dot( XMLoadFloat3( &node->Plane.Normal ), GetCameraPositionXM() ) );
            if ( plane_normal > node->Plane.Distance ) {
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

/** Draws the AABB for the BSP-Tree using the line renderer*/
void GothicAPI::DebugDrawBSPTree() {
    zCBspTree* tree = LoadedWorldInfo->BspTree;
    zCBspBase* root = tree->GetRootNode();

    // Recursively go through the tree and draw all nodes
    DebugDrawTreeNode( root, root->BBox3D );
}

/** Collects vobs using gothics BSP-Tree */
void GothicAPI::CollectVisibleVobs( std::vector<VobInfo*>& vobs, std::vector<VobLightInfo*>& lights, std::vector<SkeletalVobInfo*>& mobs ) {
    zCBspTree* tree = LoadedWorldInfo->BspTree;

    zCBspBase* rootBsp = tree->GetRootNode();
    BspInfo* root = &BspLeafVobLists[rootBsp];
    if ( zCCamera::GetCamera() ) {
        zCCamera::GetCamera()->Activate();
    }

    // Recursively go through the tree and draw all nodes
    CollectVisibleVobsHelper( root, root->OriginalNode->BBox3D, 63, vobs, lights, mobs );

    FXMVECTOR camPos = GetCameraPositionXM();
    const float vobIndoorDist = Engine::GAPI->GetRendererState().RendererSettings.IndoorVobDrawRadius;
    const float vobOutdoorDist = Engine::GAPI->GetRendererState().RendererSettings.OutdoorVobDrawRadius;
    const float vobOutdoorSmallDist = Engine::GAPI->GetRendererState().RendererSettings.OutdoorSmallVobDrawRadius;
    const float vobSmallSize = Engine::GAPI->GetRendererState().RendererSettings.SmallVobSize;

    std::list<VobInfo*> removeList; // TODO: This should not be needed!
    
    // Add visible dynamically added vobs
    if ( Engine::GAPI->GetRendererState().RendererSettings.DrawVOBs ) {
        float dist;
        for ( VobInfo* it : DynamicallyAddedVobs ) {
            // Get distance to this vob
            XMStoreFloat( &dist, XMVector3Length( camPos - it->Vob->GetPositionWorldXM() ) );
            // Draw, if in range
            if ( it->VisualInfo && ((dist < vobIndoorDist && it->IsIndoorVob) || (dist < vobOutdoorSmallDist && it->VisualInfo->MeshSize < vobSmallSize) || (dist < vobOutdoorDist)) ) {
#ifdef BUILD_GOTHIC_1_08k
                // TODO: This is sometimes nullptr, suggesting that the Vob is invalid. Why does this happen?
                if ( !it->VobConstantBuffer ) {
                    removeList.push_back( it );
                    continue;
                }
#endif
                if ( !it->Vob->GetShowVisual() ) {
                    continue;
                }

                VobInstanceInfo vii;
                vii.world = it->WorldMatrix;
                vii.color = it->GroundColor;

                reinterpret_cast<MeshVisualInfo*>(it->VisualInfo)->Instances.push_back( vii );

                vobs.push_back( it );
                it->VisibleInRenderPass = true;
            }
        }
    }

#ifdef BUILD_GOTHIC_1_08k
    // TODO: See above for info on this
    for ( VobInfo* vi : removeList ) {
        RegisteredVobs.insert( vi->Vob ); // This vob isn't in this set anymore, but still in DynamicallyAddedVobs??
        OnRemovedVob( vi->Vob, oCGame::GetGame()->_zCSession_world );
    }
#endif
}

/** Collects visible sections from the current camera perspective */
void GothicAPI::CollectVisibleSections( std::vector<WorldMeshSectionInfo*>& sections ) {
    const XMFLOAT3 camPos = Engine::GAPI->GetCameraPosition();
    const INT2 camSection = WorldConverter::GetSectionOfPos( camPos );

    if ( Engine::GAPI->GetRendererState().RendererSettings.DrawSectionIntersections ) {
        extern const float WORLD_SECTION_SIZE;
        const float sectionViewDist = Engine::GAPI->GetRendererState().RendererSettings.SectionDrawRadius * WORLD_SECTION_SIZE;
        for ( auto& itx : WorldSections ) {
            for ( auto& ity : itx.second ) {
                WorldMeshSectionInfo& section = ity.second;

                float dist = Toolbox::ComputePointAABBDistance( camPos, section.BoundingBox.Min, section.BoundingBox.Max );
                if ( dist < sectionViewDist ) {
                    int flags = 15; // Frustum check, no farplane
                    if ( zCCamera::GetCamera()->BBox3DInFrustum( section.BoundingBox, flags ) == ZTCAM_CLIPTYPE_OUT )
                        continue;

                    sections.push_back( &section );
                }
            }
        }
    } else {
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
                    int flags = 15; // Frustum check, no farplane
                    if ( zCCamera::GetCamera()->BBox3DInFrustum( section.BoundingBox, flags ) == ZTCAM_CLIPTYPE_OUT )
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
        for ( std::vector<VobInfo*>::iterator it = node->IndoorVobs.begin(); it != node->IndoorVobs.end(); ++it ) {
            if ( (*it) == vob ) {
                (*it) = node->IndoorVobs.back();
                node->IndoorVobs.pop_back();
                break;
            }
        }

        for ( std::vector<VobInfo*>::iterator it = node->SmallVobs.begin(); it != node->SmallVobs.end(); ++it ) {
            if ( (*it) == vob ) {
                (*it) = node->SmallVobs.back();
                node->SmallVobs.pop_back();
                break;
            }
        }

        for ( std::vector<VobInfo*>::iterator it = node->Vobs.begin(); it != node->Vobs.end(); ++it ) {
            if ( (*it) == vob ) {
                (*it) = node->Vobs.back();
                node->Vobs.pop_back();
                break;
            }
        }
    }
    vob->ParentBSPNodes.clear();

    // Add to dynamic vob list
    DynamicallyAddedVobs.push_back( vob );
}

std::vector<VobInfo*>::iterator GothicAPI::MoveVobFromBspToDynamic( VobInfo* vob, std::vector<VobInfo*>* source ) {
    std::vector<VobInfo*>::iterator itn = source->end();
    std::vector<VobInfo*>::iterator itc;

    // Remove from all nodes
    for ( size_t i = 0; i < vob->ParentBSPNodes.size(); i++ ) {
        BspInfo* node = vob->ParentBSPNodes[i];

        // Remove from possible lists
        for ( auto it = node->IndoorVobs.begin(); it != node->IndoorVobs.end(); ++it ) {
            if ( (*it) == vob ) {
                itc = node->IndoorVobs.erase( it );
                break;
            }
        }

        if ( &node->IndoorVobs == source )
            itn = itc;

        for ( auto it = node->SmallVobs.begin(); it != node->SmallVobs.end(); ++it ) {
            if ( (*it) == vob ) {
                itc = node->SmallVobs.erase( it );
                break;
            }
        }

        if ( &node->SmallVobs == source )
            itn = itc;

        for ( auto it = node->Vobs.begin(); it != node->Vobs.end(); ++it ) {
            if ( (*it) == vob ) {
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

static void CVVH_AddNotDrawnVobToList( std::vector<VobInfo*>& target, std::vector<VobInfo*>& source, float dist ) {
    std::vector<VobInfo*> remVobs;

    for ( auto const& it : source ) {
        if ( !it->VisibleInRenderPass ) {
            float vd;
            XMStoreFloat( &vd, XMVector3Length( Engine::GAPI->GetCameraPositionXM() - XMLoadFloat3( &it->LastRenderPosition ) ) );
            if ( vd < dist && it->Vob->GetShowVisual() ) {
                VobInstanceInfo vii;
                vii.world = it->WorldMatrix;
                vii.color = it->GroundColor;

                reinterpret_cast<MeshVisualInfo*>(it->VisualInfo)->Instances.push_back( vii );
                target.push_back( it );
                it->VisibleInRenderPass = true;
            }
        }
    }
}

static void CVVH_AddNotDrawnVobToList( std::vector<VobLightInfo*>& target, std::vector<VobLightInfo*>& source, float dist ) {
    float veclength;
    for ( auto const& it : source ) {
        if ( !it->VisibleInRenderPass ) {
            XMStoreFloat( &veclength, XMVector3Length( Engine::GAPI->GetCameraPositionXM() - it->Vob->GetPositionWorldXM() ) );
            if ( veclength - it->Vob->GetLightRange() < dist ) {
                target.push_back( it );
                it->VisibleInRenderPass = true;
            }
        }
    }
}

static void CVVH_AddNotDrawnVobToList( std::vector<SkeletalVobInfo*>& target, std::vector<SkeletalVobInfo*>& source, float dist ) {
    float vd;
    for ( auto const& it : source ) {
        if ( !it->VisibleInRenderPass ) {
            XMStoreFloat( &vd, XMVector3Length( Engine::GAPI->GetCameraPositionXM() - it->Vob->GetPositionWorldXM() ) );
            if ( vd < dist && it->Vob->GetShowVisual() ) {
                target.push_back( it );
                it->VisibleInRenderPass = true;
            }
        }
    }
}

/** Recursive helper function to draw collect the vobs */
void GothicAPI::CollectVisibleVobsHelper( BspInfo* base, zTBBox3D boxCell, int clipFlags, std::vector<VobInfo*>& vobs, std::vector<VobLightInfo*>& lights, std::vector<SkeletalVobInfo*>& mobs ) {
    const float vobIndoorDist = Engine::GAPI->GetRendererState().RendererSettings.IndoorVobDrawRadius;
    const float vobOutdoorDist = Engine::GAPI->GetRendererState().RendererSettings.OutdoorVobDrawRadius;
    const float vobOutdoorSmallDist = Engine::GAPI->GetRendererState().RendererSettings.OutdoorSmallVobDrawRadius;
    const float vobSmallSize = Engine::GAPI->GetRendererState().RendererSettings.SmallVobSize;
    const float visualFXDrawRadius = Engine::GAPI->GetRendererState().RendererSettings.VisualFXDrawRadius;
    const XMFLOAT3 camPos = Engine::GAPI->GetCameraPosition();

    while ( base->OriginalNode ) {
        // Check for occlusion-culling
        if ( Engine::GAPI->GetRendererState().RendererSettings.EnableOcclusionCulling && !base->OcclusionInfo.VisibleLastFrame ) {
            return;
        }

        if ( clipFlags > 0 ) {
            float yMaxWorld = Engine::GAPI->GetLoadedWorldInfo()->BspTree->GetRootNode()->BBox3D.Max.y;

            zTBBox3D nodeBox = base->OriginalNode->BBox3D;
            float nodeYMax = std::min( yMaxWorld, camPos.y );
            nodeYMax = std::max( nodeYMax, base->OriginalNode->BBox3D.Max.y );
            nodeBox.Max.y = nodeYMax;

            float dist = Toolbox::ComputePointAABBDistance( camPos, base->OriginalNode->BBox3D.Min, base->OriginalNode->BBox3D.Max );
            if ( dist < vobOutdoorDist ) {
                zTCam_ClipType nodeClip;
                if ( !Engine::GAPI->GetRendererState().RendererSettings.EnableOcclusionCulling ) {
                    nodeClip = zCCamera::GetCamera()->BBox3DInFrustum( nodeBox, clipFlags );
                } else {
                    nodeClip = static_cast<zTCam_ClipType>(base->OcclusionInfo.LastCameraClipType); // If we are using occlusion-clipping, this test has already been done
                }

                if ( nodeClip == ZTCAM_CLIPTYPE_OUT ) {
                    return; // Nothig to see here. Discard this node and the subtree}
                }
            } else {
                // Too far
                return;
            }
        }

        if ( base->OriginalNode->IsLeaf() ) {
            // Check if this leaf is inside the frustum
            bool insideFrustum = true;

            zCBspLeaf* leaf = static_cast<zCBspLeaf*>(base->OriginalNode);
            std::vector<VobInfo*>& listA = base->IndoorVobs;
            std::vector<VobInfo*>& listB = base->SmallVobs;
            std::vector<VobInfo*>& listC = base->Vobs;
            std::vector<SkeletalVobInfo*>& listD = base->Mobs;

            // Concat the lists
            const float dist = Toolbox::ComputePointAABBDistance( camPos, base->OriginalNode->BBox3D.Min, base->OriginalNode->BBox3D.Max );
            // float dist = XMVector3Length(XMLoadFloat3(&base->BBox3D.Min) - XMLoadFloat3(&camPos));

            if ( insideFrustum ) {
                if ( Engine::GAPI->GetRendererState().RendererSettings.DrawVOBs ) {
                    if ( dist < vobIndoorDist ) {
                        CVVH_AddNotDrawnVobToList( vobs, listA, vobIndoorDist );
                    }

                    if ( dist < vobOutdoorSmallDist ) {
                        CVVH_AddNotDrawnVobToList( vobs, listB, vobOutdoorSmallDist );
                    }
                }

                if ( dist < vobOutdoorDist ) {
                    if ( Engine::GAPI->GetRendererState().RendererSettings.DrawVOBs ) {
                        CVVH_AddNotDrawnVobToList( vobs, listC, vobOutdoorDist );
                    }
                }

                if ( Engine::GAPI->GetRendererState().RendererSettings.DrawMobs && dist < vobOutdoorSmallDist ) {
                    CVVH_AddNotDrawnVobToList( mobs, listD, vobOutdoorDist );
                }

                if ( RendererState.RendererSettings.EnableDynamicLighting && dist < visualFXDrawRadius ) {
                    // Add dynamic lights
                    float minDynamicUpdateLightRange = Engine::GAPI->GetRendererState().RendererSettings.MinLightShadowUpdateRange;
                    XMVECTOR playerPosition = Engine::GAPI->GetPlayerVob() != nullptr ? Engine::GAPI->GetPlayerVob()->GetPositionWorldXM() : XMVectorSet( FLT_MAX, FLT_MAX, FLT_MAX, 0 );
                    FXMVECTOR cameraPosition = Engine::GAPI->GetCameraPositionXM();

                    // Take cameraposition if we are freelooking
                    if ( zCCamera::IsFreeLookActive() ) {
                        playerPosition = cameraPosition;
                    }

                    for ( int i = 0; i < leaf->LightVobList.NumInArray; i++ ) {
                        zCVobLight* vob = leaf->LightVobList.Array[i];

                        float lightCameraDist;
                        XMStoreFloat( &lightCameraDist, XMVector3Length( cameraPosition - vob->GetPositionWorldXM() ) );
                        if ( lightCameraDist + vob->GetLightRange() < visualFXDrawRadius ) {
                            // Check if we already have this light
                            auto vit = VobLightMap.find( vob );
                            if ( vit == VobLightMap.end() ) {
                                bool PFXVobLight = false;
                                if ( zCVob* parent = vob->GetVobParent() ) {
                                    if ( parent->As<oCVisualFX>() ) {
                                        PFXVobLight = true;
                                    }
                                }

                                // Add if not. This light must have been added during gameplay
                                VobLightInfo* vi = new VobLightInfo;
                                vi->Vob = vob;
                                vi->IsPFXVobLight = PFXVobLight;
                                vi->UpdateShadows = !PFXVobLight;
                                vit = VobLightMap.emplace( vob, vi ).first;

                                // Create shadow-buffers for these lights since it was dynamically added to the world
                                if ( !vi->IsPFXVobLight && RendererState.RendererSettings.EnablePointlightShadows >= GothicRendererSettings::PLS_STATIC_ONLY )
                                    Engine::GraphicsEngine->CreateShadowedPointLight( &vi->LightShadowBuffers, vi, true ); // Also flag as dynamic
                            }

                            VobLightInfo* vi = vit->second;
                            if ( !vi->VisibleInRenderPass && vob->IsEnabled() /*&& vob->GetShowVisual()*/ ) {
                                vi->VisibleInRenderPass = true;

                                // Update the lights shadows if: Light is dynamic or full shadow-updates are set
                                if ( !vi->IsPFXVobLight ) {
                                    if ( RendererState.RendererSettings.EnablePointlightShadows >= GothicRendererSettings::PLS_FULL
                                        || (RendererState.RendererSettings.EnablePointlightShadows >= GothicRendererSettings::PLS_UPDATE_DYNAMIC && !vob->IsStatic()) ) {
                                        // Now check for distances, etc
                                        float lightPlayerDist;
                                        XMStoreFloat( &lightPlayerDist, XMVector3Length( playerPosition - vob->GetPositionWorldXM() ) );
                                        if ( vob->GetLightRange() > minDynamicUpdateLightRange && lightPlayerDist < vob->GetLightRange() * 1.5f )
                                            vi->UpdateShadows = true;
                                    }
                                }

                                // Render it
                                lights.push_back( vi );
                            }
                        }
                    }
                }
            }
            return;
        } else {
            zCBspNode* node = static_cast<zCBspNode*>(base->OriginalNode);

            int	planeAxis = node->PlaneSignbits;

            boxCell.Min.y = node->BBox3D.Min.y;
            boxCell.Max.y = node->BBox3D.Min.y;

            zTBBox3D tmpbox = boxCell;
            float plane_normal;
            XMStoreFloat( &plane_normal, XMVector3Dot( XMLoadFloat3( &node->Plane.Normal ), GetCameraPositionXM() ) );
            if ( plane_normal > node->Plane.Distance ) {
                if ( node->Front ) {
                    reinterpret_cast<float*>(&tmpbox.Min)[planeAxis] = node->Plane.Distance;
                    CollectVisibleVobsHelper( base->Front, tmpbox, clipFlags, vobs, lights, mobs );
                }

                reinterpret_cast<float*>(&boxCell.Max)[planeAxis] = node->Plane.Distance;
                base = base->Back;
            } else {
                if ( node->Back ) {
                    reinterpret_cast<float*>(&tmpbox.Max)[planeAxis] = node->Plane.Distance;
                    CollectVisibleVobsHelper( base->Back, tmpbox, clipFlags, vobs, lights, mobs );
                }

                reinterpret_cast<float*>(&boxCell.Min)[planeAxis] = node->Plane.Distance;
                base = base->Front;
            }
        }
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

                    // Treat indoor vobs as indoor vobs only in outdoor locations
                    if ( outdoorLocation && vob->IsIndoorVob() ) {
                        // Only add once
                        if ( std::find( bvi.IndoorVobs.begin(), bvi.IndoorVobs.end(), v ) == bvi.IndoorVobs.end() ) {
                            v->ParentBSPNodes.push_back( &bvi );
                            bvi.IndoorVobs.push_back( v );
                            v->IsIndoorVob = true;
                        }
                    } else if ( v->VisualInfo->MeshSize < vobSmallSize ) {
                        // Only add once
                        if ( std::find( bvi.SmallVobs.begin(), bvi.SmallVobs.end(), v ) == bvi.SmallVobs.end() ) {
                            v->ParentBSPNodes.push_back( &bvi );
                            bvi.SmallVobs.push_back( v );
                        }
                    } else {
                        // Only add once
                        if ( std::find( bvi.Vobs.begin(), bvi.Vobs.end(), v ) == bvi.Vobs.end() ) {
                            v->ParentBSPNodes.push_back( &bvi );
                            bvi.Vobs.push_back( v );
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
                    if ( std::find( bvi.Mobs.begin(), bvi.Mobs.end(), v ) == bvi.Mobs.end() ) {
                        v->ParentBSPNodes.push_back( &bvi );
                        bvi.Mobs.push_back( v );
                    }
                }
            }
        }

        for ( int i = 0; i < leaf->LightVobList.NumInArray; i++ ) {
            zCVobLight* vob = leaf->LightVobList.Array[i];

            // Add the light to the map if not already done
            auto vit = VobLightMap.find( vob );
            if ( vit == VobLightMap.end() ) {
                VobLightInfo* vi = new VobLightInfo;
                vi->Vob = vob;
                VobLightMap[vob] = vi;

                float minDynamicUpdateLightRange = Engine::GAPI->GetRendererState().RendererSettings.MinLightShadowUpdateRange;
                if ( RendererState.RendererSettings.EnablePointlightShadows >= GothicRendererSettings::PLS_STATIC_ONLY
                    && vi->Vob->GetLightRange() > minDynamicUpdateLightRange ) {
                    // Create shadowcubemap, if wanted
                    Engine::GraphicsEngine->CreateShadowedPointLight( &vi->LightShadowBuffers, vi );
                }

                if ( vob->IsIndoorVob() ) {
                    vi->IsIndoorVob = true;
                }
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

/** Builds our BspTreeVobMap */
void GothicAPI::BuildBspVobMapCache() {
    BuildBspVobMapCacheHelper( LoadedWorldInfo->BspTree->GetRootNode() );
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
    zCCamera::GetCamera()->SetFarPlane( value );
    zCCamera::GetCamera()->Activate();
}

float GothicAPI::GetFarPlane() {
    return zCCamera::GetCamera()->GetFarPlane();
}

/** Sets/Gets the far-plane */
void GothicAPI::SetNearPlane( float value ) {
    LogWarn() << "SetNearPlane not implemented yet!";
}

float GothicAPI::GetNearPlane() {
    return zCCamera::GetCamera()->GetNearPlane();
}

/** Get material by texture name */
zCMaterial* GothicAPI::GetMaterialByTextureName( const std::string& name ) {
    for ( auto const& it : LoadedMaterials ) {
        if ( it->GetTexture() ) {
            std::string tn = it->GetTexture()->GetNameWithoutExt();
            if ( _strnicmp( name.c_str(), tn.c_str(), 255 ) == 0 )
                return it;
        }
    }

    return nullptr;
}

void GothicAPI::GetMaterialListByTextureName( const std::string& name, std::list<zCMaterial*>& list ) {
    for ( auto const& it : LoadedMaterials ) {
        if ( it->GetTexture() ) {
            std::string tn = it->GetTexture()->GetNameWithoutExt();
            if ( _strnicmp( name.c_str(), tn.c_str(), 255 ) == 0 )
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
    HookedFunctions::OriginalFunctions.original_zCWorldRender( oCGame::GetGame()->_zCSession_world, *zCCamera::GetCamera() );
}

/** Reset's the material info that were previously gathered */
void GothicAPI::ResetMaterialInfo() {
    MaterialInfos.clear();
}

/** Returns the material info associated with the given material */
MaterialInfo* GothicAPI::GetMaterialInfoFrom( zCTexture* tex ) {
    auto it = MaterialInfos.find( tex );
    if ( it == MaterialInfos.end() && tex ) {
        // Make a new one and try to load it
        MaterialInfos[tex].LoadFromFile( tex->GetNameWithoutExt() );
    }

    return &MaterialInfos[tex];
}

/** Adds a surface */
void GothicAPI::AddSurface( const std::string& name, MyDirectDrawSurface7* surface ) {
    SurfacesByName[name] = surface;
}

/** Gets a surface by texturename */
MyDirectDrawSurface7* GothicAPI::GetSurface( const std::string& name ) {
    return SurfacesByName[name];
}

/** Removes a surface */
void GothicAPI::RemoveSurface( MyDirectDrawSurface7* surface ) {
    SurfacesByName.erase( surface->GetTextureName() );
}

/** Returns the loaded skeletal mesh vobs */
std::list<SkeletalVobInfo*>& GothicAPI::GetSkeletalMeshVobs() {
    return SkeletalMeshVobs;
}

/** Returns the loaded skeletal mesh vobs */
std::list<SkeletalVobInfo*>& GothicAPI::GetAnimatedSkeletalMeshVobs() {
    return AnimatedSkeletalVobs;
}

/** Returns a texture from the given surface */
zCTexture* GothicAPI::GetTextureBySurface( MyDirectDrawSurface7* surface ) {
    for ( auto const& it : LoadedMaterials ) {
        auto const texture = it->GetTexture();
        if ( texture && texture->GetSurface() == surface )
            return texture;
    }

    return nullptr;
}

/** Resets all vob-stats drawn this frame */
void GothicAPI::ResetVobFrameStats( std::list<VobInfo*>& vobs ) {
    for ( auto&& it : vobs ) {
        it->VisibleInRenderPass = false;
    }
}

/** Sets the currently bound texture */
void GothicAPI::SetBoundTexture( int idx, zCTexture* tex ) {
    BoundTextures[idx] = tex;
}

zCTexture* GothicAPI::GetBoundTexture( int idx ) {
    return BoundTextures[idx];
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

    std::string zen = zenFolder + LoadedWorldInfo->WorldName;

    LogInfo() << "Loading custom ZEN-Resources from: " << zen;

    // Suppressed Textures
    LoadSuppressedTextures( zen + ".spt" );

    // Load vegetation
    LoadVegetation( zen + ".veg" );

#if ENABLE_TESSELATION > 0
    // Load world mesh information
    LoadSectionInfos();
#endif
}

/** Saves resources created for this .ZEN */
void GothicAPI::SaveCustomZENResources() {
    auto gameName = GetGameName();
    std::string zenFolder;
    if ( gameName == "Original" ) {
        zenFolder = "system\\GD3D11\\ZENResources\\";
    } else {
        zenFolder = "system\\GD3D11\\ZENResources\\" + gameName + "\\";
    }

    bool mkDirErr = false;
    if ( !Toolbox::FolderExists( zenFolder ) ) {
        mkDirErr = !Toolbox::CreateDirectoryRecursive( zenFolder );
    }

    if ( mkDirErr ) {
        LogError() << "Could not save custom ZEN-Resources. Could not create directory: " << zenFolder;
        return;
    }

    std::string zen = zenFolder + LoadedWorldInfo->WorldName;

    LogInfo() << "Saving custom ZEN-Resources to: " << zen;

    // Suppressed Textures
    SaveSuppressedTextures( zen + ".spt" );

    // Save vegetation
    SaveVegetation( zen + ".veg" );

#if ENABLE_TESSELATION > 0
    // Save world mesh information
    SaveSectionInfos();
#endif
}

/** Applys the suppressed textures */
void GothicAPI::ApplySuppressedSectionTextures() {
    for ( auto const& it : SuppressedTexturesBySection ) {
        WorldMeshSectionInfo* section = it.first;

        // Look into each mesh of this section and find the texture
        for ( std::map<MeshKey, WorldMeshInfo*>::iterator mit = section->WorldMeshes.begin(); mit != section->WorldMeshes.end(); mit++ ) {
            for ( unsigned int i = 0; i < it.second.size(); i++ ) {
                // Is this the texture we are looking for?
                if ( (*mit).first.Material && (*mit).first.Material->GetTexture() && (*mit).first.Material->GetTexture()->GetNameWithoutExt() == it.second[i] ) {
                    // Yes, move it to the suppressed map
                    section->SuppressedMeshes[(*mit).first] = (*mit).second;
                    section->WorldMeshes.erase( mit );
                    break;
                }
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
    FILE* f = fopen( file.c_str(), "rb" );

    LogInfo() << "Loading vegetation";

    // Reset first
    ResetVegetation();

    if ( !f )
        return XR_FAILED;

    int version;
    fread( &version, sizeof( version ), 1, f );

    size_t num = VegetationBoxes.size();
    fread( &num, sizeof( num ), 1, f );

    for ( size_t i = 0; i < num; i++ ) {
        GVegetationBox* b = new GVegetationBox;
        b->LoadFromFILE( f, version );

        AddVegetationBox( b );
    }

    fclose( f );

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

    WritePrivateProfileStringA( "General", "ChangeToMode", std::to_string( s.ChangeWindowPreset ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "General", "AtmosphericScattering", std::to_string( s.AtmosphericScattering ? TRUE : FALSE ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "General", "EnableFog", std::to_string( s.DrawFog ? TRUE : FALSE ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "General", "EnableHDR", std::to_string( s.EnableHDR ? TRUE : FALSE ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "General", "HDRToneMap", std::to_string( s.HDRToneMap ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "General", "EnableDebugLog", std::to_string( s.EnableDebugLog ? TRUE : FALSE ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "General", "EnableAutoupdates", std::to_string( s.EnableAutoupdates ? TRUE : FALSE ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "General", "EnableGodRays", std::to_string( s.EnableGodRays ? TRUE : FALSE ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "General", "AllowNormalmaps", std::to_string( s.AllowNormalmaps ? TRUE : FALSE ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "General", "AllowNumpadKeys", std::to_string( s.AllowNumpadKeys ? TRUE : FALSE ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "General", "EnableInactiveFpsLock", std::to_string( s.EnableInactiveFpsLock ? TRUE : FALSE ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "General", "MultiThreadResourceManager", std::to_string( s.MTResoureceManager ? TRUE : FALSE ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "General", "CompressBackBuffer", std::to_string( s.CompressBackBuffer ? TRUE : FALSE ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "General", "AnimateStaticVobs", std::to_string( s.AnimateStaticVobs ? TRUE : FALSE ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "General", "DrawWorldSectionIntersections", std::to_string( s.DrawSectionIntersections ? TRUE : FALSE ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "General", "SunLightStrength", std::to_string( s.SunLightStrength ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "General", "DrawG1ForestPortals", std::to_string( s.DrawG1ForestPortals ? TRUE : FALSE ).c_str(), ini.c_str() );

    /*
    * Draw-distance is saved on a per World basis using SaveRendererWorldSettings
    */

    WritePrivateProfileStringA( "General", "EnableOcclusionCulling", std::to_string( s.EnableOcclusionCulling ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "General", "FpsLimit", std::to_string( s.FpsLimit ).c_str(), ini.c_str() );
    
    auto res = Engine::GraphicsEngine->GetResolution();
    WritePrivateProfileStringA( "Display", "TextureQuality", std::to_string( s.textureMaxSize ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "Display", "Width", std::to_string( res.x ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "Display", "Height", std::to_string( res.y ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "Display", "VSync", std::to_string( s.EnableVSync ? TRUE : FALSE ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "Display", "ForceFOV", std::to_string( s.ForceFOV ? TRUE : FALSE ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "Display", "FOVHoriz", std::to_string( static_cast<int>(s.FOVHoriz) ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "Display", "FOVVert", std::to_string( static_cast<int>(s.FOVVert) ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "Display", "Gamma", std::to_string( s.GammaValue ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "Display", "Brightness", std::to_string( s.BrightnessValue ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "Display", "DisplayFlip", std::to_string( s.DisplayFlip ? TRUE : FALSE ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "Display", "LowLatency", std::to_string( s.LowLatency ? TRUE : FALSE ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "Display", "HDR_Monitor", std::to_string( s.HDR_Monitor ? TRUE : FALSE ).c_str(), ini.c_str() );

    WritePrivateProfileStringA( "Display", "StretchWindow", std::to_string( s.StretchWindow ? TRUE : FALSE ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "Display", "UIScale", std::to_string( s.GothicUIScale ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "Display", "Rain", std::to_string( s.EnableRain ? TRUE : FALSE ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "Display", "RainEffects", std::to_string( s.EnableRainEffects ? TRUE : FALSE ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "Display", "LimitLightIntesity", std::to_string( s.LimitLightIntesity ? TRUE : FALSE ).c_str(), ini.c_str() );

    WritePrivateProfileStringA( "Shadows", "EnableShadows", std::to_string( s.EnableShadows ? TRUE : FALSE ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "Shadows", "EnableSoftShadows", std::to_string( s.EnableSoftShadows ? TRUE : FALSE ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "Shadows", "ShadowMapSize", std::to_string( s.ShadowMapSize ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "Shadows", "WorldShadowRangeScale", std::to_string( s.WorldShadowRangeScale ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "Shadows", "PointlightShadows", std::to_string( s.EnablePointlightShadows ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "Shadows", "EnableDynamicLighting", std::to_string( s.EnableDynamicLighting ? TRUE : FALSE ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "Shadows", "SmoothCameraUpdate", std::to_string( s.SmoothShadowCameraUpdate ? TRUE : FALSE ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "Shadows", "ShadowStrength", std::to_string( s.ShadowStrength ).c_str(), ini.c_str() );

    WritePrivateProfileStringA( "SMAA", "Enabled", std::to_string( s.EnableSMAA ? TRUE : FALSE ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "SMAA", "SharpenFactor", std::to_string( s.SharpenFactor ).c_str(), ini.c_str() );

    WritePrivateProfileStringA( "HBAO", "Enabled", std::to_string( s.HbaoSettings.Enabled ? TRUE : FALSE ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "HBAO", "Bias", std::to_string( s.HbaoSettings.Bias ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "HBAO", "Radius", std::to_string( s.HbaoSettings.Radius ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "HBAO", "PowerExponent", std::to_string( s.HbaoSettings.PowerExponent ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "HBAO", "BlurSharpness", std::to_string( s.HbaoSettings.BlurSharpness ).c_str(), ini.c_str() );
    //WritePrivateProfileStringA( "HBAO", "EnableDualLayerAO", std::to_string( s.HbaoSettings.EnableDualLayerAO ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "HBAO", "EnableBlur", std::to_string( s.HbaoSettings.EnableBlur ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "HBAO", "SsaoBlurRadius", std::to_string( s.HbaoSettings.SsaoBlurRadius ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "HBAO", "SsaoStepCount", std::to_string( s.HbaoSettings.SsaoStepCount ).c_str(), ini.c_str() );

#if ENABLE_TESSELATION > 0
    WritePrivateProfileStringA( "Tesselation", "EnableTesselation", std::to_string( s.EnableTesselation ? TRUE : FALSE ).c_str(), ini.c_str() );
    WritePrivateProfileStringA( "Tesselation", "AllowWorldMeshTesselation", std::to_string( s.AllowWorldMeshTesselation ? TRUE : FALSE ).c_str(), ini.c_str() );
#endif

    WritePrivateProfileStringA( "FontRendering", "Enable", std::to_string( s.EnableCustomFontRendering ? TRUE : FALSE ).c_str(), ini.c_str() );

    return XR_SUCCESS;
}

/** Loads the users settings from the menu */
XRESULT GothicAPI::LoadMenuSettings( const std::string& file ) {
    TCHAR NPath[MAX_PATH];
    // Returns Gothic directory.
    int len = GetCurrentDirectory( MAX_PATH, NPath );
    // Get path to Gothic.Ini
    auto ini = std::string( NPath, len ).append( "\\" + file );


    if ( !Toolbox::FileExists( ini ) ) {
        LogWarn() << "Settings file not found: " << ini;
        return XR_FAILED;
    }
    LogInfo() << "Loading menu settings from " << ini;


    GothicRendererSettings defaultRendererSettings;
    defaultRendererSettings.SetDefault();

    GothicRendererSettings& s = RendererState.RendererSettings;

    s.ChangeWindowPreset = GetPrivateProfileIntA( "General", "ChangeToMode", 0, ini.c_str() );
    s.DrawFog = GetPrivateProfileBoolA( "General", "EnableFog", true, ini );
    s.AtmosphericScattering = GetPrivateProfileBoolA( "General", "AtmosphericScattering", true, ini );
    s.EnableHDR = GetPrivateProfileBoolA( "General", "EnableHDR", false, ini );
    s.HDRToneMap = GothicRendererSettings::E_HDRToneMap( GetPrivateProfileIntA( "General", "HDRToneMap", 4, ini.c_str() ) );
    s.EnableDebugLog = GetPrivateProfileBoolA( "General", "EnableDebugLog", defaultRendererSettings.EnableDebugLog, ini );
    s.EnableAutoupdates = GetPrivateProfileBoolA( "General", "EnableAutoupdates", defaultRendererSettings.EnableAutoupdates, ini );
    s.EnableGodRays = GetPrivateProfileBoolA( "General", "EnableGodRays", defaultRendererSettings.EnableGodRays, ini );
    s.AllowNormalmaps = GetPrivateProfileBoolA( "General", "AllowNormalmaps", defaultRendererSettings.AllowNormalmaps, ini );
    s.AllowNumpadKeys = GetPrivateProfileBoolA( "General", "AllowNumpadKeys", defaultRendererSettings.AllowNumpadKeys, ini );
    s.EnableInactiveFpsLock = GetPrivateProfileBoolA( "General", "EnableInactiveFpsLock", defaultRendererSettings.EnableInactiveFpsLock, ini );
    s.MTResoureceManager = GetPrivateProfileBoolA( "General", "MultiThreadResourceManager", defaultRendererSettings.MTResoureceManager, ini );
    s.CompressBackBuffer = GetPrivateProfileBoolA( "General", "CompressBackBuffer", defaultRendererSettings.CompressBackBuffer, ini );
    s.AnimateStaticVobs = GetPrivateProfileBoolA( "General", "AnimateStaticVobs", defaultRendererSettings.AnimateStaticVobs, ini );
    s.DrawSectionIntersections = GetPrivateProfileBoolA( "General", "DrawWorldSectionIntersections", defaultRendererSettings.DrawSectionIntersections, ini );
    s.SunLightStrength = GetPrivateProfileFloatA( "General", "SunLightStrength", defaultRendererSettings.SunLightStrength, ini );
    s.DrawG1ForestPortals = GetPrivateProfileBoolA( "General", "DrawG1ForestPortals", defaultRendererSettings.DrawG1ForestPortals, ini );

    /*
    * Draw-distance is Loaded on a per World basis using LoadRendererWorldSettings
    */

    s.EnableOcclusionCulling = GetPrivateProfileBoolA( "General", "EnableOcclusionCulling", defaultRendererSettings.EnableOcclusionCulling, ini );
    s.FpsLimit = GetPrivateProfileIntA( "General", "FpsLimit", 0, ini.c_str() );

    // override INI settings with GMP minimum values.
    if ( GMPModeActive ) {
        s.OutdoorVobDrawRadius = std::max( 20000.f, s.OutdoorVobDrawRadius );
        s.OutdoorSmallVobDrawRadius = std::max( 20000.f, s.OutdoorSmallVobDrawRadius );
        s.SectionDrawRadius = std::max( 3, s.SectionDrawRadius );
        s.EnableHDR = false;
    }

    static XMFLOAT3 defaultLightDirection = XMFLOAT3( 1, 1, 1 );

    s.EnableShadows = GetPrivateProfileBoolA( "Shadows", "EnableShadows", defaultRendererSettings.EnableShadows, ini );
    s.EnableSoftShadows = GetPrivateProfileBoolA( "Shadows", "EnableSoftShadows", defaultRendererSettings.EnableSoftShadows, ini );
    s.ShadowMapSize = GetPrivateProfileIntA( "Shadows", "ShadowMapSize", defaultRendererSettings.ShadowMapSize, ini.c_str() );
    s.EnablePointlightShadows = GothicRendererSettings::EPointLightShadowMode( GetPrivateProfileIntA( "Shadows", "PointlightShadows", GothicRendererSettings::EPointLightShadowMode::PLS_STATIC_ONLY, ini.c_str() ) );
    s.WorldShadowRangeScale = GetPrivateProfileFloatA( "Shadows", "WorldShadowRangeScale", 1.0f, ini );
    s.EnableDynamicLighting = GetPrivateProfileBoolA( "Shadows", "EnableDynamicLighting", defaultRendererSettings.EnableDynamicLighting, ini );
    s.SmoothShadowCameraUpdate = GetPrivateProfileBoolA( "Shadows", "SmoothCameraUpdate", defaultRendererSettings.SmoothShadowCameraUpdate, ini );
    s.ShadowStrength = GetPrivateProfileFloatA( "Shadows", "ShadowStrength", defaultRendererSettings.ShadowStrength, ini );

    INT2 res = {};
    RECT desktopRect;
    GetClientRect( GetDesktopWindow(), &desktopRect );
    s.textureMaxSize = std::max<int>( 32, GetPrivateProfileIntA( "Display", "TextureQuality", 16384, ini.c_str() ) );
    res.x = GetPrivateProfileIntA( "Display", "Width", desktopRect.right, ini.c_str() );
    res.y = GetPrivateProfileIntA( "Display", "Height", desktopRect.bottom, ini.c_str() );
    s.EnableVSync = GetPrivateProfileBoolA( "Display", "VSync", false, ini );
    s.ForceFOV = GetPrivateProfileBoolA( "Display", "ForceFOV", defaultRendererSettings.ForceFOV, ini );
    s.FOVHoriz = GetPrivateProfileIntA( "Display", "FOVHoriz", 90, ini.c_str() );
    s.FOVVert = GetPrivateProfileIntA( "Display", "FOVVert", 90, ini.c_str() );
    s.GammaValue = GetPrivateProfileFloatA( "Display", "Gamma", 1.0f, ini );
    s.BrightnessValue = GetPrivateProfileFloatA( "Display", "Brightness", 1.0f, ini );
    s.DisplayFlip = GetPrivateProfileBoolA( "Display", "DisplayFlip", false, ini );
    s.LowLatency = GetPrivateProfileBoolA( "Display", "LowLatency", false, ini );
    s.HDR_Monitor = GetPrivateProfileBoolA( "Display", "HDR_Monitor", false, ini );
    s.StretchWindow = GetPrivateProfileBoolA( "Display", "StretchWindow", false, ini );
    s.GothicUIScale = GetPrivateProfileFloatA( "Display", "UIScale", 1.0f, ini );
    s.EnableRain = GetPrivateProfileBoolA( "Display", "Rain", true, ini );
    s.EnableRainEffects = GetPrivateProfileBoolA( "Display", "RainEffects", true, ini );
    s.LimitLightIntesity = GetPrivateProfileBoolA( "Display", "LimitLightIntesity", false, ini );

    s.EnableSMAA = GetPrivateProfileBoolA( "SMAA", "Enabled", false, ini );
    s.SharpenFactor = GetPrivateProfileFloatA( "SMAA", "SharpenFactor", 0.30f, ini );

#if ENABLE_TESSELATION > 0
    s.AllowWorldMeshTesselation = GetPrivateProfileBoolA( "Tesselation", "AllowWorldMeshTesselation", false, ini );
    s.EnableTesselation = GetPrivateProfileBoolA( "Tesselation", "EnableTesselation", false, ini );
#endif

    HBAOSettings defaultHBAOSettings;
    s.HbaoSettings.Enabled = GetPrivateProfileBoolA( "HBAO", "Enabled", defaultHBAOSettings.Enabled, ini );
    s.HbaoSettings.Bias = GetPrivateProfileFloatA( "HBAO", "Bias", defaultHBAOSettings.Bias, ini );
    s.HbaoSettings.Radius = GetPrivateProfileFloatA( "HBAO", "Radius", defaultHBAOSettings.Radius, ini );
    s.HbaoSettings.PowerExponent = GetPrivateProfileFloatA( "HBAO", "PowerExponent", defaultHBAOSettings.PowerExponent, ini );
    s.HbaoSettings.BlurSharpness = GetPrivateProfileFloatA( "HBAO", "BlurSharpness", defaultHBAOSettings.BlurSharpness, ini );
    //s.HbaoSettings.EnableDualLayerAO = GetPrivateProfileIntA( "HBAO", "EnableDualLayerAO", defaultHBAOSettings.EnableDualLayerAO, ini.c_str() );
    s.HbaoSettings.EnableBlur = GetPrivateProfileBoolA( "HBAO", "EnableBlur", defaultHBAOSettings.EnableBlur, ini );
    s.HbaoSettings.SsaoBlurRadius = GetPrivateProfileIntA( "HBAO", "SsaoBlurRadius", defaultHBAOSettings.SsaoBlurRadius, ini.c_str() );
    s.HbaoSettings.SsaoStepCount = GetPrivateProfileIntA( "HBAO", "SsaoStepCount", defaultHBAOSettings.SsaoStepCount, ini.c_str() );

    s.EnableCustomFontRendering = GetPrivateProfileBoolA( "FontRendering", "Enable", defaultRendererSettings.EnableCustomFontRendering, ini );

    // Fix the shadow range
    s.WorldShadowRangeScale = Toolbox::GetRecommendedWorldShadowRangeScaleForSize( s.ShadowMapSize );

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
#if BUILD_SPACER_NET
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
            case WINDOW_MODE_FULLSCREEN_EXCLUSIVE: {
                s.DisplayFlip = false;
                s.StretchWindow = true;
                zSTRING section( "VIDEO" ); zSTRING defValue( "0" );
                zCOption::GetOptions()->WriteString( section, "zStartupWindowed", defValue );
                WritePrivateProfileStringA( "Display", "DisplayFlip", "0", ini.c_str() );
                WritePrivateProfileStringA( "Display", "LowLatency", "0", ini.c_str() );
                break;
            }
            case WINDOW_MODE_FULLSCREEN_BORDERLESS: {
                s.DisplayFlip = true;
                s.LowLatency = false;
                s.StretchWindow = true;
                WritePrivateProfileStringA( "Display", "DisplayFlip", "1", ini.c_str() );
                WritePrivateProfileStringA( "Display", "LowLatency", "0", ini.c_str() );
                break;
            }
            case WINDOW_MODE_FULLSCREEN_LOWLATENCY: {
                s.DisplayFlip = true;
                s.LowLatency = true;
                s.StretchWindow = true;
                WritePrivateProfileStringA( "Display", "DisplayFlip", "1", ini.c_str() );
                WritePrivateProfileStringA( "Display", "LowLatency", "1", ini.c_str() );
                break;
            }
            case WINDOW_MODE_WINDOWED: {
                s.DisplayFlip = false;
                s.StretchWindow = false;
                zSTRING section( "VIDEO" ); zSTRING defValue( "1" );
                zCOption::GetOptions()->WriteString( section, "zStartupWindowed", defValue );
                WritePrivateProfileStringA( "Display", "DisplayFlip", "0", ini.c_str() );
                WritePrivateProfileStringA( "Display", "LowLatency", "0", ini.c_str() );
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
void GothicAPI::AddStagingTexture( UINT mip, ID3D11Texture2D* stagingTexture, ID3D11Texture2D* texture ) {
    Engine::GAPI->EnterResourceCriticalSection();
    FrameStagingTextures.emplace_back( std::make_pair( mip, stagingTexture ), texture );
    Engine::GAPI->LeaveResourceCriticalSection();
}

/** Adds a mip map generation deferred command */
void GothicAPI::AddMipMapGeneration( D3D11Texture* texture ) {
    Engine::GAPI->EnterResourceCriticalSection();
    FrameMipMapGenerations.push_back( texture );
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
void GothicAPI::DrawMorphMesh( zCMorphMesh* msh, std::map<zCMaterial*, std::vector<MeshInfo*>>& meshes ) {
    XMFLOAT3 bbmin, bbmax;
    bbmin = XMFLOAT3( FLT_MAX, FLT_MAX, FLT_MAX );
    bbmax = XMFLOAT3( -FLT_MAX, -FLT_MAX, -FLT_MAX );

    zCProgMeshProto* morphMesh = msh->GetMorphMesh();
    if ( !morphMesh )
        return;

    XMFLOAT3* posList = morphMesh->GetPositionList()->Array->toXMFLOAT3();
    for ( int i = 0; i < morphMesh->GetNumSubmeshes(); i++ ) {
        std::vector<ExVertexStruct> vertices;

        zCSubMesh* s = morphMesh->GetSubmesh( i );
        vertices.reserve( s->WedgeList.NumInArray );
        for ( int v = 0; v < s->WedgeList.NumInArray; v++ ) {
            zTPMWedge& wedge = s->WedgeList.Array[v];
            vertices.emplace_back();
            ExVertexStruct& vx = vertices.back();
            vx.Position = posList[wedge.position];
            vx.Normal = wedge.normal;
            vx.TexCoord = wedge.texUV;
            vx.Color = 0xFFFFFFFF;
        }

        if ( zCTexture* texture = s->Material->GetAniTexture() ) {
            D3D11GraphicsEngine* g = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
            if ( !g->BindTextureNRFX( texture, (g->GetRenderingStage() == DES_MAIN) ) )
                continue;
        }

        for ( auto const& it : meshes ) {
            for ( MeshInfo* mi : it.second ) {
                if ( mi->MeshIndex == i ) {
                    mi->MeshVertexBuffer->UpdateBuffer( &vertices[0], vertices.size() * sizeof( ExVertexStruct ) );
                    Engine::GraphicsEngine->DrawVertexBufferIndexed( mi->MeshVertexBuffer, mi->MeshIndexBuffer, mi->Indices.size() );
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
                    MeshVisualInfo* mi = new MeshVisualInfo;
                    WorldConverter::ExtractProgMeshProtoFromModel( model, mi );
                    ParticleEffectProgMeshes[vob] = mi;
                } else if ( zCProgMeshProto* progMesh = emitter->GetVisShpProgMesh() ) {
                    MeshVisualInfo* mi = new MeshVisualInfo;
                    WorldConverter::Extract3DSMeshFromVisual2( progMesh, mi );
                    ParticleEffectProgMeshes[vob] = mi;
                } else if ( zCMesh* mesh = emitter->GetVisShpMesh() ) {
                    MeshVisualInfo* mi = new MeshVisualInfo;
                    WorldConverter::ExtractProgMeshProtoFromMesh( mesh, mi );
                    ParticleEffectProgMeshes[vob] = mi;
                }
            }
        }
    }
}

/** Destroy particle effect */
void GothicAPI::DestroyParticleEffect( zCVob* vob ) {
    auto it = ParticleEffectProgMeshes.find(vob);
    if ( it != ParticleEffectProgMeshes.end() ) {
        delete it->second;
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
const stdext::unordered_map<zCQuadMark*, QuadMarkInfo>& GothicAPI::GetQuadMarks() {
    return QuadMarks;
}

/** Returns wether the camera is underwater or not */
bool GothicAPI::IsUnderWater() {
    if ( oCGame::GetGame() &&
        oCGame::GetGame()->_zCSession_world &&
        oCGame::GetGame()->_zCSession_world->GetSkyControllerOutdoor() ) {
        return oCGame::GetGame()->_zCSession_world->GetSkyControllerOutdoor()->GetUnderwaterFX() != 0;
    }

    return false;
}

#if ENABLE_TESSELATION > 0
/** Saves all sections information */
void GothicAPI::SaveSectionInfos() {
    for ( auto&& itx : Engine::GAPI->GetWorldSections() ) {
        for ( auto&& ity : itx.second ) {
            // Save this section to file
            ity.second.SaveMeshInfos( LoadedWorldInfo->WorldName, INT2( itx.first, ity.first ) );
        }
    }
}

/** Loads all sections information */
void GothicAPI::LoadSectionInfos() {
    for ( std::map<int, std::map<int, WorldMeshSectionInfo>>::iterator itx = Engine::GAPI->GetWorldSections().begin(); itx != Engine::GAPI->GetWorldSections().end(); itx++ ) {
        for ( std::map<int, WorldMeshSectionInfo>::iterator ity = itx->second.begin(); ity != itx->second.end(); ity++ ) {
            WorldMeshSectionInfo& section = ity->second;

            // Load this section from file
            section.LoadMeshInfos( LoadedWorldInfo->WorldName, INT2( itx->first, ity->first ) );
        }
    }
}
#endif

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
    HANDLE f = FindFirstFile( "system\\GD3D11\\Textures\\Replacements\\*.dds", &data );
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

    system( "mkdir system\\GD3D11\\Textures\\Replacements\\Normalmaps_Original" );
    system( "move /Y system\\GD3D11\\Textures\\Replacements\\*.dds system\\GD3D11\\Textures\\Replacements\\Normalmaps_Original" );

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

        for ( size_t i = 0; i < sections.size(); i++ ) {
            for ( size_t p = 0; p < sections[i]->SectionPolygons.size(); p++ ) {
                zCPolygon* poly = sections[i]->SectionPolygons[p];

                if ( !poly->GetMaterial() || // Skip stuff with alpha-channel or not set material
                    poly->GetMaterial()->HasAlphaTest() )
                    continue;

                // Check all triangles
                for ( int v = 0; v < poly->GetNumPolyVertices(); v++ ) {
                    // Check if one vertex is inside the node // TODO: This will fail for very large triangles!
                    zCVertex** vx = poly->getVertices();

                    if ( Toolbox::PositionInsideBox( *vx[v]->Position.toXMFLOAT3(),
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
    for ( std::map<int, std::map<int, WorldMeshSectionInfo>>::iterator itx = Engine::GAPI->GetWorldSections().begin(); itx != Engine::GAPI->GetWorldSections().end(); itx++ ) {
        for ( std::map<int, WorldMeshSectionInfo>::iterator ity = itx->second.begin(); ity != itx->second.end(); ity++ ) {
            WorldMeshSectionInfo& section = ity->second;

            if ( Toolbox::AABBsOverlapping( section.BoundingBox.Min, section.BoundingBox.Max, min, max ) ) {
                sections.push_back( &section );
            }
        }
    }
}

/** Generates zCPolygons for the loaded sections */
void GothicAPI::CreatezCPolygonsForSections() {
    for ( std::map<int, std::map<int, WorldMeshSectionInfo>>::iterator itx = Engine::GAPI->GetWorldSections().begin(); itx != Engine::GAPI->GetWorldSections().end(); itx++ ) {
        for ( std::map<int, WorldMeshSectionInfo>::iterator ity = itx->second.begin(); ity != itx->second.end(); ity++ ) {
            WorldMeshSectionInfo& section = ity->second;

            for ( auto it = section.WorldMeshes.begin(); it != section.WorldMeshes.end(); ++it ) {
                if ( !it->first.Material ||
                    it->first.Material->HasAlphaTest() )
                    continue;

                it->first.Material->SetAlphaFunc( zMAT_ALPHA_FUNC_NONE );

                WorldConverter::ConvertExVerticesTozCPolygons( it->second->Vertices, it->second->Indices, it->first.Material, section.SectionPolygons );
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

/** Returns the current ocean-object */
GOcean* GothicAPI::GetOcean() {
    return Ocean.get();
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
    PrintMessageTimed( INT2( 5, 5 ), "GD3D11 - Version " + version );
    PrintMessageTimed( INT2( 5, 180 ), "Device: " + gpu );
}

#if ENABLE_TESSELATION > 0
/** Applies tesselation-settings for all mesh-parts using the given info */
void GothicAPI::ApplyTesselationSettingsForAllMeshPartsUsing( MaterialInfo* info, int amount ) {
    for ( std::map<int, std::map<int, WorldMeshSectionInfo>>::iterator itx = Engine::GAPI->GetWorldSections().begin(); itx != Engine::GAPI->GetWorldSections().end(); itx++ ) {
        for ( std::map<int, WorldMeshSectionInfo>::iterator ity = itx->second.begin(); ity != itx->second.end(); ity++ ) {
            WorldMeshSectionInfo& section = ity->second;


            for ( auto it = section.WorldMeshes.begin(); it != section.WorldMeshes.end(); ++it ) {
                if ( it->first.Info == info && it->second->IndicesPNAEN.empty() && info->TextureTesselationSettings.buffer.VT_TesselationFactor > 0.5f ) {
                    // Tesselate this mesh
                    WorldConverter::TesselateMesh( it->second, amount );
                }
            }
        }
    }
}
#endif

/** Returns the current weight of the rain-fx. The bigger value of ours and gothics is returned. */
float GothicAPI::GetRainFXWeight() {
    float myRainFxWeight = RendererState.RendererSettings.RainSceneWettness;
    float gRainFxWeight = 0.0f;

    if ( oCGame::GetGame() && oCGame::GetGame()->_zCSession_world
        && oCGame::GetGame()->_zCSession_world->GetSkyControllerOutdoor()
        && oCGame::GetGame()->_zCSession_world->GetSkyControllerOutdoor()->GetWeatherType() == zTWeather::zTWEATHER_RAIN )
        gRainFxWeight = oCGame::GetGame()->_zCSession_world->GetSkyControllerOutdoor()->GetRainFXWeight();

    // This doesn't seem to go as high as 1 or just very slowly. Scale it so it does go up quicker.
    gRainFxWeight = std::min( gRainFxWeight / 0.85f, 1.0f );

    // Return the higher of the two, so we get the chance to overwrite it
    return std::max( myRainFxWeight, gRainFxWeight );
}

/** Returns the wetness of the scene. Lasts longer than RainFXWeight */
float GothicAPI::GetSceneWetness() {
    float rain = GetRainFXWeight();
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
