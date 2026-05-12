#pragma once
#include "pch.h"
#include "HookedFunctions.h"
#include "Engine.h"
#include "GothicAPI.h"
#include "BaseGraphicsEngine.h"
#include "zCTree.h"
#include "zCCamera.h"
#include "zCVob.h"
#include "zCSkyController_Outdoor.h"

class zCSkyController_Outdoor;
class zCSkyController;

enum zTWorldLoadMode
{
    zWLD_LOAD_GAME_STARTUP,
    zWLD_LOAD_GAME_SAVED_DYN,
    zWLD_LOAD_GAME_SAVED_STAT,
    zWLD_LOAD_EDITOR_COMPILED,
    zWLD_LOAD_EDITOR_UNCOMPILED,
    zWLD_LOAD_MERGE
};

class zCWorld {
public:
    /** Hooks the functions of this Class */
    static void Hook() {

        if ( HookedFunctions::OriginalFunctions.original_ContainerDraw ) {
            DetourAttachTyped( &HookedFunctions::OriginalFunctions.original_ContainerDraw, hooked_ContainerDraw  );
        }

        DetourAttachTyped( &HookedFunctions::OriginalFunctions.original_zCWorldRender, hooked_Render  );
        DetourAttachTyped( &HookedFunctions::OriginalFunctions.original_zCWorldVobAddedToWorld, hooked_VobAddedToWorld  );

        DetourAttachTyped( &HookedFunctions::OriginalFunctions.original_zCWorldLoadWorld, hooked_LoadWorld  );
        DetourAttachTyped( &HookedFunctions::OriginalFunctions.original_zCWorldVobRemovedFromWorld, hooked_zCWorldVobRemovedFromWorld  );
        //DetourAttachTyped( &HookedFunctions::OriginalFunctions.original_zCWorldDisposeWorld, hooked_zCWorldDisposeWorld  );
        DetourAttachTyped( &HookedFunctions::OriginalFunctions.original_zCWorldDisposeVobs, hooked_zCWorldDisposeVobs  );

        DetourAttachTyped( &HookedFunctions::OriginalFunctions.original_oCWorldRemoveFromLists, hooked_oCWorldRemoveFromLists  );
        DetourAttachTyped( &HookedFunctions::OriginalFunctions.original_oCWorldEnableVob, hooked_oCWorldEnableVob  );
        DetourAttachTyped( &HookedFunctions::OriginalFunctions.original_oCWorldDisableVob, hooked_oCWorldDisableVob  );
        DetourAttachTyped( &HookedFunctions::OriginalFunctions.original_oCWorldRemoveVob, hooked_oCWorldRemoveVob  );

#ifdef BUILD_SPACER_NET
        DetourAttachTyped( &HookedFunctions::OriginalFunctions.original_zCWorldCompileWorld, hooked_zCWorldCompileWorld  );
        DetourAttachTyped( &HookedFunctions::OriginalFunctions.original_zCWorldGenerateStaticWorldLighting, hooked_zCWorldGenerateStaticWorldLighting  );
#endif
    }

    inline static bool isDrawingContainers = false;
    static void __cdecl hooked_ContainerDraw() {
        ZoneScoped;
        isDrawingContainers = true;

        auto _ = Engine::GraphicsEngine->RecordGraphicsEvent( GE_NAME( "Draw Inventory World" ) );

        HookedFunctions::OriginalFunctions.original_ContainerDraw();

        isDrawingContainers = false;
    }

    static void __fastcall hooked_oCWorldEnableVob( zCWorld* thisptr, void* unknwn, zCVob* vob, zCVob* parent ) {
        HookedFunctions::OriginalFunctions.original_oCWorldEnableVob( thisptr, vob, parent );

        hook_infunc

            // Re-Add it
            Engine::GAPI->OnAddVob( vob, thisptr );

        hook_outfunc
    }

    static void __fastcall hooked_oCWorldDisableVob( zCWorld* thisptr, void* unknwn, zCVob* vob ) {
        hook_infunc

            // Remove it
            Engine::GAPI->OnRemovedVob( vob, thisptr );

        hook_outfunc
            
        HookedFunctions::OriginalFunctions.original_oCWorldDisableVob( thisptr, vob );
    }

    static void __fastcall hooked_oCWorldRemoveVob( void* thisptr, void* unknwn, zCVob* vob ) {
        //Engine::GAPI->SetCanClearVobsByVisual(); // TODO: #8
        HookedFunctions::OriginalFunctions.original_oCWorldRemoveVob( thisptr, vob );
        //Engine::GAPI->SetCanClearVobsByVisual(false); // TODO: #8
    }

    static void __fastcall hooked_oCWorldRemoveFromLists( zCWorld* thisptr, zCVob* vob ) {
        hook_infunc

            // Remove it
            Engine::GAPI->OnRemovedVob( vob, thisptr );

        hook_outfunc
            
        HookedFunctions::OriginalFunctions.original_oCWorldRemoveFromLists( thisptr, vob );
    }

