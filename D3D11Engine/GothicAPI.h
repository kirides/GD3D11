#pragma once

#include "pch.h"
#include "AlignedAllocator.h"
#include "Frustum.h"
#include "BspPortalCuller.h"
#include "HorizonCuller.h"
#include "SpatialBVH.h"
#include "GothicGraphicsState.h"
#include "WorldConverter.h"
#include "zCTree.h"
#include "zCPolyStrip.h"
#include "zTypes.h"
#include "RenderQueue.h"
#include "ShaderIDs.h"
#include "TransparencyQueue.h"
#include "ThreadPool.h"
#include <shared_mutex>

static const char* MENU_SETTINGS_FILE = "system\\GD3D11\\UserSettings.ini";
const float INDOOR_LIGHT_DISTANCE_SCALE_FACTOR = 0.5f;

class zCFlash;
class zCBspBase;
class zCModelPrototype;
struct ScreenSpaceLine;
struct LineVertex;

struct RndCullContext {
    RndCullContext():
    frustum({}),
    cameraPosition({0,0,0}),
    stage(RenderStage::STAGE_DRAW_UNKNOWN),
    queue(nullptr),
    drawDistances({}),
    drawDistancesSq({}),
    drawFlags({})
    {
    }
    
    Frustum frustum;
    XMFLOAT3 cameraPosition;
    RenderStage stage;

    RenderQueue* queue;

    /** Sector/portal visibility for this pass, or null to not portal-cull at all. Only the main
        camera pass sets it: shadow passes must keep collecting casters in rooms the player cannot
        see into, and it is solved for the player camera anyway. */
    const class BspPortalCuller* portalCuller = nullptr;

    /** Ghost-occluder horizon cull for this pass, or null to not use it. Main camera pass only: the
        horizon is rasterized for the player's view, so a shadow cascade must not test against it. */
    const class HorizonCuller* horizon = nullptr;

    /** Drop static VOBs whose BaseVisualInfo::MeshSize (bounding-box diagonal) is below this, in world
        units. 0 = keep everything, which is what every main-view pass wants. Set only by the shadow
        cascades, which derive it from their own world-units-per-texel: a prop that resolves to under a
        couple of texels in a coarse cascade costs its full vertex + raster work for a smudge. Rejecting
        it here rather than at draw time also skips its instance upload and indirect command. */
    float minVobSize = 0.0f;

    struct
    {
        float OutdoorVobs;
        float OutdoorVobsSmall;
        float IndoorVobs;
        float VisualFX;
    } drawDistances;

    struct
    {
        float OutdoorVobs;
        float OutdoorVobsSmall;
        float IndoorVobs;
        float VisualFX;
    } drawDistancesSq;

    struct
    {
        bool DrawVOBs;
        bool DrawMobs;
        bool EnableDynamicLighting;
        bool EnableOcclusionCulling;
        bool CullVobs;
        // GPU-driven culling (D3D12): collect static VOBs with the DISTANCE test only and let the backend
        // frustum/occlusion-cull them on the GPU instead (D3D12Cull.cpp). Suppresses both the per-VOB
        // frustum test and the BSP-node frustum rejection that would drop whole subtrees behind the camera.
        // LIGHTS and skeletal MOBs keep their frustum tests either way — the light buffer is capped and the
        // per-draw skeletal path has no GPU cull yet. Defaults false: D3D11 never sets it.
        bool SkipVobFrustumCull;
        bool CollectIndoorVobs;
        bool CollectMobs;
        bool CollectLights;
    } drawFlags;
};

enum EBspTreeCollectFlags : unsigned int {
    COLLECT_VOBS = 1 << 0,
    COLLECT_LIGHTS = 1 << 1,
    COLLECT_MOBS = 1 << 2,
    COLLECT_INDOOR_VOBS = 1 << 3,

    COLLECT_ALL_VOBS = COLLECT_VOBS | COLLECT_INDOOR_VOBS,
    
    COLLECT_MUTATE = 1 << 30,
    COLLECT_ALL_MUTATE = 0xFFFFFFFF,
    COLLECT_ALL_NO_MUTATE = COLLECT_ALL_MUTATE & ~COLLECT_MUTATE,
};

/** Entry of a BSP leaf's static-VOB list.
 *
 *  The world position is mirrored into the list element instead of being read back out of the
 *  VobInfo. CollectLeafVobs' first act on every candidate is a distance reject, and with a bare
 *  VobInfo* list that reject had to dereference a scattered heap object per candidate - a cache
 *  miss for the majority that get rejected. Inline, the reject walks one contiguous 16-byte-stride
 *  array and only survivors ever touch the VobInfo.
 *
 *  The mirror cannot go stale: a static vob that moves is pulled out of every leaf list by
 *  MoveVobFromBspToDynamic (from OnVobMoved) *before* its transform is applied, so a VOB that is
 *  in a leaf list is by construction a VOB that has not moved since it was put there. */
struct LeafVobEntry {
    DirectX::XMFLOAT3 Position;
    VobInfo* Info;
};
static_assert( sizeof( LeafVobEntry ) == 16, "LeafVobEntry must stay 4-per-cache-line" );

struct BspInfo {
    BspInfo() {
        NumStaticLights = 0;
        OriginalNode = nullptr;
        Front = nullptr;
        Back = nullptr;

        OcclusionInfo.VisibleLastFrame = false;
        OcclusionInfo.LastVisitedFrameID = 0;
        OcclusionInfo.QueryID = -1;
        OcclusionInfo.QueryInProgress = false;
        OcclusionInfo.LastCameraClipType = ZTCAM_CLIPTYPE_OUT;

        OcclusionInfo.NodeMesh = nullptr;
    }

    BspInfo( BspInfo&& other ) noexcept {
        Vobs = std::move( other.Vobs );
        IndoorVobs = std::move( other.IndoorVobs );
        SmallVobs = std::move( other.SmallVobs );
        Lights = std::move( other.Lights );
        IndoorLights = std::move( other.IndoorLights );
        Mobs = std::move( other.Mobs );
        NodePolygons = std::move( other.NodePolygons );
        SectorIds = std::move( other.SectorIds );
        NumStaticLights = other.NumStaticLights;
        
        OcclusionInfo.NodeMesh = std::move(other.OcclusionInfo.NodeMesh);
        OriginalNode = other.OriginalNode;
        Front = other.Front;
        Back = other.Back;
    }

    BspInfo( const BspInfo& ) = delete;
    BspInfo& operator=( const BspInfo& ) = delete;

    ~BspInfo() {
        delete OcclusionInfo.NodeMesh;
    }

    bool IsEmpty() {
        return Vobs.empty() && IndoorVobs.empty() && SmallVobs.empty() && Lights.empty() && IndoorLights.empty();
    }

    std::vector<LeafVobEntry> Vobs;
    std::vector<LeafVobEntry> IndoorVobs;
    std::vector<LeafVobEntry> SmallVobs;
    std::vector<VobLightInfo*> Lights;
    std::vector<VobLightInfo*> IndoorLights;
    std::vector<SkeletalVobInfo*> Mobs;

