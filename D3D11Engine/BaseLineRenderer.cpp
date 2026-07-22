#include "pch.h"
#include "BaseLineRenderer.h"

BaseLineRenderer::BaseLineRenderer() {}

BaseLineRenderer::~BaseLineRenderer() {}

/** Plots a vector of floats */
void BaseLineRenderer::PlotNumbers( const std::vector<float>& values, const XMFLOAT3& location, const XMFLOAT3& direction, float distance, float heightScale, const XMFLOAT4& color ) {
    for ( unsigned int i = 1; i < values.size(); i++ ) {
        XMFLOAT3 v1;
        XMFLOAT3 v2;
        XMStoreFloat3( &v1, XMLoadFloat3( &location ) + (XMLoadFloat3( &direction ) * ((i - 1) * distance)) + XMVectorSet( 0, 0, values[i - 1] * heightScale, 0 ) );
        XMStoreFloat3( &v2, XMLoadFloat3( &location ) + (XMLoadFloat3( &direction ) * (i * distance)) + XMVectorSet( 0, 0, values[i] * heightScale, 0 ) );
        AddLine( LineVertex( v1, color ), LineVertex( v2, color ) );
    }
}

/** Adds a triangle to the renderlist */
void BaseLineRenderer::AddTriangle( const XMFLOAT3& t0, const XMFLOAT3& t1, const XMFLOAT3& t2, const XMFLOAT4& color ) {
    AddLine( LineVertex( t0, color ), LineVertex( t1, color ) );
    AddLine( LineVertex( t0, color ), LineVertex( t2, color ) );
    AddLine( LineVertex( t1, color ), LineVertex( t2, color ) );
}

/** Adds a point locator to the renderlist */
void BaseLineRenderer::AddPointLocator( const XMFLOAT3& location, float size, const XMFLOAT4& color ) {
    XMFLOAT3 u = location; u.z += size;
    XMFLOAT3 d = location; d.z -= size;

    XMFLOAT3 r = location; r.x += size;
    XMFLOAT3 l = location; l.x -= size;

    XMFLOAT3 b = location; b.y += size;
    XMFLOAT3 f = location; f.y -= size;

    AddLine( LineVertex( u, color ), LineVertex( d, color ) );
    AddLine( LineVertex( r, color ), LineVertex( l, color ) );
    AddLine( LineVertex( f, color ), LineVertex( b, color ) );
}

/** Adds a plane to the renderlist */
void BaseLineRenderer::AddPlane( const XMFLOAT4& plane, const XMFLOAT3& origin, float size, const XMFLOAT4& color ) {

    FXMVECTOR DebugPlaneP1 = XMVector3Normalize( XMVectorSet( 1, 1, ((-plane.x - plane.y) / plane.z), 0 ) );

    FXMVECTOR pNormal = XMLoadFloat4( &plane ); //w component of plane will not be used in XMVectorCross3 therefore upfront usage of FXMVECTOR is concenient
    FXMVECTOR DebugPlaneP2 = XMVector3Cross( pNormal, DebugPlaneP1 );

    //DebugPlaneP2 += SlidingPlaneOrigin;
    FXMVECTOR O = XMLoadFloat3( &origin );

    XMFLOAT3 from;  XMFLOAT3 to;
    XMStoreFloat3( &from, (O - DebugPlaneP1) - DebugPlaneP2 );
    XMStoreFloat3( &to, (O - DebugPlaneP1) + DebugPlaneP2 );
    AddLine( LineVertex( from ), LineVertex( to ) );

    XMStoreFloat3( &from, (O - DebugPlaneP1) + DebugPlaneP2 );
    XMStoreFloat3( &to, (O + DebugPlaneP1) + DebugPlaneP2 );
    AddLine( LineVertex( from ), LineVertex( to ) );

    XMStoreFloat3( &from, (O + DebugPlaneP1) + DebugPlaneP2 );
    XMStoreFloat3( &to, (O + DebugPlaneP1) - DebugPlaneP2 );
    AddLine( LineVertex( from ), LineVertex( to ) );

    XMStoreFloat3( &from, (O + DebugPlaneP1) - DebugPlaneP2 );
    XMStoreFloat3( &to, (O - DebugPlaneP1) - DebugPlaneP2 );
    AddLine( LineVertex( from ), LineVertex( to ) );
}

