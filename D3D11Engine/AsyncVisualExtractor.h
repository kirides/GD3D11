#pragma once
#include "pch.h"
#include "ThreadPool.h"
#include <mutex>

class zCModel;
class zCMeshSoftSkin;
struct SkeletalMeshVisualInfo;

/** Converts ZENGIN visuals into our own mesh/buffer objects on worker threads, and owns the ZENGIN
 *  references that makes safe.
 *
 *  The references are the whole point, because the obvious alternative does not work: a job cannot be
 *  waited on before the data it reads is freed. Our only notification that a visual is dying -
 *  GothicAPI::OnVisualDeleted - hangs off zCVisual's destructor, the *base* destructor, which runs
 *  last, so by the time it fires ~zCModel and ~zCProgMeshProto have already released everything the
 *  worker is reading. Holding a zCObject_AddRef for the life of the job stops refCtr reaching 0 at all,
 *  and with it stops a vob unloading and reloading within one frame from handing the recycled address
 *  back out as a different object.
 *
 *  Releasing them is always deferred, never done where a job is retired - see QueueRelease(). */
class AsyncVisualExtractor {
public:
    /** Starts the background extraction that fills 'info' from 'model'. Retires any job already
     *  attached to 'info' first. Main thread only. */
    void ExtractSkeletal( zCModel* model, SkeletalMeshVisualInfo* info );

    /** Blocks until the job attached to 'info' has finished, then retires it. Must be called before
     *  deleting 'info' - the job writes straight into it. No-op when there is no such job. */
    void WaitForSkeletal( SkeletalMeshVisualInfo* info );

    /** Retires the jobs that have already finished, so their references don't outlive them - nobody
     *  else would ever come back for a job whose result was never awaited. Never blocks. Once per
     *  frame, from the main thread. */
    void DrainFinished();

    /** Retires every job, finished or not, and flushes the release queue. World teardown only: the
     *  caller must already have dropped the SkeletalMeshVisualInfos these jobs write into. */
    void CancelAll();

    /** Queues one reference to be handed back at the next FlushReleases(). Callable from anywhere -
     *  all it does is append - which matters because releasing inline is a trap:
     *   - the destructor it may run re-enters OnVisualDeleted, which walks (and deletes out of) the
     *     very containers every caller here is in the middle of;
     *   - from WorldConverter's node-visual pruner that re-entry ends up back at the mutex the pruner
     *     is holding, which is a deadlock rather than a race. */
    void QueueRelease( void* object );

    /** Hands the queued references back to ZENGIN. Main thread, and only from a top-level point: each
     *  release can run a visual's destructor and come back around into OnVisualDeleted. */
    void FlushReleases();

private:
    /** One in-flight extraction, plus every reference it depends on.
     *
     *  SoftSkins is a snapshot and not just the model, because a live zCModel is no guarantee that the
     *  meshes it pointed at a moment ago still exist: ZENGIN refills zCModel::SoftSkinList *in place*
     *  on armor changes and mesh-lib swaps, releasing the old zCMeshSoftSkins as it goes. */
    struct PendingExtraction {
        TaskHandle<void> Task;
        zCModel* Model = nullptr;
        std::vector<zCMeshSoftSkin*> SoftSkins;
    };

    gtl::flat_hash_map<SkeletalMeshVisualInfo*, PendingExtraction> m_Pending;

    std::mutex m_ReleaseMutex;
    std::vector<void*> m_PendingReleases;
};

extern AsyncVisualExtractor* s_AsyncVisualExtractor;
