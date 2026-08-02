#include "pch.h"
#include "SharedVisualRegistry.h"
#include "WorldObjects.h"

SharedVisualRegistry* s_SharedVisualRegistry = new SharedVisualRegistry();

SharedVisualRegistry::~SharedVisualRegistry() {
    Clear();
}

MeshVisualInfo* SharedVisualRegistry::Acquire( const void* key, bool& outNeedsFill ) {
    std::scoped_lock lock( m_mutex );
    m_TotalAcquires++;

    auto it = m_Visuals.find( key );
    if ( it != m_Visuals.end() ) {
        MeshVisualInfo* mvi = it->second;
        mvi->SharedRefs++;

        // A cancelled extraction (world change / shutdown flush) leaves a finished-but-empty entry
        // behind. Without this it would stay empty forever and every vob sharing it would render an
        // invisible weapon, because the "visual changed" check just re-enters here and gets the same
        // dead object back. Hand it out for a refill instead.
        outNeedsFill = mvi->Ready.load( std::memory_order_acquire ) && !mvi->Visual;
        if ( outNeedsFill ) {
            mvi->Ready.store( false, std::memory_order_release );
        }
        return mvi;
    }

    MeshVisualInfo* mvi = new MeshVisualInfo;
    mvi->SharedKey = key;
    mvi->SharedRefs = 1;
    // Not ready until the caller has filled it. Handing out a fresh entry as "ready" would let a second
    // acquirer draw it while it is still empty (or, worse, while a worker is writing it).
    mvi->Ready.store( false, std::memory_order_release );
    m_Visuals[key] = mvi;
    m_TotalConversions++;
    m_PeakSize = std::max( m_PeakSize, m_Visuals.size() );

    outNeedsFill = true;
    return mvi;
}

void SharedVisualRegistry::Release( MeshVisualInfo* mvi ) {
    if ( !mvi ) {
        return;
    }

    {
        std::scoped_lock lock( m_mutex );
        if ( mvi->SharedRefs > 1 ) {
            mvi->SharedRefs--;
            return;
        }

        mvi->SharedRefs = 0;
        // Unregister() may already have dropped the mapping, in which case there is nothing to erase.
        if ( mvi->SharedKey ) {
            auto it = m_Visuals.find( mvi->SharedKey );
            if ( it != m_Visuals.end() && it->second == mvi ) {
                m_Visuals.erase( it );
            }
            mvi->SharedKey = nullptr;
        }
    }

    // Outside the lock: ~MeshVisualInfo blocks on any in-flight extraction job for this visual, and
    // that job has no business waiting on the registry mutex behind us.
    delete mvi;
}

void SharedVisualRegistry::Unregister( const void* key ) {
    std::scoped_lock lock( m_mutex );

    auto it = m_Visuals.find( key );
    if ( it == m_Visuals.end() ) {
        return;
    }

    it->second->SharedKey = nullptr;
    m_Visuals.erase( it );
}

void SharedVisualRegistry::Clear() {
    std::vector<MeshVisualInfo*> doomed;
    {
        std::scoped_lock lock( m_mutex );
        doomed.reserve( m_Visuals.size() );
        for ( auto const& it : m_Visuals ) {
            if ( it.second->SharedRefs != 0 ) {
                LogWarn() << "SharedVisualRegistry: dropping attachment visual '" << it.second->VisualName
                    << "' that still has " << it.second->SharedRefs << " reference(s)";
            }
            it.second->SharedKey = nullptr;
            it.second->SharedRefs = 0;
            doomed.emplace_back( it.second );
        }
        m_Visuals.clear();

        if ( m_TotalAcquires ) {
            LogInfo() << "SharedVisualRegistry: " << m_TotalAcquires << " attachment(s) served by "
                << m_TotalConversions << " converted mesh(es), peak " << m_PeakSize << " resident";
        }
        m_TotalAcquires = 0;
        m_TotalConversions = 0;
        m_PeakSize = 0;
    }

    for ( MeshVisualInfo* mvi : doomed ) {
        delete mvi;
    }
}

size_t SharedVisualRegistry::Size() const {
    std::scoped_lock lock( m_mutex );
    return m_Visuals.size();
}