    // This is filled in case we have loaded a custom worldmesh
    std::vector<zCPolygon*> NodePolygons;

    /** Sectors (rooms) this leaf holds polys of, as indices into BspPortalCuller's sector array.
        Empty on outdoor leafs, which is the vast majority - see BspPortalCuller::BuildFromWorld. */
    std::vector<uint16_t> SectorIds;

    int NumStaticLights;

    /** Occlusion info for this node */
    struct OcclusionInfo_s {
        MeshInfo* NodeMesh;
        unsigned int LastVisitedFrameID;
        int LastCameraClipType;
        int QueryID;
        bool VisibleLastFrame;
        bool QueryInProgress;
    } OcclusionInfo;

    // Original bsp-node
    zCBspBase* OriginalNode;
    BspInfo* Front;
    BspInfo* Back;
};

/** Pre-built linear cache of all BSP leaf bounding boxes for SIMD-accelerated frustum culling.
 *  Stores Min/Max extents in Structure-of-Arrays layout, 32-byte aligned for AVX2 batch processing.
 *  Padded to a multiple of 8 entries with sentinel values that always fail culling tests. */
struct BspLeafLinearCache {
    VectorA32<float> MinX, MinY, MinZ;
    VectorA32<float> MaxX, MaxY, MaxZ;
    std::vector<BspInfo*> Leaves;
    uint32_t Count = 0;

    void Build( BspInfo* root );
    void Clear();
};


struct CameraReplacement {
    XMFLOAT4X4 ViewReplacement;
    XMFLOAT4X4 ProjectionReplacement;
    XMFLOAT3 PositionReplacement;
    XMFLOAT3 LookAtReplacement;
    
    Frustum frustum;
};

/** Version of this struct */
const int MATERIALINFO_VERSION = 6;

struct MaterialInfo {
    enum EMaterialType {
        MT_None,
        MT_Water,
        MT_Ocean,
        MT_Portal,
        MT_WaterfallFoam,
        MT_FullAlpha, // why does this exist "NW_MISC_FULLALPHA_01" ?? This is just a block of nothing
    };

    MaterialInfo() :
        PixelShader(static_cast<PShaderID>(0)),
        MaterialType(MT_None)
    {
        buffer.SetDefault();
    }

    ~MaterialInfo() = default;

    MaterialInfo( MaterialInfo&& other ) = default;
    MaterialInfo& operator=( MaterialInfo&& ) = default;

    MaterialInfo(const MaterialInfo&) = delete;
    MaterialInfo& operator=( const MaterialInfo& ) = delete;

    /** Writes this info to a file */
    void WriteToFile( const std::string_view name );

    /** Loads this info from a file */
    void LoadFromFile( const std::string_view name );

    struct Buffer {
        float SpecularIntensity;
        float SpecularPower;
        float NormalmapStrength;
        float DisplacementFactor;
        float4 Color;

        void SetDefault() {
            // -- Defaults for NON Normalmapped, NON FX-Mapped materials
            SpecularIntensity = 0.1f;
            SpecularPower = 5.0f;
            // ---
            
            NormalmapStrength = 1.0f;
            DisplacementFactor = 0.1f;
            Color = 0xFFFFFFFF;
        }

        static bool FloatEqualEps(float a, float b, float epsilon = 0.0001f) noexcept{
            return std::abs(a - b) <= epsilon;
        }
        
        bool operator==( const Buffer& other ) const noexcept {
            return FloatEqualEps(SpecularIntensity, other.SpecularIntensity) &&
                FloatEqualEps(SpecularPower, other.SpecularPower) &&
                FloatEqualEps(NormalmapStrength, other.NormalmapStrength) &&
                FloatEqualEps(DisplacementFactor, other.DisplacementFactor) &&
                Color == other.Color;
        }
    };

    PShaderID PixelShader;
    EMaterialType MaterialType;
    Buffer buffer;

    bool IsSame(const MaterialInfo* other ) const {
        if ( other == nullptr ) return false;
        return PixelShader == other->PixelShader
            && MaterialType == other->MaterialType
            && buffer == other->buffer;
    }
};

struct ParticleFrameData {
    unsigned char* Buffer;
    unsigned int BufferPosition;
    unsigned int BufferSize;
    unsigned int NeededSize;
};

struct PolyStripInfo {
    std::vector<ExVertexStruct> vertices;
    zCMaterial* material;
    zCVob* vob;
};

/** One mip-level upload handed over by ZENGIN's resource-loader thread: a filled STAGING texture and
    the texture it has to be copied into once the game thread owns the immediate context again.

    Both are strong references on purpose. The surface owning the destination can be cached out - and
    with it its D3D11Texture deleted - at any point between the loader thread queueing this and the
    game thread draining the queue at frame start (turning normalmaps on/off purges the whole texture
    cache, which does exactly that, repeatedly). Handing a freed ID3D11Texture2D to
    CopySubresourceRegion trips the debug layer's interface sentinel and corrupts driver state in
    release builds, so the queue keeps both resources alive by itself instead of relying on whoever
    created them still being around. */
struct DeferredMipUpload {
    GfxTexture* Texture; // only to know which one to delete in case its removed
    UINT Mip;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> Staging;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> Destination;
};

/** Class used to communicate between Gothic and the Engine */
class zCPolygon;
class zCTexture;
class zCParticleFX;
class zCVisual;
class GSky;
class GMesh;
class zCBspBase;
class GInventory;
class zCVobLight;
class MyDirectDrawSurface7;
class GVegetationBox;
class zCMorphMesh;
class zCDecal;

class GothicAPI {
public:
    GothicAPI();
    ~GothicAPI();

    /** Call to OnRemoveVob(all skeletal vobs) and OnAddVob(all skeletal vobs) in case of invisibility */
    void ReloadVobs();
    /** Call to OnRemoveVob(player) and OnAddVob(player) in case of invisibility */
    void ReloadPlayerVob();

    inline const std::string& GetGameName() const { return m_gameName; }
    inline void SetGameName( std::string value ) { m_gameName = std::move(value); }

    /** Called when the game starts */
    void OnGameStart();

    /** Called when the window got set */
    void OnSetWindow( HWND hWnd );

    /** Message-Callback for the main window */
    LRESULT OnWindowMessage( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam );

    /** Sends a message to the original gothic-window */
    void SendMessageToGameWindow( UINT msg, WPARAM wParam, LPARAM lParam );

    /** Called when the game is about to load a new level */
    void OnLoadWorld( const std::string& levelName, int loadMode );

    /** Called when the game loaded a new level */
    void OnGeometryLoaded( zCBspTree* tree );

    /** Called when the game is done loading the world */
    void OnWorldLoaded();

    /** Sets the per mod & per world renderersettings which can be persisted */
    void LoadRendererWorldSettings( GothicRendererSettings& s );
    void LoadRendererWorldSettings( GothicRendererSettings& s, const char* iniFile );

