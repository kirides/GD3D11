#pragma once

#include "GothicAPI.h"
#include "HookedFunctions.h"
#include "zCArray.h"
#include "zCObject.h"
#include "zCPolygon.h"
#include "zSTRING.h"

enum EVisualCamAlignType {
    zVISUAL_CAM_ALIGN_NONE = 0,
    zVISUAL_CAM_ALIGN_YAW = 1,
    zVISUAL_CAM_ALIGN_FULL = 2
};

class zCBspLeaf;
class zCVisual;
class zCWorld;

class zCVob {
public:
    /** Hooks the functions of this Class */
    static void Hook() {
        XHook( HookedFunctions::OriginalFunctions.original_zCVobSetVisual, GothicMemoryLocations::zCVob::SetVisual, zCVob::Hooked_SetVisual );
        XHook( HookedFunctions::OriginalFunctions.original_zCVobDestructor, GothicMemoryLocations::zCVob::Destructor, zCVob::Hooked_Destructor );

        XHook( HookedFunctions::OriginalFunctions.original_zCVobEndMovement, GothicMemoryLocations::zCVob::EndMovement, zCVob::Hooked_EndMovement );
    }

    /** Called when this vob got it's world-matrix changed */
#ifdef BUILD_GOTHIC_1_08k
    static void __fastcall Hooked_EndMovement( void* thisptr, void* unknwn ) {
        hook_infunc

            HookedFunctions::OriginalFunctions.original_zCVobEndMovement( thisptr );

        if ( Engine::GAPI )
            Engine::GAPI->OnVobMoved( (zCVob*)thisptr );

        hook_outfunc
    }
#else
    static void __fastcall Hooked_EndMovement( void* thisptr, void* unknwn, int transformChanged ) // G2 has one parameter more
    {
        hook_infunc

            HookedFunctions::OriginalFunctions.original_zCVobEndMovement( thisptr, transformChanged );

        if ( Engine::GAPI && transformChanged )
            Engine::GAPI->OnVobMoved( (zCVob*)thisptr );

        hook_outfunc
    }
#endif

    /** Called on destruction */
    static void __fastcall Hooked_Destructor( void* thisptr, void* unknwn ) {
        hook_infunc

            // Notify the world. We are doing this here for safety so nothing possibly deleted remains in our world.
            if ( Engine::GAPI )
                Engine::GAPI->OnRemovedVob( (zCVob*)thisptr, ((zCVob*)thisptr)->GetHomeWorld() );

        HookedFunctions::OriginalFunctions.original_zCVobDestructor( thisptr );

        hook_outfunc
    }

    /** Called when this vob is about to change the visual */
    static void __fastcall Hooked_SetVisual( void* thisptr, void* unknwn, zCVisual* visual ) {
        hook_infunc

            HookedFunctions::OriginalFunctions.original_zCVobSetVisual( thisptr, visual );

        // Notify the world
        if ( Engine::GAPI )
            Engine::GAPI->OnSetVisual( (zCVob*)thisptr );

        hook_outfunc
    }

#if defined BUILD_SPACER || defined BUILD_SPACER_NET
    /** Returns the helper-visual for this class
        This actually uses a map to lookup the visual. Beware for performance-issues! */
    zCVisual* GetClassHelperVisual() {
        XCALL( GothicMemoryLocations::zCVob::GetClassHelperVisual );
    }

    /** Returns the visual saved in this vob */
    zCVisual* GetVisual() {
        zCVisual* visual = GetMainVisual();

        if ( !visual )
            visual = GetClassHelperVisual();

        return visual;
    }
#else
    /** Returns the visual saved in this vob */
    zCVisual* GetVisual() {
        return GetMainVisual();
    }
#endif

#ifdef BUILD_GOTHIC_1_08k
    void _EndMovement() {
        XCALL( GothicMemoryLocations::zCVob::EndMovement );
    }
#else
    void _EndMovement( int p = 1 ) {
        XCALL( GothicMemoryLocations::zCVob::EndMovement );
    }
#endif

