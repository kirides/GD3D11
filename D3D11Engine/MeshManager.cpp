#include "MeshManager.h"
#include "BaseGraphicsEngine.h"
#include "zCProgMeshProto.h"
#include "zCMeshSoftSkin.h"

MeshManager* s_MeshManager = new MeshManager();

MeshManager::MeshManager() {
    m_skeletalMeshInfo = {};
    m_staticMeshInfo = {};
}