/** Adds an AABB-Box to the renderlist */
void BaseLineRenderer::AddAABB( const XMFLOAT3& location, float halfSize, const XMFLOAT4& color ) {
    // Bottom -x -y -z to +x -y -z
    AddLine( LineVertex( XMFLOAT3( location.x - halfSize, location.y - halfSize, location.z - halfSize ), color ), LineVertex( XMFLOAT3( location.x + halfSize, location.y - halfSize, location.z - halfSize ), color ) );

    AddLine( LineVertex( XMFLOAT3( location.x + halfSize, location.y - halfSize, location.z - halfSize ), color ), LineVertex( XMFLOAT3( location.x + halfSize, location.y + halfSize, location.z - halfSize ), color ) );

    AddLine( LineVertex( XMFLOAT3( location.x + halfSize, location.y + halfSize, location.z - halfSize ), color ), LineVertex( XMFLOAT3( location.x - halfSize, location.y + halfSize, location.z - halfSize ), color ) );

    AddLine( LineVertex( XMFLOAT3( location.x - halfSize, location.y + halfSize, location.z - halfSize ), color ), LineVertex( XMFLOAT3( location.x - halfSize, location.y - halfSize, location.z - halfSize ), color ) );

    // Top
    AddLine( LineVertex( XMFLOAT3( location.x - halfSize, location.y - halfSize, location.z + halfSize ), color ), LineVertex( XMFLOAT3( location.x + halfSize, location.y - halfSize, location.z + halfSize ), color ) );

    AddLine( LineVertex( XMFLOAT3( location.x + halfSize, location.y - halfSize, location.z + halfSize ), color ), LineVertex( XMFLOAT3( location.x + halfSize, location.y + halfSize, location.z + halfSize ), color ) );

    AddLine( LineVertex( XMFLOAT3( location.x + halfSize, location.y + halfSize, location.z + halfSize ), color ), LineVertex( XMFLOAT3( location.x - halfSize, location.y + halfSize, location.z + halfSize ), color ) );

    AddLine( LineVertex( XMFLOAT3( location.x - halfSize, location.y + halfSize, location.z + halfSize ), color ), LineVertex( XMFLOAT3( location.x - halfSize, location.y - halfSize, location.z + halfSize ), color ) );

    // Sides
    AddLine( LineVertex( XMFLOAT3( location.x - halfSize, location.y - halfSize, location.z + halfSize ), color ), LineVertex( XMFLOAT3( location.x - halfSize, location.y - halfSize, location.z - halfSize ), color ) );

    AddLine( LineVertex( XMFLOAT3( location.x + halfSize, location.y - halfSize, location.z + halfSize ), color ), LineVertex( XMFLOAT3( location.x + halfSize, location.y - halfSize, location.z - halfSize ), color ) );

    AddLine( LineVertex( XMFLOAT3( location.x + halfSize, location.y + halfSize, location.z + halfSize ), color ), LineVertex( XMFLOAT3( location.x + halfSize, location.y + halfSize, location.z - halfSize ), color ) );

    AddLine( LineVertex( XMFLOAT3( location.x - halfSize, location.y + halfSize, location.z + halfSize ), color ), LineVertex( XMFLOAT3( location.x - halfSize, location.y + halfSize, location.z - halfSize ), color ) );

}

/** Adds an AABB-Box to the renderlist */
void BaseLineRenderer::AddAABB( const XMFLOAT3& location, const XMFLOAT3& halfSize, const XMFLOAT4& color ) {
    AddAABBMinMax( XMFLOAT3( location.x - halfSize.x,
        location.y - halfSize.y,
        location.z - halfSize.z ), XMFLOAT3( location.x + halfSize.x,
            location.y + halfSize.y,
            location.z + halfSize.z ), color );
}



