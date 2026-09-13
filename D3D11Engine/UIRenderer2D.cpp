#include "pch.h"
#include "UIRenderer2D.h"
#include "BaseGraphicsEngine.h"
#include "Engine.h"
#include "GothicAPI.h"
#include "zTypes.h"
#include "zFont.h"
#include "zCTexture.h"
#include "zCView.h"
#include "oCGame.h"
#include "D3D7/MyDirectDrawSurface7.h"

namespace {
    // Flushed early past this, which keeps order; well under D3D12's 16 MiB per-frame UI ring.
    constexpr size_t kMaxPendingVertices = 256 * 1024;
    constexpr int kMergeLookback = 16;
    constexpr int kMaxPolygonVertices = 32;
    // Sutherland-Hodgman against a rectangle adds at most one vertex per plane.
    constexpr int kMaxClippedVertices = kMaxPolygonVertices + 4;

    uint32_t PackColor( const float c[4] ) {
        auto channel = []( float v ) { return static_cast<uint32_t>( std::clamp( v + 0.5f, 0.0f, 255.0f ) ); };
        return channel( c[0] ) | (channel( c[1] ) << 8) | (channel( c[2] ) << 16) | (channel( c[3] ) << 24);
    }

    void UnpackColor( uint32_t color, float c[4] ) {
        c[0] = static_cast<float>( color & 0xFF );
        c[1] = static_cast<float>( (color >> 8) & 0xFF );
        c[2] = static_cast<float>( (color >> 16) & 0xFF );
        c[3] = static_cast<float>( (color >> 24) & 0xFF );
    }
}

float UIRenderer2D::ComputeFontScale( const BaseGraphicsEngine& engine ) {
    float scale = 1.0f;
    static int savedBarSize = -1;
    if ( auto game = oCGame::GetGame(); game && game->swimBar ) {
        if ( savedBarSize == -1 ) {
            savedBarSize = game->swimBar->psizex;
        }
        scale = static_cast<float>( savedBarSize ) / 180.f;
    }
    return scale * engine.GetCustomFontMultiplier();
}

void UIRenderer2D::SetClipRect( float x, float y, float width, float height ) {
    if ( width < 2.0f || height < 2.0f ) {
        m_ClipMinX = m_ClipMinY = -1e7f;
        m_ClipMaxX = m_ClipMaxY = 1e7f;
        return;
    }
    m_ClipMinX = x;
    m_ClipMinY = y;
    m_ClipMaxX = x + width;
    m_ClipMaxY = y + height;
}