    /** Updates the vobs transforms */
    void EndMovement() {
        _EndMovement();
    }

    /** Returns the visual saved in this vob */
    zCVisual* GetMainVisual() {
        XCALL( GothicMemoryLocations::zCVob::GetVisual );
    }

    /** Returns the name of this vob */
    std::string GetName() {
        return __GetObjectName().ToChar();
    }

    /** Returns the world-position of this vob */
    DirectX::XMFLOAT3 GetPositionWorld() const {
        // Get the data right off the memory to save a function call
        return DirectX::XMFLOAT3( *(float*)THISPTR_OFFSET( GothicMemoryLocations::zCVob::Offset_WorldPosX ),
            *(float*)THISPTR_OFFSET( GothicMemoryLocations::zCVob::Offset_WorldPosY ),
            *(float*)THISPTR_OFFSET( GothicMemoryLocations::zCVob::Offset_WorldPosZ ) );
        //XCALL(GothicMemoryLocations::zCVob::GetPositionWorld);
    }

    /** Returns the world-position of this vob */
    DirectX::FXMVECTOR XM_CALLCONV GetPositionWorldXM() const {
        // Get the data right off the memory to save a function call
        DirectX::FXMVECTOR pos = DirectX::XMVectorSet( *(float*)THISPTR_OFFSET( GothicMemoryLocations::zCVob::Offset_WorldPosX ),
            *(float*)THISPTR_OFFSET( GothicMemoryLocations::zCVob::Offset_WorldPosY ),
            *(float*)THISPTR_OFFSET( GothicMemoryLocations::zCVob::Offset_WorldPosZ ), 0 );
        return pos;
        //XCALL(GothicMemoryLocations::zCVob::GetPositionWorld);
    }

    /** Sets this vobs position */
    void SetPositionWorld( const DirectX::XMFLOAT3& v ) {
#ifdef BUILD_SPACER
        XCALL( GothicMemoryLocations::zCVob::SetPositionWorld );
#endif
    }
    /** Sets this vobs position */
    void SetPositionWorldDX( const DirectX::XMFLOAT3& v ) {
#ifdef BUILD_SPACER
        XCALL( GothicMemoryLocations::zCVob::SetPositionWorld );
#endif
    }
    /** Sets this vobs position */
    void XM_CALLCONV SetPositionWorldXM( DirectX::FXMVECTOR v ) {
        DirectX::XMFLOAT3 store; DirectX::XMStoreFloat3( &store, v );
        SetPositionWorldDX( store );
    }

    /** Returns the local bounding box */
    zTBBox3D GetBBoxLocal() {
        XCALL( GothicMemoryLocations::zCVob::GetBBoxLocal );
    }

    /** Returns a pointer to this vobs world-matrix */
    DirectX::XMFLOAT4X4* GetWorldMatrixPtr() {
        return (DirectX::XMFLOAT4X4*)(this + GothicMemoryLocations::zCVob::Offset_WorldMatrixPtr);
    }

    /** Copys the world matrix into the given memory location */
    void GetWorldMatrix( DirectX::XMFLOAT4X4* m ) {
        *m = *GetWorldMatrixPtr();
    }

    /** Returns a copy of the world matrix */
    DirectX::XMMATRIX GetWorldMatrixXM() {
        return XMLoadFloat4x4( (DirectX::XMFLOAT4X4*)(this + GothicMemoryLocations::zCVob::Offset_WorldMatrixPtr) );
    }

    /** Returns the world-polygon right under this vob */
    zCPolygon* GetGroundPoly() {
        return *(zCPolygon**)THISPTR_OFFSET( GothicMemoryLocations::zCVob::Offset_GroundPoly );
    }

    /** Returns whether this vob is currently in an indoor-location or not */
    bool IsIndoorVob() {
        if ( !GetGroundPoly() )
            return false;

        return GetGroundPoly()->GetLightmap() != nullptr;
    }

