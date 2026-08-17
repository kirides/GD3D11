#pragma once
#include "pch.h"

#pragma pack (push, 1)
struct LineVertex {
    LineVertex() {}
    LineVertex( const XMFLOAT3& position, DWORD color = 0xFFFFFFFF ) {
        Position = position;
        Color = color;
    }

    LineVertex( const XMFLOAT3& position, const XMFLOAT4& color, float zScale = 1.0f ) {
        Position = position;
        Position.w = zScale;
        Color = color;
    }

    LineVertex( const XMFLOAT3& position, const float4& color, float zScale = 1.0f ) {
        Position = position;
        Position.w = zScale;
        Color = color;
    }

    float4 Position;
    float4 Color;
};
#pragma pack (pop)

class BaseLineRenderer {
public:
    BaseLineRenderer();
    virtual ~BaseLineRenderer();

    /** Adds a line to the list */
    virtual XRESULT AddLine( const LineVertex& v1, const LineVertex& v2 );
    virtual XRESULT AddLineScreenSpace( const LineVertex& v1, const LineVertex& v2 );

    /** Flushes the cached lines */
    virtual XRESULT Flush() = 0;
    virtual XRESULT FlushScreenSpace() = 0;

    /** Clears the line cache */
    virtual XRESULT ClearCache();

    /** Adds a point locator to the renderlist */
    void AddPointLocator( const XMFLOAT3& location, float size = 1, const XMFLOAT4& color = XMFLOAT4( 1, 1, 1, 1 ) );

    /** Adds a plane to the renderlist */
    void AddPlane( const XMFLOAT4& plane, const XMFLOAT3& origin, float size = 1, const XMFLOAT4& color = XMFLOAT4( 1, 1, 1, 1 ) );

    /** Adds a ring to the renderlist */
    void AddRingZ( const XMFLOAT3& location, float size = 1.0f, const XMFLOAT4& color = XMFLOAT4( 1, 1, 1, 1 ), int res = 32 );

    /** Adds an AABB-Box to the renderlist */
    void AddAABB( const XMFLOAT3& location, float halfSize, const XMFLOAT4& color = XMFLOAT4( 1, 1, 1, 1 ) );
    void AddAABB( const XMFLOAT3& location, const XMFLOAT3& halfSize, const XMFLOAT4& color = XMFLOAT4( 1, 1, 1, 1 ) );
    void AddAABBMinMax( const XMFLOAT3& min, const XMFLOAT3& max, const XMFLOAT4& color = XMFLOAT4( 1, 1, 1, 1 ) );

    /** Adds a triangle to the renderlist */
    void AddTriangle( const XMFLOAT3& t0, const XMFLOAT3& t1, const XMFLOAT3& t2, const XMFLOAT4& color = XMFLOAT4( 1, 1, 1, 1 ) );

    /** Plots a vector of floats */
    void PlotNumbers( const std::vector<float>& values, const XMFLOAT3& location, const XMFLOAT3& direction, float distance, float heightScale, const XMFLOAT4& color = XMFLOAT4( 1, 1, 1, 1 ) );

    /** Draws a wireframe mesh */
    void AddWireframeMesh( const std::vector<ExVertexStruct>& vertices, const std::vector<VERTEX_INDEX>& indices, const XMFLOAT4& color = XMFLOAT4( 1, 1, 1, 1 ), const XMFLOAT4X4* world = nullptr );

protected:
    // Hard cap per list, in VERTICES (two per line). Nothing drains these lists unless a frame actually
    // renders the world, so an unbounded push (a stuck editor overlay, a world that never renders) would
    // otherwise grow forever. 1M vertices = 32 MB of cache — far past anything the debug overlays emit.
    static constexpr size_t kMaxCachedVertices = 1u << 20;

    /** Line cache, shared storage for backends to Flush()/FlushScreenSpace() from */
    std::vector<LineVertex> LineCache;
    std::vector<LineVertex> ScreenSpaceLineCache;
    bool CacheOverflowLogged = false;
};