    /*
    static void __fastcall hooked_zCWorldDisposeWorld( void* thisptr, void* unknwn ) {
        //Engine::GAPI->ResetWorld();
        HookedFunctions::OriginalFunctions.original_zCWorldDisposeWorld( thisptr );
    }
    */

    static void __fastcall hooked_zCWorldDisposeVobs( zCWorld* thisptr, void* unknwn, zCTree<zCVob>* tree ) {
        ZoneScoped;
        // Reset only if this is the main world, inventory worlds are handled differently
        if ( thisptr == Engine::GAPI->GetLoadedWorldInfo()->MainWorld )
            Engine::GAPI->ResetVobs();

        HookedFunctions::OriginalFunctions.original_zCWorldDisposeVobs( thisptr, tree );
    }

    static void __fastcall hooked_zCWorldVobRemovedFromWorld( zCWorld* thisptr, void* unknwn, zCVob* vob ) {
        hook_infunc

            // Remove it first, before it becomes invalid
            Engine::GAPI->OnRemovedVob( vob, thisptr );

        hook_outfunc
            
        HookedFunctions::OriginalFunctions.original_zCWorldVobRemovedFromWorld( thisptr, vob );
    }

    static void __fastcall hooked_LoadWorld( zCWorld* thisptr, void* unknwn, const zSTRING& fileName, const int loadMode ) {
        ZoneScoped;
        Engine::GAPI->OnLoadWorld( fileName.ToChar(), loadMode );

        HookedFunctions::OriginalFunctions.original_zCWorldLoadWorld( thisptr, fileName, loadMode );

        Engine::GAPI->GetLoadedWorldInfo()->MainWorld = thisptr;
    }

    static void __fastcall hooked_VobAddedToWorld( zCWorld* thisptr, void* unknwn, zCVob* vob ) {
        HookedFunctions::OriginalFunctions.original_zCWorldVobAddedToWorld( thisptr, vob );

        hook_infunc

            if ( vob->GetVisual() ) {
                //LogInfo() << vob->GetVisual()->GetFileExtension(0);
                Engine::GAPI->OnAddVob( vob, thisptr );
            }

        hook_outfunc
    }

#ifdef BUILD_SPACER_NET
    static void __fastcall hooked_zCWorldCompileWorld( zCWorld* thisptr, void* unknwn, int& a2, float a3, int a4, int a5, void* a6 ) {
        HookedFunctions::OriginalFunctions.original_zCWorldCompileWorld( thisptr, a2, a3, a4, a5, a6 );

        // Make sure worker thread don't work on any point light
        Engine::RefreshWorkerThreadpool();

        LogInfo() << "Loading world!";
        Engine::GAPI->GetLoadedWorldInfo()->MainWorld = thisptr;
        Engine::GAPI->OnGeometryLoaded( thisptr->GetBspTree() );
    }

    static void __fastcall hooked_zCWorldGenerateStaticWorldLighting( zCWorld* thisptr, void* unknwn, int& a2, void* a3 ) {
        HookedFunctions::OriginalFunctions.original_zCWorldGenerateStaticWorldLighting( thisptr, a2, a3 );

        // Make sure worker thread don't work on any point light
        Engine::RefreshWorkerThreadpool();

        LogInfo() << "Loading world!";
        Engine::GAPI->GetLoadedWorldInfo()->MainWorld = thisptr;
        Engine::GAPI->OnGeometryLoaded( thisptr->GetBspTree() );
    }
#endif

    // Get around C2712
    static void Do_hooked_Render( zCWorld* thisptr, zCCamera& camera ) {
        Engine::GAPI->SetTextureTestBindMode( false, "" );

        //HookedFunctions::OriginalFunctions.original_zCWorldRender(thisptr, camera);
        if ( thisptr == Engine::GAPI->GetLoadedWorldInfo()->MainWorld ) {
            Engine::GAPI->OnWorldUpdate();
        } else {
            // Inventory
            Engine::GAPI->DrawInventory( thisptr, camera );
        }
    }

    static void __fastcall hooked_Render( zCWorld* thisptr, void* unknwn, zCCamera& camera ) {
        if ( thisptr != Engine::GAPI->GetLoadedWorldInfo()->MainWorld ) {
            // This needs to be called to init the camera and everything for the inventory vobs
            // The PresentPending-Guard will stop the renderer from rendering the world into one of the cells here
            // TODO: This can be implemented better.
            HookedFunctions::OriginalFunctions.original_zCWorldRender( thisptr, camera );
        }

        hook_infunc
            Do_hooked_Render( thisptr, camera );
        hook_outfunc

        if ( thisptr == Engine::GAPI->GetLoadedWorldInfo()->MainWorld ) {
            Engine::GAPI->GetRendererState().RendererInfo.RenderStage = STAGE_DRAW_UNKNOWN;
            if ( Engine::GAPI->GetRendererState().RendererSettings.AtmosphericScattering ) {
                HookedFunctions::OriginalFunctions.original_zCWorldRender( thisptr, camera );
            } else {
                camera.SetFarPlane( 25000.0f );
                HookedFunctions::OriginalFunctions.original_zCWorldRender( thisptr, camera );
            }
        }
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