void UIRenderer2D::AddPolygon( zCTexture* texture, const zTRndSimpleVertex* vertices, int numVertices, const UIPolygonState& state ) {
    if ( !texture || !vertices || numVertices < 3 ) return;
    if ( numVertices > kMaxPolygonVertices ) {
        static bool logged = false;
        if ( !logged ) {
            logged = true;
            LogWarn() << "UIRenderer2D: DrawPolySimple polygon with " << numVertices << " vertices dropped (cap " << kMaxPolygonVertices << ").";
        }
        return;
    }

    if ( texture->CacheIn( -1 ) != zRES_CACHED_IN ) return;
    MyDirectDrawSurface7* surface = texture->GetSurface();
    GfxTexture* gfx = surface ? surface->GetEngineTexture() : nullptr;
    if ( !gfx ) return;

    // Same alpha-func table as zCRnd_D3D::DrawPolySimple.
    uint32_t params = 0;
    EUIBlend2D blend = EUIBlend2D::Premultiplied;
    bool vertexAlpha = true;
    switch ( state.AlphaFunc ) {
    case zRND_ALPHA_FUNC_BLEND:
    case zRND_ALPHA_FUNC_SUB:
        params = UIVertexParams::ModeBlend | (state.AlphaSourceConstant ? UIVertexParams::IgnoreTexAlpha : 0);
        break;
    case zRND_ALPHA_FUNC_ADD:
        params = UIVertexParams::ModeAdd | (state.AlphaSourceConstant ? UIVertexParams::IgnoreTexAlpha : 0);
        break;
    case zRND_ALPHA_FUNC_MUL:
        params = UIVertexParams::ModeTexOnly;
        blend = EUIBlend2D::Mul;
        vertexAlpha = false;
        break;
    case zRND_ALPHA_FUNC_MUL2:
        params = UIVertexParams::ModeTexOnly;
        blend = EUIBlend2D::Mul2;
        vertexAlpha = false;
        break;
    default:
        if ( texture->HasAlphaChannel() ) {
            params = UIVertexParams::ModeBlend;
        } else {
            params = UIVertexParams::ModeOpaque;
            vertexAlpha = false;
        }
        break;
    }

    ClipVertex poly[kMaxClippedVertices];
    bool anyVisible = !vertexAlpha;
    bool outsideUnitRange = false;
    for ( int i = 0; i < numVertices; ++i ) {
        const zTRndSimpleVertex& in = vertices[i];
        ClipVertex& out = poly[i];
        out.X = in.pos.x;
        out.Y = in.pos.y;
        out.U = in.uv.x;
        out.V = in.uv.y;
        UnpackColor( in.color.dword, out.C );

        float alpha = 0.0f;
        if ( vertexAlpha ) {
            alpha = out.C[3];
            if ( state.AlphaSourceConstant ) alpha = std::floor( state.AlphaFactor * alpha );
            anyVisible |= alpha > 0.0f;
        }
        out.C[3] = alpha;

        constexpr float eps = 1e-3f;
        outsideUnitRange |= in.uv.x < -eps || in.uv.x > 1.0f + eps || in.uv.y < -eps || in.uv.y > 1.0f + eps;
    }
    // Constant-alpha-zero tiles (FillZBuffer, indoor sky background) only ever wrote depth nobody reads.
    if ( !anyVisible ) return;

    if ( state.Bilinear ) params |= UIVertexParams::Linear;
    // Plain UI quads clamp so linear filtering doesn't bleed the opposite edge in; scrolled UVs wrap.
    if ( outsideUnitRange ) params |= UIVertexParams::Wrap;

    EmitPolygon( poly, numVertices, gfx, blend, params );
}

void UIRenderer2D::AddLine( float x1, float y1, float x2, float y2, uint32_t color ) {
    const float scale = std::max( 0.1f, Engine::GAPI->GetRendererState().RendererSettings.GothicUIScale );
    const float half = 0.5f / scale;

    float dx = x2 - x1, dy = y2 - y1;
    const float length = std::sqrt( dx * dx + dy * dy );
    if ( length > 1e-4f ) {
        dx /= length;
        dy /= length;
    } else {
        dx = 1.0f;
        dy = 0.0f;
    }

    // One physical pixel wide, centered on the pixel centers the endpoints name.
    const float ax = x1 + half - dx * half, ay = y1 + half - dy * half;
    const float bx = x2 + half + dx * half, by = y2 + half + dy * half;
    const float nx = -dy * half, ny = dx * half;

    ClipVertex poly[kMaxClippedVertices];
    const float corners[4][2] = { { ax + nx, ay + ny }, { bx + nx, by + ny }, { bx - nx, by - ny }, { ax - nx, ay - ny } };
    for ( int i = 0; i < 4; ++i ) {
        poly[i].X = corners[i][0];
        poly[i].Y = corners[i][1];
        poly[i].U = 0.0f;
        poly[i].V = 0.0f;
        UnpackColor( color, poly[i].C );
    }

    EmitPolygon( poly, 4, nullptr, EUIBlend2D::Premultiplied, UIVertexParams::ModeOpaque | UIVertexParams::Untextured );
}