    /** Returns the world this vob resists in */
    zCWorld* GetHomeWorld() {
        return *(zCWorld**)THISPTR_OFFSET( GothicMemoryLocations::zCVob::Offset_HomeWorld );
    }

    /** Returns whether this vob is currently in sleeping state or not. Sleeping state is something like a waiting (cached out) NPC */
    int GetSleepingMode() {
        unsigned int flags = *(unsigned int*)THISPTR_OFFSET( GothicMemoryLocations::zCVob::Offset_SleepingMode );

        return (flags & GothicMemoryLocations::zCVob::MASK_SkeepingMode);
    }
    void SetSleeping( int on ) {
        XCALL( GothicMemoryLocations::zCVob::SetSleeping );
    }

#ifndef BUILD_SPACER_NET
    /** Returns whether the visual of this vob is visible */
    bool GetShowVisual() {
        //unsigned int flags = *(unsigned int*)THISPTR_OFFSET( GothicMemoryLocations::zCVob::Offset_Flags );

#ifndef BUILD_SPACER
        return GetShowMainVisual();
#else
        // Show helpers in spacer if wanted
        bool showHelpers = (*(int*)GothicMemoryLocations::zCVob::s_ShowHelperVisuals) != 0;
        return GetShowMainVisual() || showHelpers;
#endif
    }


#else
    bool GetShowVisual() {
        bool showHelpers = (*(int*)GothicMemoryLocations::zCVob::s_ShowHelperVisuals) != 0;

        if ( !showHelpers ) {
            zCVisual* visual = GetMainVisual();

            if ( !visual ) {
                visual = GetClassHelperVisual();

                if ( visual ) {
                    return false;
                }
            }
        }

        return GetShowMainVisual() || showHelpers;
    }
#endif

    /** Returns whether to show the main visual or not. Only used for the spacer */
    bool GetShowMainVisual() {
        unsigned int flags = *(unsigned int*)THISPTR_OFFSET( GothicMemoryLocations::zCVob::Offset_Flags );

        return (flags & GothicMemoryLocations::zCVob::MASK_ShowVisual) != 0;
    }

    /** Alignemt to the camera */
    EVisualCamAlignType GetAlignment() {
        unsigned int flags = *(unsigned int*)THISPTR_OFFSET( GothicMemoryLocations::zCVob::Offset_CameraAlignment );

        //.text:00601652                 shl     eax, 1Eh
        //.text:00601655                 sar     eax, 1Eh

        flags <<= GothicMemoryLocations::zCVob::SHIFTLR_CameraAlignment;
        flags >>= GothicMemoryLocations::zCVob::SHIFTLR_CameraAlignment;

        return (EVisualCamAlignType)flags;
    }

    /** Checks the inheritance chain and casts to T* if possible. Returns nullptr otherwise */
    template<class T>
    T* As() {
        zCClassDef* classDef = ((zCObject*)this)->_GetClassDef();
        if ( CheckInheritance( classDef, T::GetStaticClassDef() ) ) {
            return reinterpret_cast<T*>(this);
        }
        return nullptr;
    }
protected:

    bool CheckInheritance( const zCClassDef* def, const zCClassDef* target ) {
        while ( def ) {
            if ( def == target ) {
                return true;
            }
            def = def->baseClassDef;
        }
        return false;
    }

    zSTRING& __GetObjectName() {
        XCALL( GothicMemoryLocations::zCObject::GetObjectName );
    }


    /*void DoFrameActivity()
    {
        XCALL(GothicMemoryLocations::zCVob::DoFrameActivity);
    }*/



    /*zTBBox3D* GetBoundingBoxWS()
    {
        return (zTBBox3D *)THISPTR_OFFSET(GothicMemoryLocations::zCVob::Offset_BoundingBoxWS);
    }*/

    /** Data */
    /*zCTree<zCVob>* GlobalVobTreeNode;
    int LastTimeDrawn;
    DWORD LastTimeCollected;

    zCArray<zCBspLeaf*>	LeafList;
    DirectX::XMFLOAT4X4 WorldMatrix;
    zTBBox3D BoundingBoxWS;*/
};
