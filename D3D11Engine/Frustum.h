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


        CacheOBBPlanes();

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

        const float cx = aabb.Center.x;
        const float cy = aabb.Center.y;
        const float cz = aabb.Center.z;
        const float ex = aabb.Extents.x;
        const float ey = aabb.Extents.y;
        const float ez = aabb.Extents.z;

        for ( int i = 0; i < 6; ++i ) {
            const float nx = m_cachedPlanes[i].x;
            const float ny = m_cachedPlanes[i].y;
            const float nz = m_cachedPlanes[i].z;
            const float w = m_cachedPlanes[i].w;

            // Distance from the AABB center to the plane
            const float dist = nx * cx + ny * cy + nz * cz + w;

            // Projected radius of the AABB onto the plane's normal
            const float projRadius = ex * std::abs( nx ) + ey * std::abs( ny ) + ez * std::abs( nz );

            // If the center is further outside the plane than its projected radius,
            // the entire box is disjoint. We can early-out immediately.
            if ( dist > projRadius ) {
                return false;
            }
        }

        // If no separating plane was found, it must be intersecting or contained.
        return true;
    }

    // Schneller Sphere-Test für VOBs
    bool Intersects( const BoundingSphere& sphere ) const {
        if ( m_always_containing ) return true;

        if ( m_useSphere ) {
            return m_boundingSphere.Intersects( sphere );
        }

        const float cx = sphere.Center.x;
        const float cy = sphere.Center.y;
        const float cz = sphere.Center.z;
        const float r = sphere.Radius;

        // Scalar early-out loop. 
        // For outward-facing planes, if distance > radius, it is completely outside.
        for ( int i = 0; i < 6; ++i ) {
            const float dist = m_cachedPlanes[i].x * cx +
                m_cachedPlanes[i].y * cy +
                m_cachedPlanes[i].z * cz +
                m_cachedPlanes[i].w;
            if ( dist > r ) {
                return false;
            }
        }

        return true;
    }

    // Schneller AABB-Test
    DirectX::ContainmentType Contains(const BoundingBox& aabb) const {
        if (m_always_containing) return ContainmentType::CONTAINS;
        if (m_useSphere) {
            return m_boundingSphere.Contains(aabb);
        }
        
        const float cx = aabb.Center.x;
        const float cy = aabb.Center.y;
        const float cz = aabb.Center.z;
        const float ex = aabb.Extents.x;
        const float ey = aabb.Extents.y;
        const float ez = aabb.Extents.z;

        bool intersects = false;

        for ( int i = 0; i < 6; ++i ) {
            const float nx = m_cachedPlanes[i].x;
            const float ny = m_cachedPlanes[i].y;
            const float nz = m_cachedPlanes[i].z;
            const float w = m_cachedPlanes[i].w;

            // 1. Calculate distance from the AABB center to the plane
            const float dist = nx * cx + ny * cy + nz * cz + w;

            // 2. Calculate the projected radius of the AABB onto the plane's normal
            const float projRadius = ex * std::abs( nx ) + ey * std::abs( ny ) + ez * std::abs( nz );

            // 3. Since planes are OUTWARD facing:
            if ( dist > projRadius ) {
                return DirectX::ContainmentType::DISJOINT; // Completely outside
            }
            if ( dist > -projRadius ) {
                intersects = true; // Partially inside, keep checking the other planes
            }
        }

        return intersects ? DirectX::ContainmentType::INTERSECTS : DirectX::ContainmentType::CONTAINS;
    }

    bool Intersects( const zTBBox3D& aabb ) const {
        if ( m_always_containing ) return true;
        // Fast scalar conversion - avoids memory->SIMD->memory roundtrip
        BoundingBox bb;
        bb.Center.x = (aabb.Min.x + aabb.Max.x) * 0.5f;
        bb.Center.y = (aabb.Min.y + aabb.Max.y) * 0.5f;
        bb.Center.z = (aabb.Min.z + aabb.Max.z) * 0.5f;
        bb.Extents.x = (aabb.Max.x - aabb.Min.x) * 0.5f;
        bb.Extents.y = (aabb.Max.y - aabb.Min.y) * 0.5f;
        bb.Extents.z = (aabb.Max.z - aabb.Min.z) * 0.5f;

        return Intersects( bb );
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

    static BoundingBox BBoxFromzTBBox3D(const zTBBox3D& aabb) {
        BoundingBox bb;
        bb.Center.x = (aabb.Min.x + aabb.Max.x) * 0.5f;
        bb.Center.y = (aabb.Min.y + aabb.Max.y) * 0.5f;
        bb.Center.z = (aabb.Min.z + aabb.Max.z) * 0.5f;
        bb.Extents.x = (aabb.Max.x - aabb.Min.x) * 0.5f;
        bb.Extents.y = (aabb.Max.y - aabb.Min.y) * 0.5f;
        bb.Extents.z = (aabb.Max.z - aabb.Min.z) * 0.5f;
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

        // Near plane
        XMVECTOR plane = XMVectorSet(0.0f, 0.0f, -1.0f, m_frustum.Near);
        plane = DirectX::MathInternal::XMPlaneTransform(plane, vOrientation, vOrigin);
        XMStoreFloat4(&m_cachedPlanes[0], XMPlaneNormalize(plane));

        // Left plane
        plane = XMVectorSet(-1.0f, 0.0f, m_frustum.LeftSlope, 0.0f);
        plane = DirectX::MathInternal::XMPlaneTransform(plane, vOrientation, vOrigin);
        XMStoreFloat4(&m_cachedPlanes[1], XMPlaneNormalize(plane));

        // Right plane
        plane = XMVectorSet(1.0f, 0.0f, -m_frustum.RightSlope, 0.0f);
        plane = DirectX::MathInternal::XMPlaneTransform(plane, vOrientation, vOrigin);
        XMStoreFloat4(&m_cachedPlanes[2], XMPlaneNormalize(plane));

        // Bottom plane
        plane = XMVectorSet(0.0f, -1.0f, m_frustum.BottomSlope, 0.0f);
        plane = DirectX::MathInternal::XMPlaneTransform(plane, vOrientation, vOrigin);
        XMStoreFloat4(&m_cachedPlanes[3], XMPlaneNormalize(plane));

        // Top plane
        plane = XMVectorSet(0.0f, 1.0f, -m_frustum.TopSlope, 0.0f);
        plane = DirectX::MathInternal::XMPlaneTransform(plane, vOrientation, vOrigin);
        XMStoreFloat4(&m_cachedPlanes[4], XMPlaneNormalize(plane));

        // Far plane
        plane = XMVectorSet(0.0f, 0.0f, 1.0f, -m_frustum.Far);
        plane = DirectX::MathInternal::XMPlaneTransform(plane, vOrientation, vOrigin);
        XMStoreFloat4(&m_cachedPlanes[5], XMPlaneNormalize(plane));
    }
    
    // Cache world-space planes from an Oriented Bounding Box (Directional Light / Ortho)
// Plane order: [0]=Left, [1]=Right, [2]=Bottom, [3]=Top, [4]=Near, [5]=Far
void CacheOBBPlanes() {
    XMVECTOR C = XMLoadFloat3(&m_orientedBox.Center);
    XMVECTOR E = XMLoadFloat3(&m_orientedBox.Extents);
    XMVECTOR Q = XMLoadFloat4(&m_orientedBox.Orientation);

    XMMATRIX R = XMMatrixRotationQuaternion(Q);
    XMVECTOR AxisX = R.r[0];
    XMVECTOR AxisY = R.r[1];
    XMVECTOR AxisZ = R.r[2];

    XMVECTOR Ex = XMVectorSplatX(E);
    XMVECTOR Ey = XMVectorSplatY(E);
    XMVECTOR Ez = XMVectorSplatZ(E);

    // Near face: Min Z boundary. Outward normal is -AxisZ
    XMVECTOR P_Near = XMVectorSubtract( C, XMVectorMultiply( AxisZ, Ez ) );
    XMStoreFloat4( &m_cachedPlanes[0], XMPlaneFromPointNormal( P_Near, XMVectorNegate( AxisZ ) ) );

    // Left face: Min X boundary. Outward normal is -AxisX
    XMVECTOR P_Left = XMVectorSubtract( C, XMVectorMultiply( AxisX, Ex ) );
    XMStoreFloat4( &m_cachedPlanes[1], XMPlaneFromPointNormal( P_Left, XMVectorNegate( AxisX ) ) );

    // Right face: Max X boundary. Outward normal is +AxisX
    XMVECTOR P_Right = XMVectorAdd( C, XMVectorMultiply( AxisX, Ex ) );
    XMStoreFloat4( &m_cachedPlanes[2], XMPlaneFromPointNormal( P_Right, AxisX ) );

    // Bottom face: Min Y boundary. Outward normal is -AxisY
    XMVECTOR P_Bottom = XMVectorSubtract( C, XMVectorMultiply( AxisY, Ey ) );
    XMStoreFloat4( &m_cachedPlanes[3], XMPlaneFromPointNormal( P_Bottom, XMVectorNegate( AxisY ) ) );

    // Top face: Max Y boundary. Outward normal is +AxisY
    XMVECTOR P_Top = XMVectorAdd( C, XMVectorMultiply( AxisY, Ey ) );
    XMStoreFloat4( &m_cachedPlanes[4], XMPlaneFromPointNormal( P_Top, AxisY ) );

    // Far face: Max Z boundary. Outward normal is +AxisZ
    XMVECTOR P_Far = XMVectorAdd( C, XMVectorMultiply( AxisZ, Ez ) );
    XMStoreFloat4( &m_cachedPlanes[5], XMPlaneFromPointNormal( P_Far, AxisZ ) );
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

    std::array<XMFLOAT4, 6> m_cachedPlanes{};
    bool m_useSphere = false;
    bool m_useBoundingOrientedBox = false;
    bool m_always_containing = false;
    bool isValid = false;
};
