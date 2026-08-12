#pragma once
#include "pch.h"
#include <mutex>

struct MeshVisualInfo;
class zCVisual;

/** Refcounted cache of converted node-attachment visuals, so 40 orcs carrying the same axe share one
 *  MeshVisualInfo instead of converting 40 identical copies into 40 buffer sets.
 *
 *  Keyed on the game's own visual pointer: ZENGIN caches visuals by filename, so identical attachments
 *  really do arrive as one pointer. That also makes .MMS sharing self-validating - two morph meshes
 *  only collapse when they share the zCMorphMesh whose animation state deforms them. */
class SharedVisualRegistry {
public:
    SharedVisualRegistry() = default;
    ~SharedVisualRegistry();

    SharedVisualRegistry( const SharedVisualRegistry& ) = delete;
    SharedVisualRegistry& operator=( const SharedVisualRegistry& ) = delete;

    /** Returns the shared visual, bumping its refcount. Never null. 'outNeedsFill' means the caller
        must convert the mesh into the returned (empty) object; when false it is already built, or a
        worker is still building it and Ready is false. */
    MeshVisualInfo* Acquire( const void* key, bool& outNeedsFill );

    /** Drops one reference. Destroys the visual (and unregisters it) when the last one goes. */
    void Release( MeshVisualInfo* mvi );

    /** Drops the key -> visual mapping without touching refcounts, for when Gothic frees a visual and
        the address can be recycled for something unrelated. Holders keep the entry alive. */
    void Unregister( const void* key );

    /** Drops every entry regardless of refcount. World teardown only, after all vobs are gone. */
    void Clear();

    size_t Size() const;

private:
    mutable std::mutex m_mutex;
    gtl::flat_hash_map<const void*, MeshVisualInfo*> m_Visuals;

    /** Lifetime tallies for the Clear() summary; their ratio is the sharing factor. */
    size_t m_TotalAcquires = 0;
    size_t m_TotalConversions = 0;
    size_t m_PeakSize = 0;
};

extern SharedVisualRegistry* s_SharedVisualRegistry;