    /** Persists the per mod & per world renderersettings */
    void SaveRendererWorldSettings( const GothicRendererSettings& s );
    void SaveRendererWorldSettings( const GothicRendererSettings& s, const char* iniFile );

    /** Called to update the multi thread resource manager state */
    void UpdateMTResourceManager();

    /** Called to update the compress backbuffer state */
    void UpdateCompressBackBuffer();

    /** Called to update the texture quality */
    void UpdateTextureMaxSize();

    /** Called to update the world, before rendering */
    void OnWorldUpdate();

    /** Called when a VOB got added to the BSP-Tree or the world */
    void OnAddVob( zCVob* vob, zCWorld* world );

    /** Called when a VOB got removed from the world. tearDownLight=false is for vobs that are merely
        being hidden (zCVobLight can be enabled/disabled at will, reusing the same zCVob* both times)
        rather than actually destroyed - the light's VobLightInfo/shadow buffers must survive that, since
        light collection expects them to persist and relies on zCVobLight::IsEnabled() to filter hidden
        lights out of rendering, not on the VobLightInfo being torn down and recreated every toggle. */
    void OnRemovedVob( zCVob* vob, zCWorld* world, bool tearDownLight = true );

    /** Called on a SetVisual-Call of a vob */
    void OnSetVisual( zCVob* vob );

    /** Called when a material got removed */
    void OnMaterialDeleted( zCMaterial* mat );

    /** Called when a particle system got removed */
    void OnParticleFXDeleted( zCParticleFX* pfx );

    /** Called when a visual got removed */
    void OnVisualDeleted( zCVisual* visual );

    /** Called when a vob moved */
    void OnVobMoved( zCVob* vob );

    /** Called when a material got removed */
    void OnMaterialCreated( zCMaterial* mat );

    /** Loads resources created for this .ZEN */
    void LoadCustomZENResources();

    /** Saves resources created for this .ZEN */
    void SaveCustomZENResources();

    /** Returns the GraphicsState */
    GothicRendererState& GetRendererState();

    /** Returns in which directory we started in */
    const std::string& GetStartDirectory();

    /** Draws the world-mesh */
    void DrawWorldMeshNaive();

    /** Draws a skeletal mesh-vob */
    void DrawSkeletalMeshVob( SkeletalVobInfo* vi, float distance, bool updateState = true, const std::move_only_function<bool( const zCVob* ) const>& ignoreVob = nullptr );

    void DrawSkeletalMeshVob_Layered( SkeletalVobInfo* vi, float distance, bool updateState = true, const std::move_only_function<bool( const zCVob* ) const>& ignoreVob = nullptr );

    /** Shared state for a run of ghost draws; per run, since the queue interleaves kinds. */
    void BeginTransparencyVobRun();

    /** One ghost vob (z-prepass with a null PS, then the transparency shader). D3D11 only. */
    void DrawTransparencyVob( const TransparencyVobInfo& info );

    void DrawSkeletalVN();

    /** The transparency queue stores indices into this. */
    std::vector<TransparencyVobInfo>& GetTransparencyVobs() { return TransparencyVobs; }

    TransparencyQueue& GetTransparencyQueue() { return TransparencyQueueData; }

    /** Draws the inventory */
    void DrawInventory( zCWorld* world, zCCamera& camera );

    /** Draws a morphmesh */
    void DrawMorphMesh( zCMorphMesh* msh, std::map<zCMaterial*, std::vector<std::unique_ptr<MeshInfo>>>& meshes );
    void DrawMorphMesh_Layered( zCMorphMesh* msh, std::map<zCMaterial*, std::vector<std::unique_ptr<MeshInfo>>>& meshes );

    /** Locks the resource CriticalSection */
    void EnterResourceCriticalSection();

    /** Unlocks the resource CriticalSection */
    void LeaveResourceCriticalSection();

    /** Adds a future to the internal buffer */
    void AddFuture( std::future<void>& future );

    /** Checks which futures are ready and cleans them */
    void CleanFutures();

    /** Draws a MeshInfo */
    void DrawMeshInfo( zCMaterial* mat, MeshInfo* msh );
    void DrawMeshInfo_Layered( zCMaterial* mat, MeshInfo* msh );

    /** Draws a zCParticleFX */
    void DrawParticleFX( zCVob* source, zCParticleFX* fx, ParticleFrameData& data );

    /** Gets a list of visible decals */
    void GetVisibleDecalList( std::vector<zCVob*>& decals );

    /** Returns a list of visible particle-effects */
    void GetVisibleParticleEffectsList( std::vector<zCVob*>& pfxList );

    /** Sets the Projection matrix */
    void XM_CALLCONV SetProjTransformXM( const XMMATRIX proj );

    /** Gets the Projection matrix */
    XMFLOAT4X4 GetProjTransform();

    /** Sets the world matrix */
    void XM_CALLCONV  SetWorldTransformXM( XMMATRIX world, bool transpose = false );


    /** Sets the world matrix */
    void XM_CALLCONV SetViewTransformXM( XMMATRIX view, bool transpose = false );

    /** Sets the world matrix */
    void SetViewTransform( const XMFLOAT4X4& view, bool transpose = false );

    /** Sets the world matrix */
    void SetWorldViewTransform( const XMFLOAT4X4& world, const XMFLOAT4X4& view );

    /** Sets the world matrix */
    void XM_CALLCONV SetWorldViewTransform( XMMATRIX world, CXMMATRIX view );

    /** Sets the world matrix */
    void ResetWorldTransform();

    /** Gets if player is NOT in dialog */
    int DialogFinished();

    /** Sets the world matrix */
    void ResetViewTransform();

    /** Debugging */
    void DrawTriangle( float3 pos );

    /** Add particle effect */
    void AddParticleEffect( zCVob* vob );

    /** Destroy particle effect */
    void DestroyParticleEffect( zCVob* vob );

    /** Removes the given quadmark */
    void RemoveQuadMark( zCQuadMark* mark );

    /** Returns wether the camera is underwater or not */
    bool IsUnderWater();

    /** Returns the quadmark info for the given mark. Creates a new one if needed. */
    QuadMarkInfo* GetQuadMarkInfo( zCQuadMark* mark );

    /** Returns all quad marks */
    const std::unordered_map<zCQuadMark*, QuadMarkInfo>& GetQuadMarks();

    /** Add new zCFlash object */
    void AddFlash( zCFlash* flash, zCVob* vob );

    /** Remove zCFlash object */
    void RemoveFlash( zCFlash* flash );

    /** Add this frame thunder poly strip */
    void AddThunderPolyStrip( zCPolyStrip* polyStrip );

    /** Returns the loaded sections */
    std::map<int, std::map<int, WorldMeshSectionInfo>>& GetWorldSections();

    /** Returns the wrapped world mesh */
    MeshInfo* GetWrappedWorldMesh();

    /** Returns the loaded skeletal mesh vobs */
    std::vector<SkeletalVobInfo*>& GetSkeletalMeshVobs();
    std::vector<SkeletalVobInfo*>& GetAnimatedSkeletalMeshVobs();
    std::vector<VobInfo*>& GetDynamicallyAddedVobs();

