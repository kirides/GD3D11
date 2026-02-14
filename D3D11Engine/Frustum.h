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
        
        // Cache world-space planes for fast culling
        CacheWorldSpacePlanes();

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
        
        // Cache world-space planes for fast culling
        CacheWorldSpacePlanes();
        
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
        
        // Use pre-cached world-space planes for fast culling
        // Load cached planes into XMVECTOR for the ContainedBy call
        XMVECTOR LeftPlane = XMLoadFloat4(&m_cachedPlanes[0]);
        XMVECTOR RightPlane = XMLoadFloat4(&m_cachedPlanes[1]);
        XMVECTOR BottomPlane = XMLoadFloat4(&m_cachedPlanes[2]);
        XMVECTOR TopPlane = XMLoadFloat4(&m_cachedPlanes[3]);
        XMVECTOR NearPlane = XMLoadFloat4(&m_cachedPlanes[4]);
        XMVECTOR FarPlane = XMLoadFloat4(&m_cachedPlanes[5]);
        
        return ContainedBy(
            sh,
            flags,
            LeftPlane, RightPlane, BottomPlane, 
            TopPlane, NearPlane, FarPlane);
    }
    
    ContainmentType Contains(const BoundingBox& bb, EGothicCullFlags flags) const noexcept {
        if (!flags) {
            // Cull against nothing? Then just say its somewhere.
            return ContainmentType::INTERSECTS;
        }
        
        // Use pre-cached world-space planes for fast culling
        // Load cached planes into XMVECTOR for the ContainedBy call
        XMVECTOR LeftPlane = XMLoadFloat4(&m_cachedPlanes[0]);
        XMVECTOR RightPlane = XMLoadFloat4(&m_cachedPlanes[1]);
        XMVECTOR BottomPlane = XMLoadFloat4(&m_cachedPlanes[2]);
        XMVECTOR TopPlane = XMLoadFloat4(&m_cachedPlanes[3]);
        XMVECTOR NearPlane = XMLoadFloat4(&m_cachedPlanes[4]);
        XMVECTOR FarPlane = XMLoadFloat4(&m_cachedPlanes[5]);
        
        return ContainedBy(
            bb,
            flags,
            LeftPlane, RightPlane, BottomPlane, 
            TopPlane, NearPlane, FarPlane);
    }

