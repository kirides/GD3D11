#pragma once
#include "pch.h"
#include <mutex>

struct MeshVisualInfo;
class zCVisual;

/** Engine-wide cache of converted node-attachment visuals, keyed by the game's own zCVisual pointer.
 *
 *  Every skeletal vob (i.e. every NPC) hangs weapons, shields, heads and held items off its model
 *  nodes. Those attachments used to be converted per *vob*: 40 orcs carrying the same axe meant 40
 *  MeshVisualInfos, 40 vertex/index/shadow-index buffer sets and 40 system-memory vertex copies of
 *  one axe. In a 32-bit address space that is exactly the allocation pattern the project bans (see
 *  CLAUDE.md, "Performance constraints") - and it also paid the wedge-unpack + meshoptimizer cost
 *  once per vob instead of once per mesh.
 *
 *  ZENGIN already caches visuals by filename, so all those orcs hand us the *same* zCVisual pointer.
 *  That pointer is therefore the natural identity key - and it is already what the draw loops compare
 *  against to notice "this node's attachment changed". Entries are refcounted: a vob acquires on
 *  extraction and releases when the attachment is swapped out or the vob dies.
 *
 *  Sharing is self-validating for morph meshes (.MMS): they only collapse when the game handed out
 *  the same zCMorphMesh, in which case they also share the morph animation state that
 *  UpdateMorphMeshVisual reads, so one shared deformed vertex buffer is what the game is describing
 *  anyway. Distinct per-NPC zCMorphMeshes keep distinct keys and simply never share.
 *
 *  All calls are main-thread today (extraction is kicked from the draw/update loops), but the mutex
 *  is kept because MeshVisualInfo filling itself runs on the worker pool and the cost here is nil -
 *  this is a per-attachment-change path, never a per-draw one.
 */
class SharedVisualRegistry {
public:
    SharedVisualRegistry() = default;
    ~SharedVisualRegistry();

    SharedVisualRegistry( const SharedVisualRegistry& ) = delete;
    SharedVisualRegistry& operator=( const SharedVisualRegistry& ) = delete;

    /** Returns the shared MeshVisualInfo for this visual, bumping its refcount.
     *
     *  'outNeedsFill' is true when the caller is responsible for actually converting the mesh into
     *  the returned (empty) object - either because it was just created, or because a previous
     *  extraction was cancelled and left it empty. When it comes back false the mesh is already
     *  built (or is being built right now by a worker, in which case MeshVisualInfo::Ready is still
     *  false and draw sites skip it as usual) and the caller must not touch it.
     *
     *  Never returns nullptr. */
    MeshVisualInfo* Acquire( zCVisual* key, bool& outNeedsFill );

    /** Drops one reference. Destroys the visual (and unregisters it) when the last one goes. */
    void Release( MeshVisualInfo* mvi );

    /** Removes the key -> visual mapping without touching refcounts. Called when Gothic frees a
     *  visual: the pointer may be recycled for something completely different, so it must not stay
     *  a valid lookup key. Attachments still holding the entry keep it alive until they release it,
     *  and their own "visual changed" check retires them on the next frame. */
    void Unregister( zCVisual* key );

    /** Drops every entry regardless of refcount. Only for world teardown, after all vobs (and hence
     *  all attachment references) are already gone. Logs anything still referenced. */
    void Clear();

    size_t Size() const;

private:
    mutable std::mutex m_mutex;
    gtl::flat_hash_map<zCVisual*, MeshVisualInfo*> m_Visuals;

    /** Lifetime tallies for the summary Clear() logs - the ratio between them is the sharing factor
        this whole class exists for (how many attachment slots one converted mesh ended up serving). */
    size_t m_TotalAcquires = 0;
    size_t m_TotalConversions = 0;
    size_t m_PeakSize = 0;
};

extern SharedVisualRegistry* s_SharedVisualRegistry;
