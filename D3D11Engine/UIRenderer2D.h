#pragma once
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

class BaseGraphicsEngine;
class GfxTexture;
class zCTexture;
class zFont;
struct zTRndSimpleVertex;

/** One 2D UI vertex in Gothic UI pixels (before GothicUIScale). Layout shared by VS_UI2D.hlsl and D3D12/UI2D.hlsl. */
struct UIVertex2D {
    float X, Y;
    float U, V;
    uint32_t Color;    // Gothic BGRA
    uint32_t Params;   // UIVertexParams bits
};

namespace UIVertexParams {
    constexpr uint32_t TextureIndexMask = 0x000FFFFF;   // bindless SRV slot (D3D12 only)
    constexpr uint32_t Linear = 1u << 20;
    constexpr uint32_t Wrap = 1u << 21;
    constexpr uint32_t ModeShift = 22;
    constexpr uint32_t ModeBlend = 0u << ModeShift;     // (rgb*a, a)
    constexpr uint32_t ModeAdd = 1u << ModeShift;       // (rgb*a, 0)
    constexpr uint32_t ModeOpaque = 2u << ModeShift;    // (rgb, 1)
    constexpr uint32_t ModeTexOnly = 3u << ModeShift;   // texture rgb only, for the MUL blends
    constexpr uint32_t IgnoreTexAlpha = 1u << 24;       // zRND_ALPHA_SOURCE_CONSTANT: alpha from the vertex only
    constexpr uint32_t Untextured = 1u << 25;
}

/** Blend state of a batch. BLEND, ADD and opaque all share the premultiplied state. */
enum class EUIBlend2D : uint8_t {
    Premultiplied,  // ONE / INV_SRC_ALPHA
    Mul,            // DEST_COLOR / ZERO
    Mul2,           // DEST_COLOR / SRC_COLOR
};

struct UIBatch2D {
    GfxTexture* Texture = nullptr;   // bound per batch when the backend has no bindless index
    uint32_t FirstVertex = 0;
    uint32_t VertexCount = 0;
    EUIBlend2D Blend = EUIBlend2D::Premultiplied;
    float MinX = 0, MinY = 0, MaxX = 0, MaxY = 0;
};

/** The zCRnd_D3D::xd3d_actStatus fields zCRnd_D3D::DrawPolySimple reads. */
struct UIPolygonState {
    int AlphaFunc = 0;               // zTRnd_AlphaBlendFunc
    bool AlphaSourceConstant = false;
    float AlphaFactor = 1.0f;
    bool Bilinear = true;
};

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

    void EmitPolygon( ClipVertex* poly, int count, GfxTexture* texture, EUIBlend2D blend, uint32_t params );
    void Append( GfxTexture* texture, EUIBlend2D blend, const UIVertex2D* vertices, uint32_t count,
        float minX, float minY, float maxX, float maxY );

    BaseGraphicsEngine& m_Engine;

    std::vector<UIVertex2D> m_Vertices;    // record order
    std::vector<UIVertex2D> m_Upload;      // batch order, only when a merge went out of order
    std::vector<Primitive> m_Primitives;
    std::vector<UIBatch2D> m_Batches;
    bool m_InOrder = true;
    bool m_Flushing = false;

    float m_ClipMinX = -1e7f, m_ClipMinY = -1e7f, m_ClipMaxX = 1e7f, m_ClipMaxY = 1e7f;

    void (*m_BeforeFirstRecord)(void*) = nullptr;
    void* m_BeforeFirstRecordContext = nullptr;
};