    /** Non-skeletal mesh vobs with StaticVob clear (items) -- moving-caster source for the D3D12
     *  point-shadow dynamic overlay, mirroring GetSkeletalMeshVobs. */
    std::vector<VobInfo*>& GetDynamicMeshVobs();

    /** Returns the current cameraposition */
    XMFLOAT3 GetCameraPosition();
    XMVECTOR XM_CALLCONV GetCameraPositionXM();
    zTCam_ClipType GetCameraBBox3DInFrustum(const zTBBox3D& box, int clipFlags = EGothicCullFlags::CullAll);
    zTCam_ClipType GetCameraBBox3DInFrustum(const zCVob* vob, int clipFlags, bool isLocalCamera);

    /** Returns the view matrix */
    void GetViewMatrix( XMFLOAT4X4* view );
    XMMATRIX XM_CALLCONV GetViewMatrixXM();

    /** Returns the view matrix */
    void GetInverseViewMatrixXM( XMFLOAT4X4* invView );

    /** Returns the projection-matrix */
    XMFLOAT4X4& GetProjectionMatrix();

    /** Unprojects a pixel-position on the screen */
    void XM_CALLCONV UnprojectXM(float2 p, XMVECTOR& worldPos, XMVECTOR& worldDir);

    /** Unprojects the current cursor, returns it's direction in world-space */
    XMVECTOR XM_CALLCONV UnprojectCursorXM();

    /** Traces the worldmesh and returns the hit-location */
    bool TraceWorldMesh( const XMFLOAT3& origin, const XMFLOAT3& dir, XMFLOAT3& hit, std::string* hitTextureName = nullptr, XMFLOAT3* hitTriangle = nullptr, MeshInfo** hitMesh = nullptr, zCMaterial** hitMaterial = nullptr );

    /** Traces vobs with static mesh visual */
    VobInfo* TraceStaticMeshVobsBB( const XMFLOAT3& origin, const XMFLOAT3& dir, XMFLOAT3& hit, zCMaterial** hitMaterial = nullptr );
    SkeletalVobInfo* TraceSkeletalMeshVobsBB( const XMFLOAT3& origin, const XMFLOAT3& dir, XMFLOAT3& hit );

    /** Traces a visual info. Returns -1 if not hit, distance otherwise */
    float TraceVisualInfo( const XMFLOAT3& origin, const XMFLOAT3& dir, BaseVisualInfo* visual, zCMaterial** hitMaterial = nullptr );

    /** Returns the GSky-Object */
    GSky* GetSky() const;

    /** Returns the far Z */
    float GetFarZ();

    /** Returns the fog-color */
    XMVECTOR GetFogColor();

    /** Returns true if the game is overwriting the fog color with a fog-zone */
    float GetFogOverride();

    /** Returns the inventory */
    GInventory* GetInventory();

    /** Returns if the material is currently active */
    bool IsMaterialActive( zCMaterial* mat ) const;

    /** Sets the current input state. Keeps an internal count of how many times it was disabled. */
    void SetEnableGothicInput( bool value );

    /** Returns the midpoint of the current world */
    WorldInfo* GetLoadedWorldInfo() { return LoadedWorldInfo.get(); }

    [[nodiscard]] std::string GetLoadedWorldSettingsPath(bool createPath = false) const {
        if ( !LoadedWorldInfo || LoadedWorldInfo->WorldName.empty() ) {
            return "";
        }
        auto gameName = GetGameName();
        std::string zenFolder;
        if ( gameName == "Original" ) {
            zenFolder = "system\\GD3D11\\ZENResources\\";
        } else {
            zenFolder = "system\\GD3D11\\ZENResources\\" + gameName + "\\";
        }
        if ( !Toolbox::FolderExists( zenFolder ) ) {
            if (createPath) {
                if ( !Toolbox::CreateDirectoryRecursive( zenFolder ) ) {
                    LogError() << "Could not save custom ZEN-Resources. Could not create directory: " << zenFolder;
                    return "";
                }
            }
        }

        auto const ini = zenFolder + LoadedWorldInfo->WorldName + ".INI";
        return ini;
    }

    /** Returns wether the camera is indoor or not */
    bool IsCameraIndoor();

    /** Alpha of ZenGin's env-map overlay stage for this material — see zCRenderManager::BuildShader
        (zRenderManager.cpp:701-703). Backend-neutral because both renderers draw the same stage. */
    float GetEnvMapStageAlpha( zCMaterial* mat );

    /** Sky-fog intensity (0..1) ZenGin's env-map stage scales its sheen by. Peaks well below 1.0
        even at noon - not a brightness multiplier, see GetSkyDayFactor for that. */
    float GetSkyLightIntensity();

    /** Day/night brightness (kNightFactor..1.0) for the alpha-blended world surfaces that are drawn
        unlit over static, baked-daylight vertex colors. Exactly 1.0 while the sun is up. */
    float GetSkyDayFactor();

    /** Returns whether the loaded world itself is an indoor level (mines, dungeons, ...).
        This is a per-world property baked into the compiled BSP-tree, not a per-frame camera
        test - ZenGin swaps in a zCSkyControler_Indoor for these worlds, which renders no sky
        and sets up no fog (see oCGame::EnvironmentInit). */
    bool IsIndoorWorld() const;

    /** Returns gothics fps-counter */
    int GetFramesPerSecond();

    /** Returns true, if the game was paused */
    bool IsGamePaused();

    /** Returns true while an in-game menu holds the game paused. Stricter than IsGamePaused():
        requires a live game session, so startup/teardown don't read as "paused". */
    bool IsIngameMenuPaused();

    /** Checks if a game is being saved now */
    bool IsSavingGameNow();

    /** Checks if a game is being saved or loaded now */
    bool IsInSavingLoadingState();

    /** Returns total time */
    float GetTotalTime();

    /** Returns total time DWORD */
    DWORD GetTotalTimeDW();

    /** Monotonic frame counter, bumped once per OnWorldUpdate. Unlike GetTotalTimeDW (a
        millisecond wall clock) two distinct frames can never share a value. */
    size_t GetFrameNumber() const { return FrameNumber; }

    /** Returns the current frame time */
    float GetFrameTimeSec();

    /** Returns global time */
    float GetTimeSeconds();

    /** Called once per frame from each backend's OnEndFrame, after everything for the frame has been
        recorded. Snapshots this frame's GetFrameTimeSec() (GetPreviousFrameTimeSec() hands it out for
        the whole of the NEXT frame), and rolls StableTimeSec (GetStableTimeSec()) forward — stashing
        its pre-roll value first, so GetPreviousStableTimeSec() can hand out exactly the GetStableTimeSec()
        value that the frame which just ended rendered with. See the three getters for why none of them
        are derived from a fresh GetTimeSeconds()/GetFrameTimeSec() call. */
    void OnEndFrame() {
        PreviousFrameTimeSec = GetFrameTimeSec();
        PreviousStableTimeSec = StableTimeSec;
        StableTimeSec += PreviousFrameTimeSec;
    }