private:
    // Cache world-space planes for fast culling (called after frustum is transformed to world space)
    // Plane order: [0]=Left, [1]=Right, [2]=Bottom, [3]=Top, [4]=Near, [5]=Far
    void CacheWorldSpacePlanes() {
        // Load origin and orientation of the frustum
        XMVECTOR vOrigin = XMLoadFloat3(&m_frustum.Origin);
        XMVECTOR vOrientation = XMLoadFloat4(&m_frustum.Orientation);

        // Left plane
        XMVECTOR plane = XMVectorSet(-1.0f, 0.0f, m_frustum.LeftSlope, 0.0f);
        plane = DirectX::MathInternal::XMPlaneTransform(plane, vOrientation, vOrigin);
        XMStoreFloat4(&m_cachedPlanes[0], XMPlaneNormalize(plane));

        // Right plane
        plane = XMVectorSet(1.0f, 0.0f, -m_frustum.RightSlope, 0.0f);
        plane = DirectX::MathInternal::XMPlaneTransform(plane, vOrientation, vOrigin);
        XMStoreFloat4(&m_cachedPlanes[1], XMPlaneNormalize(plane));

        // Bottom plane
        plane = XMVectorSet(0.0f, -1.0f, m_frustum.BottomSlope, 0.0f);
        plane = DirectX::MathInternal::XMPlaneTransform(plane, vOrientation, vOrigin);
        XMStoreFloat4(&m_cachedPlanes[2], XMPlaneNormalize(plane));

        // Top plane
        plane = XMVectorSet(0.0f, 1.0f, -m_frustum.TopSlope, 0.0f);
        plane = DirectX::MathInternal::XMPlaneTransform(plane, vOrientation, vOrigin);
        XMStoreFloat4(&m_cachedPlanes[3], XMPlaneNormalize(plane));

        // Near plane
        plane = XMVectorSet(0.0f, 0.0f, -1.0f, m_frustum.Near);
        plane = DirectX::MathInternal::XMPlaneTransform(plane, vOrientation, vOrigin);
        XMStoreFloat4(&m_cachedPlanes[4], XMPlaneNormalize(plane));

        // Far plane
        plane = XMVectorSet(0.0f, 0.0f, 1.0f, -m_frustum.Far);
        plane = DirectX::MathInternal::XMPlaneTransform(plane, vOrientation, vOrigin);
        XMStoreFloat4(&m_cachedPlanes[5], XMPlaneNormalize(plane));
    }

    // Small utility copied from original DXMath code, to not clip on Far/Near, like original camera does for clip 15
    // Uses branchless masking to exclude planes not in flags
    ContainmentType XM_CALLCONV ContainedBy(const BoundingBox& bbox,
        EGothicCullFlags flags,
        FXMVECTOR LeftPlane, FXMVECTOR RightPlane, 
        FXMVECTOR BottomPlane, GXMVECTOR TopPlane,
        HXMVECTOR NearPlane, HXMVECTOR FarPlane) const noexcept
    {
        // Load the box.
        XMVECTOR vCenter = XMLoadFloat3(&bbox.Center);
        XMVECTOR vExtents = XMLoadFloat3(&bbox.Extents);

        // Set w of the center to one so we can dot4 with a plane.
        vCenter = XMVectorInsert<0, 0, 0, 0, 1>(vCenter, XMVectorSplatOne());

        // Create masks branchlessly: -(!!(x)) produces 0xFFFFFFFF if x!=0, else 0x00000000
        // Using two's complement: when bit is set, !! makes 1, then -1 = 0xFFFFFFFF
        XMVECTOR maskLeft   = XMVectorReplicateInt(static_cast<uint32_t>(-static_cast<int32_t>(!!(flags & CullLeftPlane))));
        XMVECTOR maskRight  = XMVectorReplicateInt(static_cast<uint32_t>(-static_cast<int32_t>(!!(flags & CullRightPlane))));
        XMVECTOR maskBottom = XMVectorReplicateInt(static_cast<uint32_t>(-static_cast<int32_t>(!!(flags & CullBottomPlane))));
        XMVECTOR maskTop    = XMVectorReplicateInt(static_cast<uint32_t>(-static_cast<int32_t>(!!(flags & CullTopPlane))));
        XMVECTOR maskNear   = XMVectorReplicateInt(static_cast<uint32_t>(-static_cast<int32_t>(!!(flags & CullNearPlane))));
        XMVECTOR maskFar    = XMVectorReplicateInt(static_cast<uint32_t>(-static_cast<int32_t>(!!(flags & CullFarPlane))));

        XMVECTOR Outside, Inside;
        XMVECTOR AnyOutside = XMVectorFalseInt();
        XMVECTOR AllInside = XMVectorTrueInt();

        // Left plane - masked
        DirectX::MathInternal::FastIntersectAxisAlignedBoxPlane(vCenter, vExtents, LeftPlane, Outside, Inside);
        AnyOutside = XMVectorOrInt(AnyOutside, XMVectorAndInt(Outside, maskLeft));
        AllInside = XMVectorAndInt(AllInside, XMVectorOrInt(Inside, XMVectorAndCInt(XMVectorTrueInt(), maskLeft)));

        // Right plane - masked
        DirectX::MathInternal::FastIntersectAxisAlignedBoxPlane(vCenter, vExtents, RightPlane, Outside, Inside);
        AnyOutside = XMVectorOrInt(AnyOutside, XMVectorAndInt(Outside, maskRight));
        AllInside = XMVectorAndInt(AllInside, XMVectorOrInt(Inside, XMVectorAndCInt(XMVectorTrueInt(), maskRight)));

        // Bottom plane - masked
        DirectX::MathInternal::FastIntersectAxisAlignedBoxPlane(vCenter, vExtents, BottomPlane, Outside, Inside);
        AnyOutside = XMVectorOrInt(AnyOutside, XMVectorAndInt(Outside, maskBottom));
        AllInside = XMVectorAndInt(AllInside, XMVectorOrInt(Inside, XMVectorAndCInt(XMVectorTrueInt(), maskBottom)));

        // Top plane - masked
        DirectX::MathInternal::FastIntersectAxisAlignedBoxPlane(vCenter, vExtents, TopPlane, Outside, Inside);
        AnyOutside = XMVectorOrInt(AnyOutside, XMVectorAndInt(Outside, maskTop));
        AllInside = XMVectorAndInt(AllInside, XMVectorOrInt(Inside, XMVectorAndCInt(XMVectorTrueInt(), maskTop)));

        // Near plane - masked
        DirectX::MathInternal::FastIntersectAxisAlignedBoxPlane(vCenter, vExtents, NearPlane, Outside, Inside);
        AnyOutside = XMVectorOrInt(AnyOutside, XMVectorAndInt(Outside, maskNear));
        AllInside = XMVectorAndInt(AllInside, XMVectorOrInt(Inside, XMVectorAndCInt(XMVectorTrueInt(), maskNear)));

        // Far plane - masked
        DirectX::MathInternal::FastIntersectAxisAlignedBoxPlane(vCenter, vExtents, FarPlane, Outside, Inside);
        AnyOutside = XMVectorOrInt(AnyOutside, XMVectorAndInt(Outside, maskFar));
        AllInside = XMVectorAndInt(AllInside, XMVectorOrInt(Inside, XMVectorAndCInt(XMVectorTrueInt(), maskFar)));

        // If the box is outside any enabled plane it is outside.
        if (XMVector4EqualInt(AnyOutside, XMVectorTrueInt()))
            return ContainmentType::DISJOINT;

        // If the box is inside all enabled planes it is inside.
        if (XMVector4EqualInt(AllInside, XMVectorTrueInt()))
            return ContainmentType::CONTAINS;

        // The box is not inside all planes or outside a plane, it may intersect.
        return ContainmentType::INTERSECTS;
    }
    
    // Uses branchless masking to exclude planes not in flags
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

        // Create masks branchlessly: -(!!(x)) produces 0xFFFFFFFF if x!=0, else 0x00000000
        // Using two's complement: when bit is set, !! makes 1, then -1 = 0xFFFFFFFF
        XMVECTOR maskLeft   = XMVectorReplicateInt(static_cast<uint32_t>(-static_cast<int32_t>(!!(flags & CullLeftPlane))));
        XMVECTOR maskRight  = XMVectorReplicateInt(static_cast<uint32_t>(-static_cast<int32_t>(!!(flags & CullRightPlane))));
        XMVECTOR maskBottom = XMVectorReplicateInt(static_cast<uint32_t>(-static_cast<int32_t>(!!(flags & CullBottomPlane))));
        XMVECTOR maskTop    = XMVectorReplicateInt(static_cast<uint32_t>(-static_cast<int32_t>(!!(flags & CullTopPlane))));
        XMVECTOR maskNear   = XMVectorReplicateInt(static_cast<uint32_t>(-static_cast<int32_t>(!!(flags & CullNearPlane))));
        XMVECTOR maskFar    = XMVectorReplicateInt(static_cast<uint32_t>(-static_cast<int32_t>(!!(flags & CullFarPlane))));

        XMVECTOR Outside, Inside;
        XMVECTOR AnyOutside = XMVectorFalseInt();
        XMVECTOR AllInside = XMVectorTrueInt();

        // Left plane - masked
        DirectX::MathInternal::FastIntersectSpherePlane(vCenter, vRadius, LeftPlane, Outside, Inside);
        AnyOutside = XMVectorOrInt(AnyOutside, XMVectorAndInt(Outside, maskLeft));
        AllInside = XMVectorAndInt(AllInside, XMVectorOrInt(Inside, XMVectorAndCInt(XMVectorTrueInt(), maskLeft)));

        // Right plane - masked
        DirectX::MathInternal::FastIntersectSpherePlane(vCenter, vRadius, RightPlane, Outside, Inside);
        AnyOutside = XMVectorOrInt(AnyOutside, XMVectorAndInt(Outside, maskRight));
        AllInside = XMVectorAndInt(AllInside, XMVectorOrInt(Inside, XMVectorAndCInt(XMVectorTrueInt(), maskRight)));

        // Bottom plane - masked
        DirectX::MathInternal::FastIntersectSpherePlane(vCenter, vRadius, BottomPlane, Outside, Inside);
        AnyOutside = XMVectorOrInt(AnyOutside, XMVectorAndInt(Outside, maskBottom));
        AllInside = XMVectorAndInt(AllInside, XMVectorOrInt(Inside, XMVectorAndCInt(XMVectorTrueInt(), maskBottom)));

        // Top plane - masked
        DirectX::MathInternal::FastIntersectSpherePlane(vCenter, vRadius, TopPlane, Outside, Inside);
        AnyOutside = XMVectorOrInt(AnyOutside, XMVectorAndInt(Outside, maskTop));
        AllInside = XMVectorAndInt(AllInside, XMVectorOrInt(Inside, XMVectorAndCInt(XMVectorTrueInt(), maskTop)));

        // Near plane - masked
        DirectX::MathInternal::FastIntersectSpherePlane(vCenter, vRadius, NearPlane, Outside, Inside);
        AnyOutside = XMVectorOrInt(AnyOutside, XMVectorAndInt(Outside, maskNear));
        AllInside = XMVectorAndInt(AllInside, XMVectorOrInt(Inside, XMVectorAndCInt(XMVectorTrueInt(), maskNear)));

        // Far plane - masked
        DirectX::MathInternal::FastIntersectSpherePlane(vCenter, vRadius, FarPlane, Outside, Inside);
        AnyOutside = XMVectorOrInt(AnyOutside, XMVectorAndInt(Outside, maskFar));
        AllInside = XMVectorAndInt(AllInside, XMVectorOrInt(Inside, XMVectorAndCInt(XMVectorTrueInt(), maskFar)));

        // If the sphere is outside any enabled plane it is outside.
        if (XMVector4EqualInt(AnyOutside, XMVectorTrueInt()))
            return DISJOINT;

        // If the sphere is inside all enabled planes it is inside.
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
    std::array<XMFLOAT4, 6> m_cachedPlanes{}; // [0]=Left, [1]=Right, [2]=Bottom, [3]=Top, [4]=Near, [5]=Far
    bool m_useSphere = false;
    bool m_useExpandedAABB = false;
};

