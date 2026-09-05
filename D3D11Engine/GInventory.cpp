#include "pch.h"
#include "GInventory.h"
#include "Engine.h"
#include "GothicAPI.h"
#include "zCMaterial.h"

GInventory::GInventory() {}

GInventory::~GInventory() {}

/** Called when a VOB got added to the BSP-Tree or the world */
void GInventory::OnAddVob( VobInfo* vob, zCWorld* world ) {
    StaticVobs[world].reset( vob );
    CurrentVobs[world] = { vob, false };
}

void GInventory::OnAddVob( SkeletalVobInfo* vob, zCWorld* world ) {
    SkeletalEntry& entry = SkeletalVobs[vob->Vob];
    entry.Info.reset( vob );
    entry.Visual = vob->Vob->GetVisual();
    entry.LastUsed = ++UseCounter;

    CurrentVobs[world] = { vob, true };
    TrimSkeletalCache();
}

SkeletalVobInfo* GInventory::FindSkeletal( zCVob* vob, zCWorld* world ) {
    auto it = SkeletalVobs.find( vob );
    // The visual check is what makes a recycled zCVob* address safe to key on.
    if ( it == SkeletalVobs.end() || !it->second.Info || it->second.Visual != vob->GetVisual() )
        return nullptr;

    it->second.LastUsed = ++UseCounter;
    CurrentVobs[world] = { it->second.Info.get(), true };
    return it->second.Info.get();
}

/** Called when a VOB got removed from the world */
bool GInventory::OnRemovedVob( zCVob* vob, zCWorld* world ) {
    auto it = CurrentVobs.find( world );
    if ( it == CurrentVobs.end() )
        return false;

    // The skeletal info itself stays in the cache for the next add (see FindSkeletal); only its
    // "this slot is showing an item" state goes away.
    CurrentVobs.erase( it );
    StaticVobs.erase( world );
    return true;
}

/** Drops every entry built from this visual - Gothic frees it once we return */
void GInventory::OnVisualDeleted( zCVisual* visual ) {
    for ( auto it = SkeletalVobs.begin(); it != SkeletalVobs.end(); ) {
        if ( it->second.Visual != visual ) { ++it; continue; }

        for ( auto cur = CurrentVobs.begin(); cur != CurrentVobs.end(); ) {
            cur = (cur->second.Info == it->second.Info.get()) ? CurrentVobs.erase( cur ) : std::next( cur );
        }
        it = SkeletalVobs.erase( it );
    }
}

void GInventory::TrimSkeletalCache() {
    while ( SkeletalVobs.size() > MaxCachedSkeletals ) {
        auto oldest = SkeletalVobs.end();
        for ( auto it = SkeletalVobs.begin(); it != SkeletalVobs.end(); ++it ) {
            // Never evict what a slot is currently showing - CurrentVobs borrows the pointer.
            const bool live = std::ranges::any_of( CurrentVobs,
                [&]( const auto& cur ) { return cur.second.Info == it->second.Info.get(); } );
            if ( live ) continue;

            if ( oldest == SkeletalVobs.end() || it->second.LastUsed < oldest->second.LastUsed )
                oldest = it;
        }
        if ( oldest == SkeletalVobs.end() )
            break; // Everything in there is live

        SkeletalVobs.erase( oldest );
    }
}

/** Draws the inventory for the given world */
void GInventory::DrawInventory( zCWorld* world, zCCamera& camera ) {
    auto it = CurrentVobs.find( world );
    if ( it == CurrentVobs.end() || !it->second.Info )
        return;

    if ( it->second.Skeletal ) {
        Engine::GraphicsEngine->DrawVobSingle( static_cast<SkeletalVobInfo*>(it->second.Info), camera );
    } else {
        Engine::GraphicsEngine->DrawVobSingle( static_cast<VobInfo*>(it->second.Info), camera );
    }
}