    /** Returns the frame time (seconds) of the last fully-completed frame — see OnEndFrame(). */
    float GetPreviousFrameTimeSec() const { return PreviousFrameTimeSec; }

    /** Frame-stable substitute for GetTimeSeconds(): a running total built purely from summed
        GetFrameTimeSec() snapshots (see OnEndFrame()), fixed for the entire current frame instead of
        tracking ZenGin's live timer. GetTimeSeconds() itself can change value between two call sites
        within the same frame (e.g. a nested menu loop re-entering zCTimer::ResetTimer, or simply a
        depth prepass and a later color pass observing different totalTimeFloatSecs), which desyncs any
        per-vertex animation (wind sway, …) driven straight from it — the prepass and the color pass end
        up rasterizing the swayed geometry at different positions and z-fight. Use this wherever a
        time-driven quantity must render identically across every pass of one frame. */
    float GetStableTimeSec() const { return StableTimeSec; }

    /** Returns the exact GetStableTimeSec() value the PREVIOUS frame rendered with — the literal prior
        total, stashed in OnEndFrame() before StableTimeSec rolls forward. Use this instead of
        `GetStableTimeSec() - GetPreviousFrameTimeSec()`: that reconstruction happens to check out
        algebraically (both terms are updated together, from the same OnEndFrame() call, so the delta
        it subtracts is guaranteed to be the one that produced GetStableTimeSec()'s last increment) but
        it needlessly conflates a timestamp with a duration and re-derives what OnEndFrame() already
        has on hand — store-and-return is the harder-to-misuse form of the same value. */
    float GetPreviousStableTimeSec() const { return PreviousStableTimeSec; }

    /** Builds the static mesh instancing cache */
    void BuildStaticMeshInstancingCache();

    /** Draws the AABB for the BSP-Tree using the line renderer*/
    void DebugDrawBSPTree();

    /** Outlines the world's ghost occluders. Gated by RendererSettings::DrawWorldOccluders. */
    void DebugDrawOccluders( const Frustum& frustum );

    /** Recursive helper function to draw the BSP-Tree */
    void DebugDrawTreeNode( zCBspBase* base, zTBBox3D boxCell, int clipFlags = 63 );

    /** Prepares this frame's particle draw data (visibility + FX collection) and draws the
        particle prog-meshes. Backend-neutral: the concrete engine draws the collected
        FrameParticles into its own refraction targets afterwards. */
    void DrawParticlesSimple();

    /** Prepares poly strips for feeding into renderer (weapon and effect trails) */
    void CalcPolyStripMeshes();
    void CalcFlashMeshes();

    /** Moves the given vob from a BSP-Node to the dynamic vob list */
    void MoveVobFromBspToDynamic( VobInfo* vob );
    void MoveVobFromBspToDynamic( SkeletalVobInfo* vob );

    std::vector<LeafVobEntry>::iterator MoveVobFromBspToDynamic( VobInfo* vob, std::vector<LeafVobEntry>* source );

    /** Collects vobs using gothics BSP-Tree */
    void CollectVisibleVobs(
        std::vector<VobInfo*>& vobs,
        std::vector<VobLightInfo*>& lights,
        std::vector<SkeletalVobInfo*>& mobs,
        EGothicCullFlags cullFlags = EGothicCullFlags::CullAll,
        EBspTreeCollectFlags collectFlags = EBspTreeCollectFlags::COLLECT_ALL_MUTATE,
        // true => distance-only static-VOB collection; the caller frustum/occlusion-culls them itself
        // (D3D12's GPU cull). See RndCullContext::drawFlags.SkipVobFrustumCull.
        bool skipVobFrustumCull = false );

    void CollectVisibleVobs( const RndCullContext& ctx );

    /** Collects visible sections from the current camera perspective.
        `horizon` opts into the ghost-occluder cull and must stay null for every pass that is not the
        player's view - it is an explicit parameter rather than a member read precisely so a shadow or
        rain frustum cannot silently inherit the player's skyline. */
    void CollectVisibleSections( std::vector<WorldMeshSectionInfo*>& sections,
        const Frustum* queryFrustum = nullptr,
        bool useSectionRadiusFilter = true,
        const HorizonCuller* horizon = nullptr );

    /** Returns whether a world mesh intersects the given frustum (true when no bounds are available). */
    bool IsWorldMeshVisibleInFrustum( const WorldMeshInfo* mesh, const Frustum& frustum ) const;

    /** Finer-grained sibling of CollectVisibleSections: queries the world-mesh CLUSTER BVH (built
        alongside the section BVH, see BuildWorldMeshClusterBVH) instead of section bounding boxes, so
        a small-range query (a point light's cube face) only pulls in the triangles actually near it
        instead of every section its bounds happen to touch. Adjacent surviving clusters belonging to
        the same mesh are fused into one range (same rule D3D12's CoalesceWorldDepthCommands already
        uses for its indirect draws), so callers get a handful of ranges to draw, not one per cluster. */
    void CollectVisibleMeshRanges( const Frustum& frustum,
        bool useSectionRadiusFilter,
        const HorizonCuller* horizon,
        std::vector<MeshDrawRange>& outRanges );

    /** Builds our BspTreeVobMap */
    void BuildBspVobMapCache();

    /** Sector/portal visibility, rebuilt on every world load. Inactive on worlds without portals. */
    BspPortalCuller& GetPortalCuller() { return PortalCuller; }
    const BspPortalCuller& GetPortalCuller() const { return PortalCuller; }

    /** Ghost-occluder horizon, rebuilt once per frame for the player camera. */
    HorizonCuller& GetHorizonCuller() { return Horizon; }
    const HorizonCuller& GetHorizonCuller() const { return Horizon; }

    /** True when the view is fully enclosed by sectors, so the sun cascades need not be rendered and
        are cleared to "shadowed" instead. Reads the solve the main camera pass ran this frame, so call
        it only after CollectVisibleVobs. */
    bool AreSunShadowsFullyOccluded() const {
        return RendererState.RendererSettings.EnablePortalShadowSkip
            && PortalCuller.IsActive()
            && !PortalCuller.IsOutdoorVisible();
    }

    /** Returns the new node from tha base node */
    BspInfo* GetNewBspNode( zCBspBase* base );

    /** Returns our bsp-root-node */
    BspInfo* GetNewRootNode();

    /** Sets/Gets the far-plane */
    void SetFarPlane( float value );
    float GetFarPlane();

    /** Sets/Gets the far-plane */
    void SetNearPlane( float value );
    float GetNearPlane();

    /** Reloads all textures */
    void ReloadTextures();

    /** Returns true if the given string can be found in the commandline */
    bool HasCommandlineParameter( const std::string& param );

    /** Gets the int-param from the ini. String must be UPPERCASE. */
    int GetIntParamFromConfig( const std::string& param );

    /** Sets the given int param into the internal ini-cache. That does not set the actual value for the game! */
    void SetIntParamFromConfig( const std::string& param, int value );

    /** Resets the object, like at level load */
    void ResetWorld();

