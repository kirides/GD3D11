#pragma once
#include "pch.h"
#include "zCProgMeshProto.h"

class MeshManager {

public:
    MeshManager();

    std::shared_ptr<SkeletalMeshData> MeshManager::PutSkeletalData( zCSubMesh* subMesh, SkeletalMeshData* data ) {
        auto sharedData = std::shared_ptr<SkeletalMeshData>( data );
        m_skeletalMeshInfo[subMesh] = sharedData;
        return sharedData;
    }

    std::shared_ptr<SkeletalMeshData> MeshManager::GetSkeletalData( zCSubMesh* subMesh ) {
        auto f = m_skeletalMeshInfo.find( subMesh );
        if ( f != m_skeletalMeshInfo.end() ) {
            return f->second;
        }
        return nullptr;
    }

    std::shared_ptr<StaticMeshData> MeshManager::PutStaticData( zCSubMesh* subMesh, StaticMeshData* data ) {
        auto sharedData = std::shared_ptr<StaticMeshData>( data );
        m_staticMeshInfo[subMesh] = sharedData;
        return sharedData;
    }

    std::shared_ptr<StaticMeshData> MeshManager::GetStaticData( zCSubMesh* subMesh ) {
        auto f = m_staticMeshInfo.find( subMesh );
        if ( f != m_staticMeshInfo.end() ) {
            return f->second;
        }
        return nullptr;
    }

    void DropCaches() {
        m_skeletalMeshInfo.clear();
        m_staticMeshInfo.clear();
    }

private:
    std::unordered_map<zCSubMesh*, std::shared_ptr<SkeletalMeshData>> m_skeletalMeshInfo;
    std::unordered_map<zCSubMesh*, std::shared_ptr<StaticMeshData>> m_staticMeshInfo;
};