void BaseLineRenderer::AddAABBMinMax( const XMFLOAT3& min, const XMFLOAT3& max, const XMFLOAT4& color ) {
    AddLine( LineVertex( XMFLOAT3( min.x, min.y, min.z ), color ), LineVertex( XMFLOAT3( max.x, min.y, min.z ), color ) );
    AddLine( LineVertex( XMFLOAT3( max.x, min.y, min.z ), color ), LineVertex( XMFLOAT3( max.x, max.y, min.z ), color ) );
    AddLine( LineVertex( XMFLOAT3( max.x, max.y, min.z ), color ), LineVertex( XMFLOAT3( min.x, max.y, min.z ), color ) );
    AddLine( LineVertex( XMFLOAT3( min.x, max.y, min.z ), color ), LineVertex( XMFLOAT3( min.x, min.y, min.z ), color ) );

    AddLine( LineVertex( XMFLOAT3( min.x, min.y, max.z ), color ), LineVertex( XMFLOAT3( max.x, min.y, max.z ), color ) );
    AddLine( LineVertex( XMFLOAT3( max.x, min.y, max.z ), color ), LineVertex( XMFLOAT3( max.x, max.y, max.z ), color ) );
    AddLine( LineVertex( XMFLOAT3( max.x, max.y, max.z ), color ), LineVertex( XMFLOAT3( min.x, max.y, max.z ), color ) );
    AddLine( LineVertex( XMFLOAT3( min.x, max.y, max.z ), color ), LineVertex( XMFLOAT3( min.x, min.y, max.z ), color ) );

    AddLine( LineVertex( XMFLOAT3( min.x, min.y, min.z ), color ), LineVertex( XMFLOAT3( min.x, min.y, max.z ), color ) );
    AddLine( LineVertex( XMFLOAT3( max.x, min.y, min.z ), color ), LineVertex( XMFLOAT3( max.x, min.y, max.z ), color ) );
    AddLine( LineVertex( XMFLOAT3( max.x, max.y, min.z ), color ), LineVertex( XMFLOAT3( max.x, max.y, max.z ), color ) );
    AddLine( LineVertex( XMFLOAT3( min.x, max.y, min.z ), color ), LineVertex( XMFLOAT3( min.x, max.y, max.z ), color ) );
}

/** Adds a ring to the renderlist */
void BaseLineRenderer::AddRingZ( const XMFLOAT3& location, float size, const XMFLOAT4& color, int res ) {
    std::vector<XMFLOAT3> points;
    float step = XM_2PI / res;

    for ( int i = 0; i < res; i++ ) {
        points.push_back( XMFLOAT3( size * sinf( step * i ) + location.x, size * cosf( step * i ) + location.y, location.z ) );
    }

    for ( unsigned int i = 0; i < points.size() - 1; i++ ) {
        AddLine( LineVertex( points[i], color ), LineVertex( points[i + 1], color ) );
    }

    AddLine( LineVertex( points[points.size() - 1], color ), LineVertex( points[0], color ) );
}

/** Draws a wireframe mesh */
void BaseLineRenderer::AddWireframeMesh( const std::vector<ExVertexStruct>& vertices, const std::vector<VERTEX_INDEX>& indices, const XMFLOAT4& color, const XMFLOAT4X4* world ) {
    for ( size_t i = 0; i < indices.size(); i += 3 ) {
        XMFLOAT3 vx[3];
        for ( int v = 0; v < 3; v++ ) {
            if ( world ) {
                XMStoreFloat3( &vx[v], XMVector3TransformCoord( XMVectorSet( vertices[indices[i + v]].Position.x, vertices[indices[i + v]].Position.y, vertices[indices[i + v]].Position.z, 0.0f ), XMLoadFloat4x4( world ) ) );
            } else
                vx[v] = *vertices[indices[i + v]].Position.toXMFLOAT3();
        }

        AddTriangle( vx[0], vx[1], vx[2], color );
    }
}
