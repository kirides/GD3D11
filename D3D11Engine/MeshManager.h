#pragma once
#include "pch.h"
#include "zCProgMeshProto.h"

class MeshManager {

public:
    MeshManager() = default;

    int32_t RecordMesh( zCSubMesh* subMesh ) {
        auto it = m_meshIds.find( subMesh );
        if ( it == m_meshIds.end() ) {
            int32_t id = m_meshIds.size() + 1;
            m_meshIds[subMesh] = id;
            return id;
        }
        return it->second;
    }

    void DropCaches() {
        m_meshIds.clear();
    }

private:
    std::unordered_map<zCSubMesh*, int32_t> m_meshIds;
};
