#pragma once
#include "pch.h"
#include <DirectXMath.h>
#include <DirectXCollision.h>
#include <array>

using namespace DirectX;

enum EGothicCullFlags : unsigned char {
    CullNone = 0,
    CullLeftPlane = 1 << 0,
    CullRightPlane = 1 << 1,
    CullBottomPlane = 1 << 2,
    CullTopPlane = 1 << 3,
    CullNearPlane = 1 << 4,
    CullFarPlane = 1 << 5,
    
    CullSides = CullLeftPlane | CullRightPlane | CullBottomPlane | CullTopPlane,
    CullSidesNear = CullSides | CullNearPlane,
    
    CullAll = CullSides | CullNearPlane | CullFarPlane,
};

class Frustum {
public:
    // Für orthografische Projektion (Sonnen-Shadowmap)
    // shadowCasterExpansion: Extra distance to expand the frustum to include shadow casters
    //                        behind/beside the camera that may cast shadows into the view
    void BuildOrthographic( 
        CXMMATRIX view,
        CXMMATRIX proj, 
        float expandBack = 0.0f,
        float expandFront = 0.0f,
        float expandSides = 0.0f) {
        
        // Erstelle Frustum aus View-Projection Matrix
        BoundingFrustum::CreateFromMatrix( m_frustum, proj );

        // Transformiere in World-Space
        XMMATRIX invView = XMMatrixInverse( nullptr, view );
        m_frustum.Transform( m_frustum, invView );

        // If expansion is requested, convert to an expanded bounding box instead
        // This ensures shadow casters outside the direct view are still rendered
        if ( expandBack > 0.0f || expandBack < 0.0f || expandFront > 0.0f || expandSides > 0.0f ) {
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
                frustumAABB.Extents.z + expandFront
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

    // Für perspektivische Projektion (normale Kamera)
    void BuildPerspective( FXMMATRIX view, FXMMATRIX proj ) {
        
        // Erstelle Frustum aus Projection Matrix
        BoundingFrustum::CreateFromMatrix( m_frustum, proj );

        // Transformiere in World-Space
        XMMATRIX invView = XMMatrixInverse( nullptr, view );
        m_frustum.Transform( m_frustum, invView );
        
        m_useSphere = false;
        m_useExpandedAABB = false;
    }

    // Für Pointlight Cubemap (6 Frustums)
    void BuildCubemapFace( FXMVECTOR position, float range, UINT faceIndex ) {
        // Cubemap-Frustum ist effektiv eine Sphere
        XMStoreFloat3( &m_boundingSphere.Center, position );
        
        // For infinite depth, use a very large radius
        m_boundingSphere.Radius = range;

        m_useSphere = true;
        m_useExpandedAABB = false;
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
    
    ContainmentType Contains(const BoundingSphere& sh, EGothicCullFlags flags) const noexcept {
        if (!flags) {
            // Cull against nothing? Then just say its somewhere.
            return ContainmentType::INTERSECTS;
        }
        
        // Load origin and orientation of the frustum.
        XMVECTOR vOrigin = XMLoadFloat3(&m_frustum.Origin);
        XMVECTOR vOrientation = XMLoadFloat4(&m_frustum.Orientation);

        // Create 6 planes (do it inline to encourage use of registers)
        XMVECTOR NearPlane = {}, FarPlane = {}, LeftPlane = {}, RightPlane = {}, BottomPlane = {}, TopPlane = {};
        
        // TODO: Create the planes before hand and store them in world space, to avoid transforming them every time. This is what the original camera does as well.
        if (flags & EGothicCullFlags::CullLeftPlane) {
            LeftPlane = XMVectorSet(-1.0f, 0.0f, m_frustum.LeftSlope, 0.0f);
            LeftPlane = DirectX::MathInternal::XMPlaneTransform(LeftPlane, vOrientation, vOrigin);
            LeftPlane = XMPlaneNormalize(LeftPlane);
        }
        if (flags & EGothicCullFlags::CullRightPlane) {
            RightPlane = XMVectorSet(1.0f, 0.0f, -m_frustum.RightSlope, 0.0f);
            RightPlane = DirectX::MathInternal::XMPlaneTransform(RightPlane, vOrientation, vOrigin);
            RightPlane = XMPlaneNormalize(RightPlane);
        }
        if (flags & EGothicCullFlags::CullBottomPlane) {
            BottomPlane = XMVectorSet(0.0f, -1.0f, m_frustum.BottomSlope, 0.0f);
            BottomPlane = DirectX::MathInternal::XMPlaneTransform(BottomPlane, vOrientation, vOrigin);
            BottomPlane = XMPlaneNormalize(BottomPlane);
        }
        if (flags & EGothicCullFlags::CullTopPlane) {
            TopPlane = XMVectorSet(0.0f, 1.0f, -m_frustum.TopSlope, 0.0f);
            TopPlane = DirectX::MathInternal::XMPlaneTransform(TopPlane, vOrientation, vOrigin);
            TopPlane = XMPlaneNormalize(TopPlane);
        }
        if (flags & EGothicCullFlags::CullNearPlane) {
            NearPlane = XMVectorSet(0.0f, 0.0f, -1.0f, m_frustum.Near);
            NearPlane = DirectX::MathInternal::XMPlaneTransform(NearPlane, vOrientation, vOrigin);
            NearPlane = XMPlaneNormalize(NearPlane);
        }
        if (flags & EGothicCullFlags::CullFarPlane) {
            FarPlane = XMVectorSet(0.0f, 0.0f, 1.0f, -m_frustum.Far);
            FarPlane = DirectX::MathInternal::XMPlaneTransform(FarPlane, vOrientation, vOrigin);
            FarPlane = XMPlaneNormalize(FarPlane);
        }

        return ContainedBy(
        sh,
        flags,
        LeftPlane, RightPlane, BottomPlane, TopPlane, NearPlane, FarPlane);
    }

private:
    // Small utility copied from original DXMath code, to not clip on Far/Near, like original camera does for clip 15
    ContainmentType XM_CALLCONV ContainedBy(const BoundingSphere& sh,
        EGothicCullFlags flags,
        FXMVECTOR LeftPlane, FXMVECTOR RightPlane, 
        FXMVECTOR BottomPlane, GXMVECTOR TopPlane,
        HXMVECTOR NearPlane, HXMVECTOR FarPlane) const noexcept
    {
        // Load the sphere.
        XMVECTOR vCenter = XMLoadFloat3(&sh.Center);
        XMVECTOR vRadius = XMVectorReplicatePtr(&sh.Radius);

        // Set w of the center to one so we can dot4 with a plane.
        vCenter = XMVectorInsert<0, 0, 0, 0, 1>(vCenter, XMVectorSplatOne());

        XMVECTOR Outside, Inside, AnyOutside = {}, AllInside = {};

        // Test against each plane.
        if (flags & EGothicCullFlags::CullLeftPlane) {
            DirectX::MathInternal::FastIntersectSpherePlane(vCenter, vRadius, LeftPlane, Outside, Inside);
            AnyOutside = Outside;
            AllInside = Inside;
        }
        if (flags & EGothicCullFlags::CullRightPlane) {
            DirectX::MathInternal::FastIntersectSpherePlane(vCenter, vRadius, RightPlane, Outside, Inside);
            AnyOutside = XMVectorOrInt(AnyOutside, Outside);
            AllInside = XMVectorAndInt(AllInside, Inside);
        }
        if (flags & EGothicCullFlags::CullBottomPlane) {
            DirectX::MathInternal::FastIntersectSpherePlane(vCenter, vRadius, BottomPlane, Outside, Inside);
            AnyOutside = XMVectorOrInt(AnyOutside, Outside);
            AllInside = XMVectorAndInt(AllInside, Inside);
        }
        if (flags & EGothicCullFlags::CullTopPlane) {
            DirectX::MathInternal::FastIntersectSpherePlane(vCenter, vRadius, TopPlane, Outside, Inside);
            AnyOutside = XMVectorOrInt(AnyOutside, Outside);
            AllInside = XMVectorAndInt(AllInside, Inside);
        }
        if (flags & EGothicCullFlags::CullNearPlane) {
            DirectX::MathInternal::FastIntersectSpherePlane(vCenter, vRadius, NearPlane, Outside, Inside);
            AnyOutside = XMVectorOrInt(AnyOutside, Outside);
            AllInside = XMVectorAndInt(AllInside, Inside);
        }
        if (flags & EGothicCullFlags::CullFarPlane) {
            DirectX::MathInternal::FastIntersectSpherePlane(vCenter, vRadius, FarPlane, Outside, Inside);
            AnyOutside = XMVectorOrInt(AnyOutside, Outside);
            AllInside = XMVectorAndInt(AllInside, Inside);
        }

        // If the sphere is outside any plane it is outside.
        if (XMVector4EqualInt(AnyOutside, XMVectorTrueInt()))
            return DISJOINT;

        // If the sphere is inside all planes it is inside.
        if (XMVector4EqualInt(AllInside, XMVectorTrueInt()))
            return CONTAINS;

        // The sphere is not inside all planes or outside a plane, it may intersect.
        return INTERSECTS;
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