    /** Resets only the vobs */
    void ResetVobs();

    /** Get material by texture name */
    zCMaterial* GetMaterialByTextureName( const std::string& name );
    void GetMaterialListByTextureName( const std::string& name, std::list<zCMaterial*>& list );

    /** Returns the time since the last frame */
    float GetDeltaTime();

    /** If this returns true, the property holds the name of the currently bound texture. If that is the case, any MyDirectDrawSurfaces should not bind themselfes
        to the pipeline, but rather check if there are additional textures to load */
    bool IsInTextureTestBindMode( std::string& currentBoundTexture );

    /** Sets the current texture test bind mode status */
    void SetTextureTestBindMode( bool enable, const std::string& currentTexture );

    /** Sets the CameraReplacementPtr */
    void SetCameraReplacementPtr( CameraReplacement* ptr ) { CameraReplacementPtr = ptr; }
    CameraReplacement* GetCameraReplacementPtr() const { return CameraReplacementPtr; }

    /** Lets Gothic draw its sky */
    void DrawSkyGothicOriginal();

    /** Reset's the material info that were previously gathered */
    void ResetMaterialInfo();
    /** Returns the material info associated with the given material */
    MaterialInfo* GetMaterialInfoFrom(void* any, std::string_view materialName);
    MaterialInfo* GetMaterialInfoFrom(zCMaterial* mat);

    /** Returns a texture from the given surface */
    zCTexture* GetTextureBySurface( MyDirectDrawSurface7* surface );

    /** Resets all vob-stats drawn this frame */
    void ResetVobFrameStats();

    /** Sets the currently bound texture */
    void SetBoundTexture( int idx, zCTexture* tex );
    zCTexture* GetBoundTexture( int idx );

    /** The zCTexture ZENGIN is currently caching in, on *this* thread. MyDirectDrawSurface7::Unlock has
        no other way of finding out which zCTexture the surface it is unlocking belongs to, so the load
        path has to smuggle it through here. This must not be process-global: zCResourceManager derives
        from zCThread and ships with its loader thread enabled (only -ZNORESTHREAD turns it off), so the
        game thread and that loader thread can both sit inside zCTexture::LoadResourceData at the same
        time. A shared slot lets them clobber each other, and the surface latches the resulting wrong
        name permanently.
        Always set it through ScopedLoadingTexture - it restores the previous value, which keeps nested
        cache-ins (a texture loaded from inside another texture's load) correct. */
    static void SetLoadingTexture( zCTexture* tex );
    static zCTexture* GetLoadingTexture();

    /** Publishes 'tex' as the texture being cached in on this thread for the lifetime of the object. */
    struct ScopedLoadingTexture {
        explicit ScopedLoadingTexture( zCTexture* tex ) : Previous( GothicAPI::GetLoadingTexture() ) {
            GothicAPI::SetLoadingTexture( tex );
        }
        ~ScopedLoadingTexture() { GothicAPI::SetLoadingTexture( Previous ); }

        ScopedLoadingTexture( const ScopedLoadingTexture& ) = delete;
        ScopedLoadingTexture& operator=( const ScopedLoadingTexture& ) = delete;

    private:
        zCTexture* Previous;
    };

    /** Returns gothics output window */
    HWND GetOutputWindow() { return OutputWindow; }

    /** Spawns a vegetationbox at the camera */
    GVegetationBox* SpawnVegetationBoxAt( const XMFLOAT3& position, const XMFLOAT3& min = XMFLOAT3( -1000, -500, -1000 ), const XMFLOAT3& max = XMFLOAT3( 1000, 500, 1000 ), float density = 1.0f, const std::string& restrictByTexture = "" );

    /** Adds a vegetationbox to the world */
    void AddVegetationBox( GVegetationBox* box );

    /** Returns the list of current GVegentationBoxes */
    const std::list<GVegetationBox*>& GetVegetationBoxes() { return VegetationBoxes; }

    /** Removes a vegetationbox from the world */
    void RemoveVegetationBox( GVegetationBox* box );

    /** Teleports the player to the given location */
    void SetPlayerPosition( const XMFLOAT3& pos );

    /** Returns the player-vob */
    zCVob* GetPlayerVob();

    /** Returns the map of static mesh visuals */
    const gtl::flat_hash_map<zCProgMeshProto*, MeshVisualInfo*>& GetStaticMeshVisuals() { return StaticMeshVisuals; }

    /** Returns the collection of PolyStrip meshes infos */
    const std::map<zCTexture*, PolyStripInfo>& GetPolyStripInfos() { return PolyStripInfos; };

    /** Removes the given texture from the given section and stores the supression, so we can load it next time */
    void SupressTexture( WorldMeshSectionInfo* section, const std::string& texture );

    /** Resets the suppressed textures */
    void ResetSupressedTextures();

    /** Resets the vegetation */
    void ResetVegetation();

    /** Saves Suppressed textures to a file */
    XRESULT SaveSuppressedTextures( const std::string& file );

    /** Saves Suppressed textures to a file */
    XRESULT LoadSuppressedTextures( const std::string& file );

    /** Saves vegetation to a file */
    XRESULT SaveVegetation( const std::string& file );

    /** Saves vegetation to a file */
    XRESULT LoadVegetation( const std::string& file );

    /** Returns the main-thread id */
    DWORD GetMainThreadID();

    /** Returns the current cursor position, in pixels */
    POINT GetCursorPosition();

    /** Returns the current weight of the rain-fx. The bigger value of ours and gothics is returned. */
    float GetRainFXWeight();

    /** Returns true if gothic's current outdoor weather is snow */
    bool IsSnowingWeather();

    /** Returns the wetness of the scene. Lasts longer than RainFXWeight. Always 0 while it snows. */
    float GetSceneWetness();

    /** Saves the users settings from the menu */
    XRESULT SaveMenuSettings( const std::string& file );

    /** Loads the users settings from the menu */
    XRESULT LoadMenuSettings( const std::string& file );

    /** Adds a staging texture to the list of the staging textures for this frame */
    void AddStagingTexture( GfxTexture* gfx, UINT mip, const Microsoft::WRL::ComPtr<ID3D11Texture2D>& stagingTexture,
        const Microsoft::WRL::ComPtr<ID3D11Texture2D>& texture );

    /** Gets a list of the staging textures for this frame */
    std::deque<DeferredMipUpload>& GetStagingTextures() { return FrameStagingTextures; }

    /** Adds a mip map generation deferred command */
    void AddMipMapGeneration( GfxTexture* texture );

    /** Drops any pending deferred commands referencing this texture. Must be called from
        ~GfxTexture-implementations, or the game thread would dispatch them into a freed object. */
    void RemovePendingTextureCommands( GfxTexture* texture );

    /** Gets a list of the mip map generation commands for this frame */
    std::deque<GfxTexture*>& GetMipMapGeneration() {return FrameMipMapGenerations;}

    /** Adds a texture to the list of the loaded textures for this frame */
    void AddFrameLoadedTexture( MyDirectDrawSurface7* srf );

