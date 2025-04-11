#pragma once
#include "pch.h"
#include "HookedFunctions.h"
#include "Engine.h"
#include "GothicAPI.h"
#include "BaseGraphicsEngine.h"
#include "zCTree.h"
#include "zCCamera.h"
#include "zCVob.h"
#include "zCCamera.h"
#include "zCSkyController_Outdoor.h"
#include "D3D11GraphicsEngineBase.h"

class zCCamera;
class zCSkyController_Outdoor;
class zCSkyController;

class zCWorld {
public:
    /** Hooks the functions of this Class */
    static void Hook() {
        DetourAttach( &reinterpret_cast<PVOID&>(HookedFunctions::OriginalFunctions.original_zCWorldRender), hooked_Render );
        DetourAttach( &reinterpret_cast<PVOID&>(HookedFunctions::OriginalFunctions.original_zCWorldVobAddedToWorld), hooked_VobAddedToWorld );

        DetourAttach( &reinterpret_cast<PVOID&>(HookedFunctions::OriginalFunctions.original_zCWorldLoadWorld), hooked_LoadWorld );
        DetourAttach( &reinterpret_cast<PVOID&>(HookedFunctions::OriginalFunctions.original_zCWorldVobRemovedFromWorld), hooked_zCWorldVobRemovedFromWorld );
        DetourAttach( &reinterpret_cast<PVOID&>(HookedFunctions::OriginalFunctions.original_zCWorldDisposeWorld), hooked_zCWorldDisposeWorld );
        DetourAttach( &reinterpret_cast<PVOID&>(HookedFunctions::OriginalFunctions.original_zCWorldDisposeVobs), hooked_zCWorldDisposeVobs );

        DetourAttach( &reinterpret_cast<PVOID&>(HookedFunctions::OriginalFunctions.original_oCWorldRemoveFromLists), hooked_oCWorldRemoveFromLists );
        DetourAttach( &reinterpret_cast<PVOID&>(HookedFunctions::OriginalFunctions.original_oCWorldEnableVob), hooked_oCWorldEnableVob );
        DetourAttach( &reinterpret_cast<PVOID&>(HookedFunctions::OriginalFunctions.original_oCWorldDisableVob), hooked_oCWorldDisableVob );
        DetourAttach( &reinterpret_cast<PVOID&>(HookedFunctions::OriginalFunctions.original_oCWorldRemoveVob), hooked_oCWorldRemoveVob );
    }

    static void __fastcall hooked_oCWorldEnableVob( zCWorld* thisptr, void* unknwn, zCVob* vob, zCVob* parent ) {
        hook_infunc

            HookedFunctions::OriginalFunctions.original_oCWorldEnableVob( thisptr, vob, parent );

        // Re-Add it
        Engine::GAPI->OnAddVob( vob, thisptr );

        hook_outfunc
    }

    static void __fastcall hooked_oCWorldDisableVob( zCWorld* thisptr, void* unknwn, zCVob* vob ) {
        hook_infunc

            // Remove it
            Engine::GAPI->OnRemovedVob( vob, thisptr );

        HookedFunctions::OriginalFunctions.original_oCWorldDisableVob( thisptr, vob );
        hook_outfunc
    }

    static void __fastcall hooked_oCWorldRemoveVob( void* thisptr, void* unknwn, zCVob* vob ) {
        hook_infunc
            //Engine::GAPI->SetCanClearVobsByVisual(); // TODO: #8
            HookedFunctions::OriginalFunctions.original_oCWorldRemoveVob( thisptr, vob );
        //Engine::GAPI->SetCanClearVobsByVisual(false); // TODO: #8
        hook_outfunc
    }

    static void __fastcall hooked_oCWorldRemoveFromLists( zCWorld* thisptr, zCVob* vob ) {
        hook_infunc

            // Remove it
            Engine::GAPI->OnRemovedVob( vob, thisptr );

        HookedFunctions::OriginalFunctions.original_oCWorldRemoveFromLists( thisptr, vob );
        hook_outfunc
    }

    
    static void __fastcall hooked_zCWorldDisposeWorld( void* thisptr, void* vtbl ) {
        //hook_infunc
        if ( thisptr == Engine::GAPI->GetLoadedWorldInfo()->MainWorld )
        { 
            // Have to reset everything on dispose due to race conditions on loading
            D3D11GraphicsEngineBase* e = reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine);
            e->SetDefaultStates();
            Engine::GAPI->ResetWorld();
            Engine::GAPI->ResetMaterialInfo();
            Engine::GAPI->ResetRenderStates();
            Engine::RefreshWorkerThreadpool(); // Make sure worker thread don't work on any point light

            LogInfo() << "World was disposed, reseting pointlight ThreadPool";
        }

