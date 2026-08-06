#pragma once
#include "WorldObjects.h"

#include <cstdint>
#include <span>
#include <vector>

class zCVob;
class zCQuadMark;
class zCTexture;
struct PolyStripInfo;

/** Backend-neutral collection point for everything that gets drawn alpha-blended.

    Before this existed every category of transparent geometry (world mesh sections, instanced
    alpha VOBs, ghosts, decals, quad marks, poly strips) had its own render pass in a hard-coded
    sequence, and each of them sorted - at best - only within itself. A cobweb therefore always
    painted before a ghost and a ghost always before a glass window, no matter what stood in front
    of what.

    Producers now push their drawables in here during the frame; the transparency pass sorts the
    whole set strictly back-to-front once and replays it. Batching survives because the emitters
    draw maximal *consecutive* runs of the same kind in one go (see ForEachRun) - the same
    painter's-order rule the decal path has always followed.

    Only the small Items array is sorted; the payload vectors never move. Every vector is cleared
    (never freed) per frame and reserved once, per the project's no-per-frame-allocation rule. */

enum class ETransparentKind : uint8_t {
    WorldMesh,      // world mesh section, SubKind = EWorldTransparencyVariant
    AlphaVob,       // one instance of an instanced alpha-blended VOB batch
    Ghost,          // fading/ghosted vob, index into GothicAPI::TransparencyVobs
    Decal,          // unlit (blended) decal vob
    QuadMark,       // blood / spell ground marks, all blend modes
    PolyStrip,      // weapon trails, barrier lightning
    Count
};

/** World transparency meshes all run through the same draw code - the pixel shader is picked from
    MaterialInfo::MaterialType - but portals are gated behind a setting and are excluded from the
    depth-only re-draw, so the variant has to survive the sort. */
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

/** Backend-neutral reference into the backend's own alpha-VOB batch array (D3D11: m_AlphaMeshes,
    D3D12: its equivalent). Instances of one batch are contiguous in the instancing buffer, so
    consecutive items of the same batch re-merge into a single instanced draw at emit time. */
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

    /** For a backend that keeps a kind's data in its own arrays (D3D12 resolves every buffer view and
        bindless index at build time) the queue only has to carry two indices into those. Read back
        with GetIndices; the typed getters above do not apply to items added this way. */
    void AddIndexed( float distanceSq, ETransparentKind kind, uint8_t subKind,
        uint32_t index0, uint32_t index1, uint32_t batchKey );

    void AddGhost( float distanceSq, uint32_t transparencyVobIndex );
    void AddDecal( float distanceSq, zCVob* vob );
    void AddQuadMark( float distanceSq, zCQuadMark* mark, const QuadMarkInfo* info );
    void AddPolyStrip( float distanceSq, zCTexture* texture, const PolyStripInfo* info );

    /** Back-to-front by distance. With categoryMajor the kinds are kept apart and drawn in the
        legacy pass order instead - the fallback the SortedTransparency setting selects, which
        needs no second copy of the emitters. */
    void Sort( bool categoryMajor );

    bool Empty() const { return Items.empty(); }
    size_t Size() const { return Items.size(); }
    std::span<const TransparentItem> GetItems() const { return Items; }

    /** Calls emit(kind, subKind, span) for every maximal run of consecutive same-kind items.
        Returns the number of runs - a run count approaching Size() means batching collapsed. */
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
