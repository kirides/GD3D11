#include "pch.h"
#include "AsyncVisualExtractor.h"
#include "Engine.h"
#include "WorldConverter.h"
#include "WorldObjects.h"
#include "zCModel.h"
#include "zCMeshSoftSkin.h"

AsyncVisualExtractor* s_AsyncVisualExtractor = new AsyncVisualExtractor();

namespace {
    /** Copies the model's softskin list and takes a reference on every entry, so the worker can walk it
        without racing the game thread refilling it. A torn zCMeshSoftSkin is not a theoretical concern:
        one being torn down still reports its old submesh count while its submesh array is already null,
        and zCProgMeshProto::GetSubmesh is plain arithmetic off that base, so it hands back a null (or
        near-null) pointer that reads as a valid submesh. */
    std::vector<zCMeshSoftSkin*> SnapshotSoftSkins( zCModel* model ) {
        std::vector<zCMeshSoftSkin*> snapshot;

        zCArray<zCMeshSoftSkin*>* list = model->GetMeshSoftSkinList();
        if ( !list )
            return snapshot;

        snapshot.reserve( list->NumInArray );
        for ( int i = 0; i < list->NumInArray; i++ ) {
            if ( zCMeshSoftSkin* s = list->Array[i] ) {
                zCObject_AddRef( s );
                snapshot.push_back( s );
            }
        }
        return snapshot;
    }
}

void AsyncVisualExtractor::ExtractSkeletal( zCModel* model, SkeletalMeshVisualInfo* info ) {
    ZoneScoped;

    // Whatever was attached to this info may be extracting from an older model - the armor-change case,
    // where oCNPC::SetVisual has already released the model only our reference is still holding up.
    WaitForSkeletal( info );

    // Ready gates every draw/update site, so nobody touches Meshes/SkeletalMeshes while we fill them.
    info->Ready = false;

    PendingExtraction pending;
    pending.Model = model;
    pending.SoftSkins = SnapshotSoftSkins( model );
    zCObject_AddRef( model );

    // Points into pending.SoftSkins, which survives the move into m_Pending: moving a vector - and
    // rehashing the map - relocates the vector object, never its heap buffer.
    const std::span<zCMeshSoftSkin* const> softSkins = pending.SoftSkins;

    // CPU vertex/weight unpacking plus GPU buffer creation is the expensive part of a vob popping into
    // range, which is why this is worth taking off the game thread at all.
    pending.Task = Engine::WorkerThreadPool->enqueue( [model, softSkins, info]( const std::stop_token& token ) {
        if ( token.stop_requested() ) {
            // Cancelled before a worker picked it up. Everything we would read is still alive - we hold
            // references - but the caller has moved on and no longer wants this extraction.
            info->Ready.store( true );
            return;
        }
        info->ClearMeshes();
        WorldConverter::ExtractSkeletalMeshFromVob( model, softSkins, info );
        info->Visual = model;
        info->Ready.store( true );
    } );

    m_Pending[info] = std::move( pending );
}

void AsyncVisualExtractor::WaitForSkeletal( SkeletalMeshVisualInfo* info ) {
    if ( !info )
        return;

    auto it = m_Pending.find( info );
    if ( it == m_Pending.end() )
        return;

    PendingExtraction pending = std::move( it->second );
    m_Pending.erase( it );

    pending.Task.cancel(); // Lets the job bail out immediately if a worker hasn't picked it up yet
    if ( pending.Task.future.valid() )
        pending.Task.future.wait();

    QueueRelease( pending.Model );
    for ( zCMeshSoftSkin* s : pending.SoftSkins )
        QueueRelease( s );
}

void AsyncVisualExtractor::DrainFinished() {
    if ( m_Pending.empty() )
        return;

    static std::vector<SkeletalMeshVisualInfo*> finished; // Main thread only, keeps its capacity
    finished.clear();

    for ( auto& it : m_Pending ) {
        if ( !it.second.Task.future.valid() ||
            it.second.Task.future.wait_for( std::chrono::seconds( 0 ) ) == std::future_status::ready ) {
            finished.push_back( it.first );
        }
    }

    // Collected first: WaitForSkeletal erases from the map we would otherwise still be iterating.
    for ( SkeletalMeshVisualInfo* info : finished )
        WaitForSkeletal( info ); // Already finished, so this doesn't block
}

void AsyncVisualExtractor::CancelAll() {
    // No waiting here - the world teardown that calls this has already flushed the thread pool, and the
    // infos these jobs write into are gone. All that is left is handing the references back.
    for ( auto& it : m_Pending ) {
        QueueRelease( it.second.Model );
        for ( zCMeshSoftSkin* s : it.second.SoftSkins )
            QueueRelease( s );
    }
    m_Pending.clear();

    FlushReleases();
}

void AsyncVisualExtractor::QueueRelease( void* object ) {
    if ( !object )
        return;

    std::scoped_lock lock( m_ReleaseMutex );
    m_PendingReleases.push_back( object );
}

void AsyncVisualExtractor::FlushReleases() {
    for ( ;; ) {
        void* object;
        {
            // The lock is dropped before the release: the destructor's OnVisualDeleted re-entry can queue
            // further releases, and this mutex is not recursive. That re-entry is also why the vector is
            // re-checked every round rather than iterated.
            std::scoped_lock lock( m_ReleaseMutex );
            if ( m_PendingReleases.empty() )
                return;

            object = m_PendingReleases.back();
            m_PendingReleases.pop_back();
        }
        zCObject_Release( object );
    }
}
