#pragma once

/** Inventory item previews recorded into UIRenderer2D instead of ZenGin's per-slot pseudo-world render. */
namespace InventoryRenderer {
    /** This game build has the addresses the native path needs. */
    bool IsSupported();

    /** RenderItem mode is selected and the backend can draw it. */
    bool IsActive();

    /** oCItem::RenderItem( zCWorld*, zCViewBase*, float ) replacement. False = not handled, call the original. */
    bool RenderItem( void* item, void* viewItem, float addon );
}
