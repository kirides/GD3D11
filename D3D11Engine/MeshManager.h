#pragma once
#include "pch.h"
#include "zCProgMeshProto.h"
#include <mutex>

class MeshManager {

public:
    MeshManager() = default;

    // Called from both the main thread and background skeletal-mesh extraction jobs
    // (GothicAPI::LoadzCModelData), so the map needs to be protected.
    int32_t RecordMesh( zCSubMesh* subMesh ) {
        std::scoped_lock lock( m_mutex );
        auto it = m_meshIds.find( subMesh );
        if ( it == m_meshIds.end() ) {
            int32_t id = m_meshIds.size() + 1;
            m_meshIds[subMesh] = id;
            return id;
        }
        return it->second;
    }

    void DropCaches() {
        std::scoped_lock lock( m_mutex );
        m_meshIds.clear();
    }

private:
    std::mutex m_mutex;
    std::unordered_map<zCSubMesh*, int32_t> m_meshIds;
};
