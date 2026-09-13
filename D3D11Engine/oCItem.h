#pragma once
#include "pch.h"
#include "HookedFunctions.h"
#include "InventoryRenderer.h"

class oCItem {
public:
    /** Hooks the functions of this Class */
    static void Hook() {
#if GD3D11_INVENTORY_RENDERER
        DetourAttachTyped( &HookedFunctions::OriginalFunctions.original_oCItem__RenderItem, hooked_RenderItem );
#endif
    }

#if GD3D11_INVENTORY_RENDERER
    /** One inventory slot's preview. RenderItem mode records it into UIRenderer2D instead of a pseudo-world render. */
    static void __fastcall hooked_RenderItem( void* thisptr, void* unknwn, void* world, void* viewItem, float addon ) {
        if ( TryRenderItem( thisptr, viewItem, addon ) ) return;
        HookedFunctions::OriginalFunctions.original_oCItem__RenderItem( thisptr, world, viewItem, addon );
    }

private:
    // Get around C2712
    static bool TryRenderItem( void* thisptr, void* viewItem, float addon ) {
        bool handled = false;
        hook_infunc
            handled = InventoryRenderer::RenderItem( thisptr, viewItem, addon );
        hook_outfunc
        return handled;
    }
#endif
};