        HookedFunctions::OriginalFunctions.original_zCWorldDisposeWorld( thisptr );
        //hook_outfunc
    }
   

    static void __fastcall hooked_zCWorldDisposeVobs( zCWorld* thisptr, void* unknwn, zCTree<zCVob>* tree ) {
        // Reset only if this is the main world, inventory worlds are handled differently
        if ( thisptr && thisptr == Engine::GAPI->GetLoadedWorldInfo()->MainWorld )
            Engine::GAPI->ResetVobs();

        HookedFunctions::OriginalFunctions.original_zCWorldDisposeVobs( thisptr, tree );
    }

    static void __fastcall hooked_zCWorldVobRemovedFromWorld( zCWorld* thisptr, void* unknwn, zCVob* vob ) {
        hook_infunc
            // Remove it first, before it becomes invalid
            Engine::GAPI->OnRemovedVob( vob, thisptr );

        HookedFunctions::OriginalFunctions.original_zCWorldVobRemovedFromWorld( thisptr, vob );
        hook_outfunc
    }

    static void __fastcall hooked_LoadWorld( zCWorld* thisptr, void* unknwn, const zSTRING& fileName, const int loadMode ) {
        Engine::GAPI->OnLoadWorld( fileName.ToChar(), loadMode );

        HookedFunctions::OriginalFunctions.original_zCWorldLoadWorld( thisptr, fileName, loadMode );

        Engine::GAPI->GetLoadedWorldInfo()->MainWorld = thisptr;
    }

    static void __fastcall hooked_VobAddedToWorld( zCWorld* thisptr, void* unknwn, zCVob* vob ) {
        hook_infunc

            HookedFunctions::OriginalFunctions.original_zCWorldVobAddedToWorld( thisptr, vob );

        if ( vob->GetVisual() ) {
            //LogInfo() << vob->GetVisual()->GetFileExtension(0);
            Engine::GAPI->OnAddVob( vob, thisptr );
        }
        hook_outfunc
    }

    // Get around C2712
    static void Do_hooked_Render( zCWorld* thisptr, zCCamera& camera ) {
        Engine::GAPI->SetTextureTestBindMode( false, "" );
        if ( !thisptr )
            return;

        auto MainWorld = Engine::GAPI->GetLoadedWorldInfo()->MainWorld;

        if (MainWorld && thisptr == MainWorld ) { // Main world
            Engine::GAPI->OnWorldUpdate();
           
            if ( Engine::GAPI->GetRendererState().RendererSettings.AtmosphericScattering ) {
                HookedFunctions::OriginalFunctions.original_zCWorldRender( thisptr, camera );
            } else {
                camera.SetFarPlane( 25000.0f );
                HookedFunctions::OriginalFunctions.original_zCWorldRender( thisptr, camera );
            }

            /*zCWorld* w = (zCWorld *)thisptr;
            zCSkyController* sky = w->GetActiveSkyController();
            sky->RenderSkyPre();*/
        } else { // Inventory virtual world
            // Bind matrices
            //camera.UpdateViewport();
            //camera.Activate();

            // This needs to be called to init the camera and everything for the inventory vobs
            // The PresentPending-Guard will stop the renderer from rendering the world into one of the cells here
            // TODO: This can be implemented better.
            HookedFunctions::OriginalFunctions.original_zCWorldRender( thisptr, camera );

            // Inventory
            Engine::GAPI->DrawInventory( thisptr, camera );
        }
    }

    static void __fastcall hooked_Render( zCWorld* thisptr, void* unknwn, zCCamera& camera ) {
        hook_infunc
            Do_hooked_Render( thisptr, camera );
        hook_outfunc
    }

    zCTree<zCVob>* GetGlobalVobTree() {
        return reinterpret_cast<zCTree<zCVob>*>(THISPTR_OFFSET( GothicMemoryLocations::zCWorld::Offset_GlobalVobTree ));
    }

    void Render( zCCamera& camera ) {
        reinterpret_cast<void( __fastcall* )( zCWorld*, int, zCCamera& )>( GothicMemoryLocations::zCWorld::Render )( this, 0, camera );
    }

    zCSkyController_Outdoor* GetSkyControllerOutdoor() {
        return *reinterpret_cast<zCSkyController_Outdoor**>(THISPTR_OFFSET( GothicMemoryLocations::zCWorld::Offset_SkyControllerOutdoor ));
    }

    zCBspTree* GetBspTree() {
        return reinterpret_cast<zCBspTree*>(THISPTR_OFFSET( GothicMemoryLocations::zCWorld::Offset_BspTree ));
    }

    void RemoveVob( zCVob* vob ) {
        reinterpret_cast<void( __fastcall* )( zCWorld*, int, zCVob* )>( GothicMemoryLocations::zCWorld::RemoveVob )( this, 0, vob );
    }
};