void UIRenderer2D::AddGlyphRun( std::string_view str, float x, float y, const zFont* font, uint32_t color ) {
    if ( !font || !font->tex ) return;

    // Trailing '/' are Gothic control markers.
    size_t length = str.size();
    while ( length > 0 && str[length - 1] == '/' ) --length;
    if ( !length ) return;
    if ( (color >> 24) == 0 ) return;

    zCTexture* texture = font->tex;
    if ( texture->CacheIn( -1 ) != zRES_CACHED_IN ) return;
    MyDirectDrawSurface7* surface = texture->GetSurface();
    GfxTexture* gfx = surface ? surface->GetEngineTexture() : nullptr;
    if ( !gfx ) return;

    const float scale = ComputeFontScale( m_Engine );
    const float spacing = 1.0f * scale;
    const float height = static_cast<float>( font->height ) * scale;
    const uint32_t params = UIVertexParams::ModeBlend | UIVertexParams::Linear;

    float xpos = x, ypos = y;
    ClipVertex quad[kMaxClippedVertices];
    for ( size_t i = 0; i < length; ++i ) {
        const unsigned char c = static_cast<unsigned char>( str[i] );
        const float width = static_cast<float>( font->width[c] ) * scale;
        const float minX = xpos, minY = ypos;

        // Same advance as UI::zFont::AppendGlyphs.
        if ( c == '\n' ) {
            ypos += height;
            xpos = x;
        } else if ( c == ' ' ) {
            xpos += width;
            continue;
        } else {
            xpos += width + spacing;
        }
        if ( width <= 0.0f ) continue;

        const zVEC2& uv1 = font->fontuv1[c];
        const zVEC2& uv2 = font->fontuv2[c];
        const float corners[4][4] = {
            { minX,         minY,          uv1.pos.x, uv1.pos.y },
            { minX + width, minY,          uv2.pos.x, uv1.pos.y },
            { minX + width, minY + height, uv2.pos.x, uv2.pos.y },
            { minX,         minY + height, uv1.pos.x, uv2.pos.y } };
        for ( int v = 0; v < 4; ++v ) {
            quad[v].X = corners[v][0];
            quad[v].Y = corners[v][1];
            quad[v].U = corners[v][2];
            quad[v].V = corners[v][3];
            UnpackColor( color, quad[v].C );
        }
        EmitPolygon( quad, 4, gfx, EUIBlend2D::Premultiplied, params );
    }
}

void UIRenderer2D::EmitPolygon( ClipVertex* poly, int count, GfxTexture* texture, EUIBlend2D blend, uint32_t params ) {
    ClipVertex scratch[kMaxClippedVertices];
    ClipVertex* src = poly;
    ClipVertex* dst = scratch;

    // Sutherland-Hodgman against the clip rect; axis 0 = X, 1 = Y, sign picks min/max edge.
    const float bounds[4] = { m_ClipMinX, m_ClipMaxX, m_ClipMinY, m_ClipMaxY };
    for ( int plane = 0; plane < 4 && count >= 3; ++plane ) {
        const bool isY = plane >= 2;
        const bool isMax = (plane & 1) != 0;
        const float edge = bounds[plane];
        auto distance = [&]( const ClipVertex& v ) {
            const float p = isY ? v.Y : v.X;
            return isMax ? edge - p : p - edge;
        };

        int outCount = 0;
        for ( int i = 0; i < count; ++i ) {
            const ClipVertex& a = src[i];
            const ClipVertex& b = src[(i + 1) % count];
            const float da = distance( a ), db = distance( b );
            if ( da >= 0.0f ) dst[outCount++] = a;
            if ( (da >= 0.0f) != (db >= 0.0f) ) {
                const float t = da / (da - db);
                ClipVertex& o = dst[outCount++];
                o.X = a.X + (b.X - a.X) * t;
                o.Y = a.Y + (b.Y - a.Y) * t;
                o.U = a.U + (b.U - a.U) * t;
                o.V = a.V + (b.V - a.V) * t;
                for ( int c = 0; c < 4; ++c ) o.C[c] = a.C[c] + (b.C[c] - a.C[c]) * t;
            }
        }
        std::swap( src, dst );
        count = outCount;
    }
    if ( count < 3 ) return;

    const UINT textureIndex = m_Engine.GetUITextureIndex( texture );
    if ( textureIndex != UINT_MAX ) {
        params |= textureIndex & UIVertexParams::TextureIndexMask;
    }

    UIVertex2D triangles[(kMaxClippedVertices - 2) * 3];
    float minX = FLT_MAX, minY = FLT_MAX, maxX = -FLT_MAX, maxY = -FLT_MAX;
    UIVertex2D fan[kMaxClippedVertices];
    for ( int i = 0; i < count; ++i ) {
        const ClipVertex& v = src[i];
        fan[i] = { v.X, v.Y, v.U, v.V, PackColor( v.C ), params };
        minX = std::min( minX, v.X );
        minY = std::min( minY, v.Y );
        maxX = std::max( maxX, v.X );
        maxY = std::max( maxY, v.Y );
    }
    uint32_t emitted = 0;
    for ( int i = 1; i + 1 < count; ++i ) {
        triangles[emitted++] = fan[0];
        triangles[emitted++] = fan[i];
        triangles[emitted++] = fan[i + 1];
    }

    // Bindless backends leave the texture out of the batch key.
    Append( textureIndex != UINT_MAX ? nullptr : texture, blend, triangles, emitted, minX, minY, maxX, maxY );
}

