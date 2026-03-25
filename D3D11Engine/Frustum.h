#pragma once
#include "pch.h"
#include <DirectXMath.h>
#include <DirectXCollision.h>
#include <array>
#include "zTypes.h"

using namespace DirectX;

enum EGothicCullFlags : unsigned char
{
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

class Frustum
{
public:
    // Für orthografische Projektion (Sonnen-Shadowmap)
    // shadowCasterExpansion: Extra distance to expand the frustum to include shadow casters
    //                        behind/beside the camera that may cast shadows into the view
    void __vectorcall BuildOrthographic(
        FXMMATRIX view,
        float viewWidth,
        float viewHeight,
        float nearZ,
        float farZ,
        float expandSides = 0.0f,
        float expandFront = 0.0f,
        float expandBack = 0.0f
    )
    {
        XMMATRIX invView = XMMatrixInverse( nullptr, view );

        // Calculate new Z bounds directly in Light Space
        float newNearZ = nearZ - expandBack;
        float newFarZ = farZ + expandFront;

        // Construct the center and extents in perfect Light Space
        XMFLOAT3 center( 0.0f, 0.0f, (newFarZ + newNearZ) * 0.5f );
        XMFLOAT3 extents(
            (viewWidth * 0.5f) + expandSides,
            (viewHeight * 0.5f) + expandSides,
            (newFarZ - newNearZ) * 0.5f
        );

        BoundingOrientedBox viewSpaceFrustum( center, extents, { 0, 0, 0, 1 } /* Identity */ );

        // Transform correctly to World Space
        viewSpaceFrustum.Transform( m_orientedBox, invView );

        m_useBoundingOrientedBox = true;
        m_useSphere = false;
        m_always_containing = false;
        isValid = true;
    }

    // for use with shadow mapping if the last cascade is covering the whole map.
    static Frustum AlwaysContainingFrustum() {
        Frustum f;
        f.m_always_containing = true;
        f.isValid = true;
        return f;
    }

    bool SupportsCulling() const { return !m_always_containing; }

    // Für perspektivische Projektion (normale Kamera)
    void __vectorcall BuildPerspective(FXMMATRIX view, CXMMATRIX proj) {
        // Erstelle Frustum aus Projection Matrix
        BoundingFrustum::CreateFromMatrix(m_frustum, proj);

        /*static const XMMATRIX rotationY180 = XMMatrixRotationY( XM_PI );
        m_frustum.Transform(m_frustum, rotationY180 );*/

        // Transformiere in World-Space
        XMMATRIX invView = XMMatrixInverse(nullptr, view);
        m_frustum.Transform(m_frustum, invView);

        // Cache world-space planes for fast culling
        CacheWorldSpacePlanes();

        m_useSphere = false;
        m_useBoundingOrientedBox = false;
        m_always_containing = false;
        isValid = true;
    }

    // Für Pointlight Cubemap (6 Frustums)
    void BuildCubemapFace(FXMVECTOR position, float range, UINT faceIndex) {
        // Cubemap-Frustum ist effektiv eine Sphere
        XMStoreFloat3(&m_boundingSphere.Center, position);

        // For infinite depth, use a very large radius
        m_boundingSphere.Radius = range;

        m_useSphere = true;
        m_useBoundingOrientedBox = false;
        m_always_containing = false;
        isValid = true;
    }

    // Schneller AABB-Test
    bool Intersects(const BoundingBox& aabb) const {
        if (m_always_containing) return true;

        if (m_useSphere) {
            return m_boundingSphere.Intersects(aabb);
        }
        if (m_useBoundingOrientedBox) {
            return m_orientedBox.Intersects(aabb);
        }
        return m_frustum.Intersects(aabb);
    }

    // Schneller Sphere-Test für VOBs
    bool Intersects(const BoundingSphere& sphere) const {
        if (m_always_containing) return true;

        if (m_useSphere) {
            return m_boundingSphere.Intersects(sphere);
        }
        if (m_useBoundingOrientedBox) {
            return m_orientedBox.Intersects(sphere);
        }
        return m_frustum.Intersects(sphere);
    }