    /** Sets loaded textures of this frame ready */
    void SetFrameProcessedTexturesReady();

    /** Returns if the given vob is registered in the world */
    SkeletalVobInfo* GetSkeletalVobByVob( zCVob* vob );

    /** Returns the frame particle info collected from all DrawParticleFX-Calls */
    std::map<zCTexture*, ParticleRenderInfo>& GetFrameParticleInfo();

    /** Returns this frame's collected particle instances (populated by DrawParticlesSimple). */
    std::map<zCTexture*, std::vector<ParticleInstanceInfo>>& GetFrameParticles() { return FrameParticles; }

    /** Checks if the normalmaps are there */
    bool CheckNormalmapFilesOld();

    /** Returns the gamma value from the ingame menu */
    float GetGammaValue();

    /** Returns the brightness value from the ingame menu */
    float GetBrightnessValue();

    /** Returns the sections intersecting the given boundingboxes */
    void GetIntersectingSections( const XMFLOAT3& min, const XMFLOAT3& max, std::vector<WorldMeshSectionInfo*>& sections );

    /** Generates zCPolygons for the loaded sections */
    void CreatezCPolygonsForSections();

    /** Collects polygons in the given AABB */
    void CollectPolygonsInAABB( const zTBBox3D& bbox, zCPolygon**& polyList, int& numFound );

    /** Loads the data out of a zCModel and stores it in the cache */
    SkeletalMeshVisualInfo* LoadzCModelData( zCModel* model );
    SkeletalMeshVisualInfo* LoadzCModelData( oCNPC* npc );

    /** Returns lowest lod of zCModel polys */
    int GetLowestLODNumPolys_SkeletalMesh( zCModel* model );
    float3* GetLowestLODPoly_SkeletalMesh( zCModel* model, const int polyId, float3*& polyNormal );

    /** Prints a message to the screen for the given amount of time */
    void PrintMessageTimed( const INT2& position, const std::string& strMessage, float time = 3000.0f, DWORD color = 0xFFFFFFFF );

    /** Prints information about the mod to the screen for a couple of seconds */
    void PrintModInfo();

    /** Reset gothic render states so the engine will set them anew */
    void ResetRenderStates();

    void SetCanClearVobsByVisual( bool enabled = true ) {
        _canClearVobsByVisual = enabled;
    }

    /** Get sky timescale variable */
    float GetSkyTimeScale();
    
    static void ProcessVobAnimation( zCVob* vob, zTAnimationMode aniMode, VobInstanceInfo& vobInstance );
    static void UpdateShouldBlockGameInput();

private:
    struct WorldSectionBVHNode {
        DirectX::BoundingBox Bounds = {};
        uint32_t LeftChild = 0;
        uint32_t RightChild = 0;
        uint32_t LeafStart = 0;
        uint32_t LeafCount = 0;

        bool IsLeaf() const { return LeafCount > 0; }
    };

    void BuildWorldSectionBVH();
    void ClearWorldSectionBVH();
    void QueryWorldSectionBVH( const Frustum& frustum,
        std::vector<WorldMeshSectionInfo*>& sections,
        bool useSectionRadiusFilter,
        const HorizonCuller* horizon = nullptr ) const;
    bool UseWorldSectionBVH() const;

    /** One leaf primitive of the world-mesh CLUSTER BVH: either one WorldMeshInfo::Clusters[]
        entry, or - for a mesh too small to have been clustered at all (see
        WORLD_MESH_CLUSTER_MIN_TRIANGLES) - the whole mesh, marked by ClusterIndex ==
        WHOLE_MESH_CLUSTER. Built and queried alongside, but independently of, the section-level BVH
        above so CollectVisibleSections' existing callers are untouched by this. */
    struct WorldMeshClusterRef {
        static constexpr uint32_t WHOLE_MESH_CLUSTER = UINT32_MAX;

        DirectX::BoundingBox Bounds{};
        DirectX::XMFLOAT3 Center{};
        WorldMeshInfo* Mesh = nullptr;
        MeshKey Key{};
        uint32_t ClusterIndex = WHOLE_MESH_CLUSTER;
    };

    /** Gathers every WorldMeshInfo's clusters across every section into one global tree. Called from
        BuildWorldSectionBVH/ClearWorldSectionBVH so there's a single build/clear call site for both
        trees; independent storage/lifetime otherwise. */
    void BuildWorldMeshClusterBVH();

    /** Collects polygons in the given AABB */
    void CollectPolygonsInAABBRec( BspInfo* base, const zTBBox3D& bbox, std::vector<zCPolygon*>& list );

    /** Cleans empty BSPNodes */
    void CleanBSPNodes();

    /** Helper function for going through the bsp-tree */
    void BuildBspVobMapCacheHelper( zCBspBase* base );
    void BuildBspLeafLinearCache();

    /** Applys the suppressed textures */
    void ApplySuppressedSectionTextures();

    /** Puts the custom-polygons into the bsp-tree */
    void PutCustomPolygonsIntoBspTree();
    void PutCustomPolygonsIntoBspTreeRec( BspInfo* base );

    /** Hooked Window-Proc from the game */
    static LRESULT CALLBACK GothicWndProc( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam );

    /** Goes through the given zCTree and registeres all found vobs */
    void TraverseVobTree( zCTree<zCVob>* tree );

    /** Goes through the given zCTree and calls handler for each found vob */
    void TraverseVobTree( zCTree<zCVob>* tree , std::function<void( zCVob* )> handler);

    /** Saved Graphics state */
    GothicRendererState RendererState;

    /** Loaded world mitpoint */
    std::unique_ptr<WorldInfo> LoadedWorldInfo;

    /** Currently bound textures from gothic */
    zCTexture* BoundTextures[8];

    std::map<zCTexture*, std::vector<ParticleInstanceInfo>> FrameParticles;
    std::map<zCTexture*, ParticleRenderInfo> FrameParticleInfo;

    /** Loaded game sections */
    std::map<int, std::map<int, WorldMeshSectionInfo>> WorldSections;
    std::vector<WorldSectionBVHNode> WorldSectionBVHNodes;
    std::vector<WorldMeshSectionInfo*> WorldSectionBVHSections;
    bool WorldSectionBVHValid = false;
    SpatialBVH::BuildResult<WorldMeshClusterRef> WorldMeshClusterTree;
    MeshInfo* WrappedWorldMesh;

    /** List of vobs with skeletal meshes (Having a zCModel-Visual) */
    std::vector<SkeletalVobInfo*> SkeletalMeshVobs;
    std::vector<SkeletalVobInfo*> AnimatedSkeletalVobs;
    /** Non-skeletal mesh vobs added with GetFlags().StaticVob clear -- see GetDynamicMeshVobs(). */
    std::vector<VobInfo*> DynamicMeshVobs;
    std::vector<TransparencyVobInfo> TransparencyVobs;
    std::vector<SkeletalVobInfo*> VNSkeletalVobs;

    TransparencyQueue TransparencyQueueData;

