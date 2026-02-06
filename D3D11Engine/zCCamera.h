#pragma once
#include "pch.h"
#include "HookedFunctions.h"
#include "Engine.h"
#include "GothicAPI.h"
#include "BaseGraphicsEngine.h"
#include "zTypes.h"


class zCCamera {
public:

    enum ETransformType {
        TT_WORLD,
        TT_VIEW,
        TT_WORLDVIEW,
        TT_WORLDVIEW_INV,
        TT_VIEW_INV
    };

    static bool IsFreeLookActive() {
#ifdef BUILD_GOTHIC_2_6_fix
        return *reinterpret_cast<int*>(GothicMemoryLocations::zCCamera::Var_FreeLook) != 0;
#else
        return false;
#endif
    }

    const XMFLOAT4X4& GetTransformDX( const ETransformType type ) {
        switch ( type ) {
        case ETransformType::TT_WORLD:			return trafoWorld;		break;
        case ETransformType::TT_VIEW:			return trafoView;		break;
        case ETransformType::TT_WORLDVIEW:		return camMatrix;		break;
        case ETransformType::TT_WORLDVIEW_INV:	return camMatrixInv;	break;
        case ETransformType::TT_VIEW_INV:		return trafoViewInv;	break;
        default:						        return camMatrix;		break;
        };
    }

    void SetTransform( const ETransformType type, const XMFLOAT4X4& mat ) {
        reinterpret_cast<void( __fastcall* )( zCCamera*, int, const ETransformType, const XMFLOAT4X4& )>
            ( GothicMemoryLocations::zCCamera::SetTransform )( this, 0, type, mat );
    }

    void SetTransformXM( const ETransformType type, const XMMATRIX& mat ) {
        XMFLOAT4X4 m; XMStoreFloat4x4( &m, mat );
        SetTransform( type, m );
    }

    void Activate() {
        reinterpret_cast<void( __fastcall* )( zCCamera* )>( GothicMemoryLocations::zCCamera::Activate )( this );

        // TODO: actually set the RndD3d viewport and not use this ugly ass workaround
        Engine::GraphicsEngine->SetViewport( ViewportInfo( 0, 0, Engine::GraphicsEngine->GetResolution().x, Engine::GraphicsEngine->GetResolution().y, 0.0f, 1.0f ) );
    }

    void SetFOV( float azi, float elev ) {
        reinterpret_cast<void( __fastcall* )( zCCamera*, int, float, float )>
           ( GothicMemoryLocations::zCCamera::SetFOV )( this, 0, azi, elev );
    }

    void GetFOV( float& fovH, float& fovV ) {
        reinterpret_cast<void( __fastcall* )( zCCamera*, int, float&, float& )>
            ( GothicMemoryLocations::zCCamera::GetFOV_f2 )( this, 0, fovH, fovV );
    }

    void UpdateViewport() {
        reinterpret_cast<void( __fastcall* )( zCCamera* )>( GothicMemoryLocations::zCCamera::UpdateViewport )( this );
    }

    zTCam_ClipType BBox3DInFrustum( const zTBBox3D& box ) {
        //int flags = 15; // Full clip, no farplane
        int flags = 63;
        return BBox3DInFrustum( box, flags );
    }

    zTCam_ClipType BBox3DInFrustum( const zTBBox3D& box, int& clipFlags ) {
        return reinterpret_cast<zTCam_ClipType( __fastcall* )( zCCamera*, int, const zTBBox3D&, int& )>
            ( GothicMemoryLocations::zCCamera::BBox3DInFrustum )( this, 0, box, clipFlags );
    }

    float GetFarPlane() {
        return *reinterpret_cast<float*>(THISPTR_OFFSET( GothicMemoryLocations::zCCamera::Offset_FarPlane ));
    }

    float GetNearPlane() {
        return *reinterpret_cast<float*>(THISPTR_OFFSET( GothicMemoryLocations::zCCamera::Offset_NearPlane ));
    }

    void SetFarPlane( float value ) {
        reinterpret_cast<void( __fastcall* )( zCCamera*, int, float )>
            ( GothicMemoryLocations::zCCamera::SetFarPlane )( this, 0, value );
    }

    bool HasScreenFadeEnabled() {
        return *reinterpret_cast<int*>(THISPTR_OFFSET( GothicMemoryLocations::zCCamera::Offset_ScreenFadeEnabled )) != 0;
    }

    void ResetScreenFadeEnabled() {
        *reinterpret_cast<int*>(THISPTR_OFFSET( GothicMemoryLocations::zCCamera::Offset_ScreenFadeEnabled )) = 0;
    }

    bool HasCinemaScopeEnabled() {
        return *reinterpret_cast<int*>(THISPTR_OFFSET( GothicMemoryLocations::zCCamera::Offset_CinemaScopeEnabled )) != 0;
    }

    void ResetCinemaScopeEnabled() {
        *reinterpret_cast<int*>(THISPTR_OFFSET( GothicMemoryLocations::zCCamera::Offset_CinemaScopeEnabled )) = 0;
    }

    zColor GetScreenFadeColor() {
        return *reinterpret_cast<zColor*>(THISPTR_OFFSET( GothicMemoryLocations::zCCamera::Offset_ScreenFadeColor ));
    }

    zColor GetCinemaScopeColor() {
        return *reinterpret_cast<zColor*>(THISPTR_OFFSET( GothicMemoryLocations::zCCamera::Offset_CinemaScopeColor ));
    }

    int GetScreenFadeBlendFunc() {
#ifdef BUILD_GOTHIC_2_6_fix
        return static_cast<int>(*reinterpret_cast<BYTE*>(THISPTR_OFFSET( GothicMemoryLocations::zCCamera::Offset_ScreenFadeBlendFunc )));
#else
        return zRND_ALPHA_FUNC_BLEND;
#endif
    }

    DWORD GetPolyMaterial() {
        return *reinterpret_cast<DWORD*>(THISPTR_OFFSET( GothicMemoryLocations::zCCamera::Offset_PolyMaterial ));
    }

    /** Returns the frustumplanes */
    zTPlane* GetFrustumPlanes() {
        return FrustumPlanes;
    }

    /** Returns the signbits for the frustumplanes */
    byte* GetFrustumSignBits() {
        return SignBits;
    }

    static zCCamera* GetCamera() {
        return *reinterpret_cast<zCCamera**>(GothicMemoryLocations::GlobalObjects::zCCamera);
    }

    /** Frustum Planes in world space */
    zTPlane FrustumPlanes[6];
    byte SignBits[6];

    zTViewportData vpdata;

    void* targetView;
    XMFLOAT4X4 camMatrix;
    XMFLOAT4X4 camMatrixInv;

    int tremorToggle;
    float tremorScale;
    float3 tremorAmplitude;
    float3 tremorOrigin;
    float tremorVelo;

    // Transformation matrices
    XMFLOAT4X4 trafoView;
    XMFLOAT4X4 trafoViewInv;
    XMFLOAT4X4 trafoWorld;
    zCMatrixStack<XMFLOAT4X4, 8> trafoViewStack;
    zCMatrixStack<XMFLOAT4X4, 8> trafoWorldStack;
    zCMatrixStack<XMFLOAT4X4, 8> trafoWorldViewStack;
    XMFLOAT4X4 trafoProjection;
private:
};