    // Schneller AABB-Test
    DirectX::ContainmentType Contains(const BoundingBox& aabb) const {
        if (m_always_containing) return ContainmentType::CONTAINS;
        if (m_useSphere) {
            return m_boundingSphere.Contains(aabb);
        }
        if (m_useBoundingOrientedBox) {
            return m_orientedBox.Contains(aabb);
        }
        return aabb.ContainedBy(
            XMLoadFloat4(&m_cachedPlanes[0]),
            XMLoadFloat4(&m_cachedPlanes[1]),
            XMLoadFloat4(&m_cachedPlanes[2]),
            XMLoadFloat4(&m_cachedPlanes[3]),
            XMLoadFloat4(&m_cachedPlanes[4]),
            XMLoadFloat4(&m_cachedPlanes[5])
        );
    }

    bool Intersects( const zTBBox3D& aabb ) const {
        if ( m_always_containing ) return true;
        return Intersects( BBoxFromzTBBox3D( aabb ) );
    }

    DirectX::ContainmentType Contains(const zTBBox3D& aabb) const {
        if (m_always_containing) return ContainmentType::CONTAINS;
        return Contains(BBoxFromzTBBox3D(aabb));
    }
    
    // Schneller Sphere-Test für VOBs
    DirectX::ContainmentType Contains(const BoundingSphere& sphere) const {
        if (m_always_containing) return ContainmentType::CONTAINS;
        if (m_useSphere) {
            return m_boundingSphere.Contains(sphere);
        }
        if (m_useBoundingOrientedBox) {
            return m_orientedBox.Contains(sphere);
        }
        return sphere.ContainedBy(
            XMLoadFloat4( &m_cachedPlanes[0] ),
            XMLoadFloat4( &m_cachedPlanes[1] ),
            XMLoadFloat4( &m_cachedPlanes[2] ),
            XMLoadFloat4( &m_cachedPlanes[3] ),
            XMLoadFloat4( &m_cachedPlanes[4] ),
            XMLoadFloat4( &m_cachedPlanes[5] )
        );
    }

    ContainmentType Contains(const BoundingSphere& sh, EGothicCullFlags flags) const noexcept {
        return Contains(sh);
    }

    ContainmentType Contains(const BoundingBox& bb, EGothicCullFlags flags) const noexcept {
        return Contains(bb);
    }

    static BoundingBox BBoxFromzTBBox3D(const zTBBox3D& box) {
        BoundingBox bb;
        XMVECTOR bbMin = XMLoadFloat3(&box.Min);
        XMVECTOR bbMax = XMLoadFloat3(&box.Max);
        XMStoreFloat3(&bb.Center, XMVectorScale(XMVectorAdd(bbMin, bbMax), 0.5f));
        XMStoreFloat3(&bb.Extents, XMVectorScale(XMVectorSubtract(bbMax, bbMin), 0.5f));
        return bb;
    }
    
    static BoundingSphere BSphereFromzTBBox3D(const zTBBox3D& box) {
        BoundingSphere sp;
        sp.CreateFromBoundingBox(sp, BBoxFromzTBBox3D(box));
        return sp;
    }

    bool IsValid() const { return isValid; }

    const std::array<XMFLOAT4, 6>& GetPlanes() const { return m_cachedPlanes; }

    // Extract the 8 corners for a specific slice of the frustum
    std::array<XMFLOAT3, 8> GetSliceCorners( float nearZ, float farZ ) const {
        if ( m_always_containing || m_useSphere || m_useBoundingOrientedBox ) {
            return GetFrustumCorners(); // Fallback
        }
        BoundingFrustum slice = m_frustum;
        slice.Near = nearZ;
        slice.Far = farZ;
        std::array<XMFLOAT3, 8> corners;
        slice.GetCorners( corners.data() );
        return corners;
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

private:
    // Helper to get frustum corners for AABB creation
    std::array<XMFLOAT3, 8> GetFrustumCorners() const {
        std::array<XMFLOAT3, 8> corners;
        m_frustum.GetCorners(corners.data());
        return corners;
    }

    BoundingFrustum m_frustum;
    BoundingSphere m_boundingSphere;
    BoundingOrientedBox m_orientedBox;

    std::array<XMFLOAT4, 6> m_cachedPlanes{}; // [0]=Left, [1]=Right, [2]=Bottom, [3]=Top, [4]=Near, [5]=Far
    bool m_useSphere = false;
    bool m_useBoundingOrientedBox = false;
    bool m_always_containing = false;
    bool isValid = false;
};
