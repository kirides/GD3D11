#pragma once
#include "GothicAPI.h"
#include "BspPortalCuller.h"
#include "SpatialBVH.h"
#include "TransparencyQueue.h"
#include <shared_mutex>
#include "WorldMeshSection.h"
#include "ShoreField.h"

/** GothicAPI's container members. Kept out of GothicAPI.h so only the TUs that touch them pay for
    instantiating these maps; include this only where the containers themselves are needed. */
struct GothicAPIState {
    TransparencyQueue TransparencyQueueData;
    BspPortalCuller PortalCuller;
    SpatialBVH::BuildResult<GothicAPI::WorldMeshClusterRef> WorldMeshClusterTree;

    std::map<zCTexture*, std::vector<ParticleInstanceInfo>> FrameParticles;
    std::map<zCTexture*, ParticleRenderInfo> FrameParticleInfo;

    /** Loaded game sections */
    std::map<int, std::map<int, WorldMeshSectionInfo>> WorldSections;

    /** Shoreline distance/depth field of the loaded world; null without one or before it is wanted. */
    std::unique_ptr<ShoreField> ShoreFieldData;
    bool ShoreFieldBaked = false;   // tried for this world, so a failed bake isn't retried every frame

    /** Static vobs with a WaterBob, updated once per frame; non-owning, aliases VobMap. */
    std::vector<VobInfo*> FloatingVobs;

    std::unordered_map<zCVob*, std::string> tempParticleNames;

    /** List of Meshes derived from a zCParticleFX-Visual */
    std::unordered_map<zCVob*, std::unique_ptr<MeshVisualInfo>> ParticleEffectProgMeshes;

    /** Poly strip Visuals */
    std::set<zCPolyStrip*> PolyStripVisuals;

    /** Flash Visuals */
    std::unordered_map<zCFlash*, zCVob*> FlashVisuals;

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

    /** Set of all vobs we registered by now */
    gtl::flat_hash_set<zCVob*> RegisteredVobs;

    /** Map of vobs and VobIndfos */
    gtl::flat_hash_map<zCVob*, VobInfo*> VobMap;

    gtl::flat_hash_map<zCVobLight*, VobLightInfo*> VobLightMap;

    gtl::flat_hash_map<zCVob*, SkeletalVobInfo*> SkeletalVobMap;

    /** Map of VobInfo-Lists for zCBspLeafs */
    std::unordered_map<zCBspBase*, BspInfo> BspLeafVobLists;

    /** Guarded: worker-thread mesh extraction resolves MaterialInfos while the main thread looks them up.
        Values are unique_ptr so returned pointers stay valid across a rehash. */
    gtl::flat_hash_map<void*, std::unique_ptr<MaterialInfo>> MaterialInfos;
    std::shared_mutex MaterialInfosMutex;

    /** Maps visuals to vobs */
    gtl::flat_hash_map<zCVisual*, std::vector<BaseVobInfo*>> VobsByVisual;

    /** Map of textures */
    gtl::flat_hash_map<std::string, MyDirectDrawSurface7*> SurfacesByName;

    /** List of available GVegetationBoxes */
    std::list<GVegetationBox*> VegetationBoxes;

    /** Suppressed textures for the sections */
    std::map<WorldMeshSectionInfo*, std::vector<std::string>> SuppressedTexturesBySection;

    /** Textures loaded this frame */
    std::deque<DeferredMipUpload> FrameStagingTextures;
    std::deque<GfxTexture*> FrameMipMapGenerations;
    std::list<MyDirectDrawSurface7*> FrameLoadedTextures;

    /** Quad marks loaded in the world */
    std::unordered_map<zCQuadMark*, QuadMarkInfo> QuadMarks;

    /** Map of parameters from the .ini */
    std::map<std::string, int> ConfigIntValues;
};
