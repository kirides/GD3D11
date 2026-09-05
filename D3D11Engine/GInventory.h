#pragma once
#include "pch.h"
#include "zCWorld.h"
#include "WorldConverter.h"

class GInventory {
public:
    GInventory();
    ~GInventory();

    /** Called when a VOB got added to the BSP-Tree or the world. Takes ownership of the info. */
    void OnAddVob( VobInfo* vob, zCWorld* world );
    void OnAddVob( SkeletalVobInfo* vob, zCWorld* world );

    /** Returns the skeletal info already built for this vob, or null. ZenGin adds and removes the preview
        vob once per slot per frame, and node attachments are extracted on a worker thread - rebuilding the
        info every frame would mean they never get to finish (and would re-create their buffers every frame). */
    SkeletalVobInfo* FindSkeletal( zCVob* vob, zCWorld* world );

    /** Called when a VOB got removed from the world */
    bool OnRemovedVob( zCVob* vob, zCWorld* world );

    /** Drops every entry built from this visual - Gothic frees it once we return */
    void OnVisualDeleted( zCVisual* visual );

    /** Draws the inventory for the given world */
    void DrawInventory( zCWorld* world, zCCamera& camera );

private:
    /** Skeletal previews we hold on to past their removal (see FindSkeletal). Bounded, because the info
        keeps its attachments' GPU buffers alive and address space is the scarce resource here. */
    static constexpr size_t MaxCachedSkeletals = 64;

    struct SkeletalEntry {
        std::unique_ptr<SkeletalVobInfo> Info;
        zCVisual* Visual = nullptr;     // what the vob had when we built this - a recycled zCVob* is not a hit
        size_t LastUsed = 0;
    };

    struct Current {
        BaseVobInfo* Info = nullptr;    // borrowed from StaticVobs/SkeletalVobs
        bool Skeletal = false;
    };

    /** Drops the least recently used non-live entries until the cache is back within its bound. */
    void TrimSkeletalCache();

    /** Static (flattened) previews, rebuilt on every add - one per pseudo-world, as before. */
    std::map<zCWorld*, std::unique_ptr<VobInfo>> StaticVobs;
    std::map<zCVob*, SkeletalEntry> SkeletalVobs;

    /** What each pseudo-world currently holds. Only set between AddVob and RemoveVobSubtree: a retained
        skeletal entry must not be drawn into a slot that holds no item any more. */
    std::map<zCWorld*, Current> CurrentVobs;

    size_t UseCounter = 0;
};
