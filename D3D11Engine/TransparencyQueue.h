#pragma once
#include "WorldObjects.h"

#include <cstdint>
#include <span>
#include <vector>

class zCVob;
class zCQuadMark;
class zCTexture;
struct PolyStripInfo;

/** Backend-neutral collection point for everything drawn alpha-blended. Producers push during the
    frame; the transparency pass sorts back-to-front once and replays it, batching maximal
    consecutive same-kind runs (ForEachRun). Only Items is sorted - payloads never move, and every
    vector is cleared (never freed) per frame. */

enum class ETransparentKind : uint8_t {
    WorldMesh,      // world mesh section, SubKind = EWorldTransparencyVariant
    AlphaVob,       // one instance of an instanced alpha-blended VOB batch
    Ghost,          // fading/ghosted vob, index into GothicAPI::TransparencyVobs
    Decal,          // unlit (blended) decal vob
    QuadMark,       // blood / spell ground marks, all blend modes
    PolyStrip,      // weapon trails, barrier lightning
    Count
};

/** One draw path for all three; the variant only gates portals behind DrawG1ForestPortals and
    excludes them from the depth re-lay. The pixel shader comes from MaterialInfo::MaterialType. */
enum class EWorldTransparencyVariant : uint8_t {
    Normal,
    Portal,
    Waterfall
};

struct TransparentItem {
    float DistanceSq;               // camera -> representative world-space center
    uint32_t BatchKey;              // material/texture derived, tie-break only: keeps batching intact
    uint32_t PayloadIndex;          // index into the payload vector of this Kind
    ETransparentKind Kind;
    uint8_t SubKind;
};

struct TransparentWorldMesh {
    MeshKey Key;
    MeshInfo* Mesh;
};

/** Index into the backend's own alpha-VOB batch array. A batch's instances are contiguous in the
    instancing buffer, so consecutive items re-merge into one instanced draw at emit time. */
struct TransparentAlphaVob {
    uint32_t BatchIndex;
    uint32_t InstanceIndex;
};

struct TransparentQuadMark {
    zCQuadMark* Mark;
    const QuadMarkInfo* Info;
};

struct TransparentPolyStrip {
    zCTexture* Texture;
    const PolyStripInfo* Info;
};

class TransparencyQueue {
public:
    /** Drops last frame's contents, keeping all capacity. */
    void BeginFrame();

    void AddWorldMesh( float distanceSq, EWorldTransparencyVariant variant, const MeshKey& key, MeshInfo* mesh );
    void AddAlphaVob( float distanceSq, uint32_t batchIndex, uint32_t instanceIndex, uint32_t batchKey );

    /** For backends keeping a kind's data in their own arrays (D3D12). Read back with GetIndices;
        the typed getters do not apply to items added this way. */
    void AddIndexed( float distanceSq, ETransparentKind kind, uint8_t subKind,
        uint32_t index0, uint32_t index1, uint32_t batchKey );

    void AddGhost( float distanceSq, uint32_t transparencyVobIndex );
    void AddDecal( float distanceSq, zCVob* vob );
    void AddQuadMark( float distanceSq, zCQuadMark* mark, const QuadMarkInfo* info );
    void AddPolyStrip( float distanceSq, zCTexture* texture, const PolyStripInfo* info );

    /** Back-to-front by distance. categoryMajor instead keeps the kinds apart in the legacy pass
        order - the SortedTransparency=0 fallback, which needs no second copy of the emitters. */
    void Sort( bool categoryMajor );

    bool Empty() const { return Items.empty(); }
    size_t Size() const { return Items.size(); }
    std::span<const TransparentItem> GetItems() const { return Items; }

    /** emit(kind, subKind, span) per maximal same-kind run. Returns the run count; approaching
        Size() means batching collapsed. */
    template<class F> size_t ForEachRun( F&& emit ) const {
        size_t runs = 0;
        for ( size_t i = 0; i < Items.size(); ) {
            const ETransparentKind kind = Items[i].Kind;
            const uint8_t subKind = Items[i].SubKind;
            size_t end = i + 1;
            while ( end < Items.size() && Items[end].Kind == kind && Items[end].SubKind == subKind ) {
                ++end;
            }
            emit( kind, subKind, std::span<const TransparentItem>( Items.data() + i, end - i ) );
            ++runs;
            i = end;
        }
        return runs;
    }

    const TransparentWorldMesh& GetWorldMesh( const TransparentItem& item ) const { return WorldMeshes[item.PayloadIndex]; }
    const TransparentAlphaVob& GetAlphaVob( const TransparentItem& item ) const { return AlphaVobs[item.PayloadIndex]; }
    const TransparentAlphaVob& GetIndices( const TransparentItem& item ) const { return AlphaVobs[item.PayloadIndex]; }
    uint32_t GetGhostIndex( const TransparentItem& item ) const { return GhostIndices[item.PayloadIndex]; }
    zCVob* GetDecal( const TransparentItem& item ) const { return Decals[item.PayloadIndex]; }
    const TransparentQuadMark& GetQuadMark( const TransparentItem& item ) const { return QuadMarks[item.PayloadIndex]; }
    const TransparentPolyStrip& GetPolyStrip( const TransparentItem& item ) const { return PolyStrips[item.PayloadIndex]; }

    /** Squared distance from the camera to a point, the depth every producer sorts on. */
    static float DistanceSqFromCamera( const XMFLOAT3& worldPosition );

    /** Pointer-derived tie-break key. The game is 32-bit, so a pointer fits without folding. */
    static uint32_t MakeBatchKey( const void* p ) { return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p)); }

private:
    std::vector<TransparentItem> Items;

    std::vector<TransparentWorldMesh> WorldMeshes;
    std::vector<TransparentAlphaVob> AlphaVobs;
    std::vector<uint32_t> GhostIndices;
    std::vector<zCVob*> Decals;
    std::vector<TransparentQuadMark> QuadMarks;
    std::vector<TransparentPolyStrip> PolyStrips;
};
