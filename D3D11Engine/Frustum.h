#pragma once
#include "pch.h"
#include <DirectXMath.h>
#include <DirectXCollision.h>
#include <array>

using namespace DirectX;

class Frustum {
public:
    // Für orthografische Projektion (Sonnen-Shadowmap)
    // shadowCasterExpansion: Extra distance to expand the frustum to include shadow casters
    //                        behind/beside the camera that may cast shadows into the view
    void BuildOrthographic( FXMMATRIX view, FXMMATRIX proj, 
        float expandBack = 0.0f,
        float expandSides = 0.0f) {
        // Erstelle Frustum aus View-Projection Matrix
        XMMATRIX viewProj = XMMatrixMultiply( view, proj );
        BoundingFrustum::CreateFromMatrix( m_frustum, proj );

        // Transformiere in World-Space
        XMMATRIX invView = XMMatrixInverse( nullptr, view );
        m_frustum.Transform( m_frustum, invView );

        // If expansion is requested, convert to an expanded bounding box instead
        // This ensures shadow casters outside the direct view are still rendered
        if ( expandBack > 0.0f || expandBack < 0.0f || expandSides > 0.0f ) {
            // Get the AABB of the frustum
            BoundingBox frustumAABB;
            BoundingBox::CreateFromPoints( frustumAABB,
                8,
                reinterpret_cast<const XMFLOAT3*>(&GetFrustumCorners()[0]),
                sizeof( XMFLOAT3 ) );

            // Expand the bounding box to include potential shadow casters
            // Expand more in the light direction (negative Z in light space) and sides
            m_expandedAABB.Center = frustumAABB.Center;
            m_expandedAABB.Extents = XMFLOAT3(
                frustumAABB.Extents.x + expandSides,
                frustumAABB.Extents.y + expandSides,
                frustumAABB.Extents.z + expandSides
            );

            // Also shift the center backwards (in light direction) to catch casters behind
            XMVECTOR lightDir = invView.r[2]; // Z-axis of inverse view = light direction
            XMVECTOR centerVec = XMLoadFloat3( &m_expandedAABB.Center );
            centerVec = XMVectorAdd( centerVec, XMVectorScale( lightDir, -expandBack ) );
            XMStoreFloat3( &m_expandedAABB.Center, centerVec );

            m_useExpandedAABB = true;
        } else {
            m_useExpandedAABB = false;
        }

        m_useSphere = false;
    }

    // Für Pointlight Cubemap (6 Frustums)
    void BuildCubemapFace( FXMVECTOR position, float range, UINT faceIndex ) {
        // Cubemap-Frustum ist effektiv eine Sphere
        XMStoreFloat3( &m_boundingSphere.Center, position );
        m_boundingSphere.Radius = range;
        m_useSphere = true;
    }

    // Schneller AABB-Test
    bool Intersects( const BoundingBox& aabb ) const {
        if ( m_useSphere ) {
            return m_boundingSphere.Intersects( aabb );
        }
        if ( m_useExpandedAABB ) {
            return m_expandedAABB.Intersects( aabb );
        }
        return m_frustum.Intersects( aabb );
    }

    // Schneller Sphere-Test für VOBs
    bool Intersects( const BoundingSphere& sphere ) const {
        if ( m_useSphere ) {
            return m_boundingSphere.Intersects( sphere );
        }
        if ( m_useExpandedAABB ) {
            return m_expandedAABB.Intersects( sphere );
        }
        return m_frustum.Intersects( sphere );
    }

    // Schneller AABB-Test
    DirectX::ContainmentType Contains( const BoundingBox& aabb ) const {
        if ( m_useSphere ) {
            return m_boundingSphere.Contains( aabb );
        }
        if ( m_useExpandedAABB ) {
            return m_expandedAABB.Contains( aabb );
        }
        return m_frustum.Contains( aabb );
    }

    // Schneller Sphere-Test für VOBs
    DirectX::ContainmentType Contains( const BoundingSphere& sphere ) const {
        if ( m_useSphere ) {
            return m_boundingSphere.Contains( sphere );
        }
        if ( m_useExpandedAABB ) {
            return m_expandedAABB.Contains( sphere );
        }
        return m_frustum.Contains( sphere );
    }

    // Batch-Test mit SIMD (4 Spheres gleichzeitig)
    void IntersectsBatch4(
        const XMFLOAT3* centers,
        const float* radii,
        bool* results ) const {

        XMVECTOR c0 = XMLoadFloat3( &centers[0] );
        XMVECTOR c1 = XMLoadFloat3( &centers[1] );
        XMVECTOR c2 = XMLoadFloat3( &centers[2] );
        XMVECTOR c3 = XMLoadFloat3( &centers[3] );

        XMVECTOR sphereCenter = XMLoadFloat3( &m_boundingSphere.Center );
        XMVECTOR sphereRadiusSq = XMVectorReplicate(
            m_boundingSphere.Radius * m_boundingSphere.Radius );

        // Distanz² für alle 4 gleichzeitig
        XMVECTOR d0 = XMVector3LengthSq( XMVectorSubtract( c0, sphereCenter ) );
        XMVECTOR d1 = XMVector3LengthSq( XMVectorSubtract( c1, sphereCenter ) );
        XMVECTOR d2 = XMVector3LengthSq( XMVectorSubtract( c2, sphereCenter ) );
        XMVECTOR d3 = XMVector3LengthSq( XMVectorSubtract( c3, sphereCenter ) );

        // Kombinierte Radien²
        XMVECTOR r0 = XMVectorReplicate( radii[0] + m_boundingSphere.Radius );
        XMVECTOR r1 = XMVectorReplicate( radii[1] + m_boundingSphere.Radius );
        XMVECTOR r2 = XMVectorReplicate( radii[2] + m_boundingSphere.Radius );
        XMVECTOR r3 = XMVectorReplicate( radii[3] + m_boundingSphere.Radius );

        r0 = XMVectorMultiply( r0, r0 );
        r1 = XMVectorMultiply( r1, r1 );
        r2 = XMVectorMultiply( r2, r2 );
        r3 = XMVectorMultiply( r3, r3 );

        results[0] = XMVector3LessOrEqual( d0, r0 );
        results[1] = XMVector3LessOrEqual( d1, r1 );
        results[2] = XMVector3LessOrEqual( d2, r2 );
        results[3] = XMVector3LessOrEqual( d3, r3 );
    }

private:
    // Helper to get frustum corners for AABB creation
    std::array<XMFLOAT3, 8> GetFrustumCorners() const {
        std::array<XMFLOAT3, 8> corners;
        m_frustum.GetCorners( corners.data() );
        return corners;
    }

    BoundingFrustum m_frustum;
    BoundingSphere m_boundingSphere;
    BoundingBox m_expandedAABB;
    bool m_useSphere = false;
    bool m_useExpandedAABB = false;
};

