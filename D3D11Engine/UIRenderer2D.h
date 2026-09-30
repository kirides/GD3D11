#pragma once
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>
#include <DirectXMath.h>
#include "UIRenderer2DTypes.h"

class BaseGraphicsEngine;
class GfxTexture;
class zCTexture;
class zFont;
struct zTRndSimpleVertex;
struct MeshInfo;
struct SkeletalMeshInfo;

/** Backend-neutral recorder for Gothic's 2D UI. Primitives are CPU-clipped to the current D3D7 viewport and
    merged into batches, keeping painter's order between overlapping primitives. */
class UIRenderer2D {
public:
    explicit UIRenderer2D( BaseGraphicsEngine& engine ) : m_Engine( engine ) {}

    /** zCRnd_D3D::DrawPolySimple: a convex fan in UI pixels. */
    void AddPolygon( zCTexture* texture, const zTRndSimpleVertex* vertices, int numVertices, const UIPolygonState& state );
    /** zCRnd_D3D::DrawLine: an opaque one-pixel line. */
    void AddLine( float x1, float y1, float x2, float y2, uint32_t color );
    /** Custom font rendering (zCView::PrintChars hook). */
    void AddGlyphRun( std::string_view str, float x, float y, const zFont* font, uint32_t color );

    /** Item preview in a slot rect (UI pixels): Begin, add instances/bones/draws, End. Empty previews record nothing. */
    void BeginItemPreview( float x, float y, float width, float height );
    uint32_t AddItemInstance( const DirectX::XMFLOAT4X4& clipFromObject );
    uint32_t AddItemBones( std::span<const DirectX::XMFLOAT4X4> bones );
    void AddItemDraw( const UIItemDraw& draw );
    void EndItemPreview();

    /** Current D3D7 viewport in UI pixels; a degenerate rect disables clipping. */
    void SetClipRect( float x, float y, float width, float height );

    void Flush();
    void Discard();
    bool HasPending() const { return !m_Batches.empty(); }

    /** Called before the first primitive after a flush, so a pending fixed-function batch draws first. */
    void SetBeforeFirstRecord( void (*callback)(void*), void* context ) {
        m_BeforeFirstRecord = callback;
        m_BeforeFirstRecordContext = context;
    }

    /** Glyph scale of the custom font path: swim-bar width / 180 times Union's font multiplier. */
    static float ComputeFontScale( const BaseGraphicsEngine& engine );

private:
    struct ClipVertex {
        float X, Y, U, V;
        float C[4];
    };

    struct Primitive {
        uint32_t Batch;
        uint32_t First;
        uint32_t Count;
    };

    struct Rect {
        float MinX, MinY, MaxX, MaxY;
    };

    /** Conservative cover of one batch's primitives: a few rects, each grown while that adds little empty area. */
    struct Region {
        static constexpr int kMaxRects = 16;
        Rect Rects[kMaxRects];
        int Count = 0;

        bool Overlaps( const Rect& r ) const;
        void Add( const Rect& r );
    };

    void EmitPolygon( ClipVertex* poly, int count, GfxTexture* texture, EUIBlend2D blend, uint32_t params );
    void Append( GfxTexture* texture, EUIBlend2D blend, const UIVertex2D* vertices, uint32_t count,
        float minX, float minY, float maxX, float maxY );
    /** Joins the oldest same-key batch nothing drawn after it overlaps, else opens a new one. */
    uint32_t PlaceInBatch( GfxTexture* texture, EUIBlend2D blend, bool items, const Rect& rect );

    BaseGraphicsEngine& m_Engine;

    std::vector<UIVertex2D> m_Vertices;    // record order
    std::vector<UIVertex2D> m_Upload;      // batch order, only when a merge went out of order
    std::vector<Primitive> m_Primitives;
    std::vector<UIBatch2D> m_Batches;
    std::vector<Region> m_Regions;         // parallel to m_Batches
    bool m_InOrder = true;
    bool m_Flushing = false;

    std::vector<UIItemPreview> m_Items;        // record order
    std::vector<UIItemPreview> m_ItemUpload;   // batch order, only when a merge went out of order
    std::vector<Primitive> m_ItemPrimitives;
    std::vector<UIItemDraw> m_ItemDraws;
    std::vector<UIItemInstance> m_ItemInstances;
    std::vector<DirectX::XMFLOAT4X4> m_ItemBones;
    UIItemPreview m_OpenItem;
    uint32_t m_OpenItemInstances = 0;
    uint32_t m_OpenItemBones = 0;
    bool m_ItemOpen = false;
    bool m_ItemsInOrder = true;

    float m_ClipMinX = -1e7f, m_ClipMinY = -1e7f, m_ClipMaxX = 1e7f, m_ClipMaxY = 1e7f;

    void (*m_BeforeFirstRecord)(void*) = nullptr;
    void* m_BeforeFirstRecordContext = nullptr;
};