    /** List of Vobs having a zCParticleFX-Visual */
    std::vector<zCVob*> ParticleEffectVobs;
    std::vector<zCVob*> DecalVobs;
    std::unordered_map<zCVob*, std::string> tempParticleNames;

    /** List of Meshes derived from a zCParticleFX-Visual */
    std::unordered_map<zCVob*, std::unique_ptr<MeshVisualInfo>> ParticleEffectProgMeshes;

    /** Poly strip Visuals */
    std::set<zCPolyStrip*> PolyStripVisuals;

    /** Flash Visuals */
    std::unordered_map<zCFlash*, zCVob*> FlashVisuals;
    std::vector<zCPolyStrip*> FrameThunderPolyStrips;

    /** Set of Materials */
    std::set<zCMaterial*> LoadedMaterials;

    /** List of meshes rendered for this frame */
    std::set<MeshVisualInfo*> FrameMeshInstances;

    /** Map for static mesh visuals */
    gtl::flat_hash_map<zCProgMeshProto*, MeshVisualInfo*> StaticMeshVisuals;

    /** Collection of poly strip infos (includes mesh and material data) */
    std::map<zCTexture*, PolyStripInfo> PolyStripInfos;

    /** Map for skeletal mesh visuals */
    gtl::flat_hash_map<std::string, SkeletalMeshVisualInfo*> SkeletalMeshVisuals;
    gtl::flat_hash_map<oCNPC*, SkeletalMeshVisualInfo*> SkeletalMeshNpcs;

    /** Bumped once per OnWorldUpdate. See GetFrameNumber(). */
    size_t FrameNumber = 0;

    /** Snapshot of GetFrameTimeSec() taken once at the end of the frame that just finished. See
        GetPreviousFrameTimeSec(). */
    float PreviousFrameTimeSec = 0.0f;

    /** Running total of PreviousFrameTimeSec, one frame-stable "current time". See GetStableTimeSec(). */
    float StableTimeSec = 0.0f;

    /** StableTimeSec's value as of the start of the frame that just finished. See GetPreviousStableTimeSec(). */
    float PreviousStableTimeSec = 0.0f;

    /** Looks up the extracted skeletal mesh data for a live zCModel, NPC-keyed first, then by
     *  visual name. Returns nullptr while no data exists or a background extraction is still
     *  running - callers must not substitute a placeholder position in that case, see
     *  GetLowestLODPoly_SkeletalMesh. */
    SkeletalMeshVisualInfo* ResolveSkeletalVisualInfo( zCModel* model );

    /** True if this emitter samples its spawn positions from a skeletal shape-mesh
     *  (zPFX_EMITTER_SHAPE_MESH + shpModel, set by oCVisualFX's emAdjustShpToOrigin) that we
     *  cannot serve yet. Ticking such an emitter would spawn every particle at the model's
     *  origin, so the caller must skip the update entirely. */
    bool IsUnservableSkeletalShapeEmitter( zCParticleFX* fx );

    /** Re-points a mesh-shaped emitter at its origin's model when ZENGIN's one-shot assignment
     *  missed it (origin had no visual yet). Cheap no-op once the shape is set. */
    void RepairShapeMeshEmitter( zCVob* source, zCParticleFX* fx );

    /** Set of all vobs we registered by now */
    gtl::flat_hash_set<zCVob*> RegisteredVobs;

    /** List of dynamically added vobs */
    std::vector<VobInfo*> DynamicallyAddedVobs;

    /** Map of vobs and VobIndfos */
    gtl::flat_hash_map<zCVob*, VobInfo*> VobMap;
public:
    // temporarily, to allow CollectVisibleVobsHelper to be templated for inlining optimizations
    gtl::flat_hash_map<zCVobLight*, VobLightInfo*> VobLightMap;
    // Exposed for CollectLeafVobs/CollectVisibleVobsWithLeafCache (file-static helpers)
    BspLeafLinearCache LeafLinearCache;
private:
    BspPortalCuller PortalCuller;
    HorizonCuller Horizon;

    /** One-shot guard for DebugDrawOccluders' line-budget message. */
    bool OccluderDebugBudgetLogged = false;

    gtl::flat_hash_map<zCVob*, SkeletalVobInfo*> SkeletalVobMap;

    /** Map of VobInfo-Lists for zCBspLeafs */
    std::unordered_map<zCBspBase*, BspInfo> BspLeafVobLists;

    /** Map for the material infos.
        Guarded because mesh extraction runs on worker threads (WorldConverter::ExtractNodeVisualAsync,
        GothicAPI::LoadzCModelData) and resolves MaterialInfos while the main thread keeps looking them
        up per draw. Values are unique_ptr so returned pointers stay valid across a rehash. */
    gtl::flat_hash_map<void*, std::unique_ptr<MaterialInfo>> MaterialInfos;
    std::shared_mutex MaterialInfosMutex;

    /** Maps visuals to vobs */
    gtl::flat_hash_map<zCVisual*, std::vector<BaseVobInfo*>> VobsByVisual;

    /** Map of textures */
    gtl::flat_hash_map<std::string, MyDirectDrawSurface7*> SurfacesByName;

    /** Directory we started in */
    std::string StartDirectory;

    /** Resource critical section */
    CRITICAL_SECTION ResourceCriticalSection;

    /** Sky renderer */
    std::unique_ptr<GSky> SkyRenderer;

    /** Inventory manager */
    std::unique_ptr<GInventory> Inventory;

    /** Saved Wnd-Proc pointer from the game */
    LONG_PTR OriginalGothicWndProc;

    /** Whether we test texture binds to figure out what surface uses which zCTexture object */
    bool TextureTestBindMode;
    std::string BoundTestTexture;

    /** Replacement values for the camera */
    CameraReplacement* CameraReplacementPtr;

    /** List of available GVegetationBoxes */
    std::list<GVegetationBox*> VegetationBoxes;

    /** Gothics output window */
    HWND OutputWindow;

    /** Suppressed textures for the sections */
    std::map<WorldMeshSectionInfo*, std::vector<std::string>> SuppressedTexturesBySection;

    /** Current camera, stored to find out about camera switches */
    zCCamera* CurrentCamera;

    /** The id of the main thread */
    DWORD MainThreadID;

    /** Textures loaded this frame */
    std::deque<DeferredMipUpload> FrameStagingTextures;
    std::deque<GfxTexture*> FrameMipMapGenerations;
    std::list<MyDirectDrawSurface7*> FrameLoadedTextures;

    /** Quad marks loaded in the world */
    std::unordered_map<zCQuadMark*, QuadMarkInfo> QuadMarks;

    /** Map of parameters from the .ini */
    std::map<std::string, int> ConfigIntValues;

    /** The overall wetness of the current scene */
    float SceneWetness;

    /** Internal list of futures, so they can run until they are finished */
    std::vector<std::future<void>> FutureList;

    bool _canRain;

    /** Used to only allow deterministic VOB cleanup (e.g. on loading a world.)*/
    bool _canClearVobsByVisual;
    bool m_DebugMode;

    std::string m_gameName;
};