void UIRenderer2D::Append( GfxTexture* texture, EUIBlend2D blend, const UIVertex2D* vertices, uint32_t count,
    float minX, float minY, float maxX, float maxY ) {
    if ( m_Batches.empty() && m_BeforeFirstRecord ) {
        m_BeforeFirstRecord( m_BeforeFirstRecordContext );
    }
    if ( m_Vertices.size() + count > kMaxPendingVertices ) {
        Flush();
    }

    // Join the newest same-key batch that nothing drawn after it overlaps; disjoint rects commute.
    int target = -1;
    const int newest = static_cast<int>( m_Batches.size() ) - 1;
    for ( int i = newest; i >= 0 && i > newest - kMergeLookback; --i ) {
        const UIBatch2D& b = m_Batches[i];
        if ( b.Texture == texture && b.Blend == blend ) {
            target = i;
            break;
        }
        if ( minX < b.MaxX && maxX > b.MinX && minY < b.MaxY && maxY > b.MinY ) {
            break;
        }
    }

    if ( target < 0 ) {
        UIBatch2D batch;
        batch.Texture = texture;
        batch.Blend = blend;
        batch.MinX = minX;
        batch.MinY = minY;
        batch.MaxX = maxX;
        batch.MaxY = maxY;
        m_Batches.push_back( batch );
        target = static_cast<int>( m_Batches.size() ) - 1;
    } else {
        UIBatch2D& b = m_Batches[target];
        b.MinX = std::min( b.MinX, minX );
        b.MinY = std::min( b.MinY, minY );
        b.MaxX = std::max( b.MaxX, maxX );
        b.MaxY = std::max( b.MaxY, maxY );
        if ( target != newest ) m_InOrder = false;
    }

    const uint32_t first = static_cast<uint32_t>( m_Vertices.size() );
    m_Vertices.insert( m_Vertices.end(), vertices, vertices + count );
    m_Batches[target].VertexCount += count;

    if ( !m_Primitives.empty() && m_Primitives.back().Batch == static_cast<uint32_t>( target ) ) {
        m_Primitives.back().Count += count;
    } else {
        m_Primitives.push_back( { static_cast<uint32_t>( target ), first, count } );
    }
}

void UIRenderer2D::Flush() {
    if ( m_Batches.empty() || m_Flushing ) return;
    ZoneScoped;
    m_Flushing = true;

    uint32_t offset = 0;
    for ( UIBatch2D& b : m_Batches ) {
        b.FirstVertex = offset;
        offset += b.VertexCount;
    }

    std::span<const UIVertex2D> stream = m_Vertices;
    if ( !m_InOrder ) {
        // Gather each batch's primitives behind it; batches keep their creation order.
        m_Upload.resize( offset );
        for ( UIBatch2D& b : m_Batches ) b.VertexCount = 0;
        for ( const Primitive& p : m_Primitives ) {
            UIBatch2D& b = m_Batches[p.Batch];
            memcpy( &m_Upload[b.FirstVertex + b.VertexCount], &m_Vertices[p.First], p.Count * sizeof( UIVertex2D ) );
            b.VertexCount += p.Count;
        }
        stream = m_Upload;
    }

    m_Engine.DrawUI2D( stream, m_Batches );

    m_Flushing = false;
    Discard();
}

void UIRenderer2D::Discard() {
    m_Vertices.clear();
    m_Upload.clear();
    m_Primitives.clear();
    m_Batches.clear();
    m_InOrder = true;
}
