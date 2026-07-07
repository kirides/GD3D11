#include "zCMaterialManager.h"
#include "zCMaterial.h"

static MaterialHandle InvalidHandle = {0, 0};

MaterialHandle zCMaterialManager::FindMaterialHandle(zCMaterial* material) const {
    if (const auto it = std::ranges::find(materials.items, material); it != materials.items.end()) {
        const uint32_t index = std::distance(materials.items.begin(), it);
        const auto generation = materials.generations[index];
        return {index, static_cast<uint8_t>(generation)};
    }
    return InvalidHandle;
}

MaterialHandle zCMaterialManager::InsertMaterial(zCMaterial* material)
{
    const size_t numGen = materials.generations.size();
    for (size_t i = 0; i < numGen; ++i) {
        // free slot, eg after clear, or at the start
        if (materials.generations[i] == 0
            // free slot as material was deleted.
            || materials.items[i] == nullptr) {
            const auto generation = UpdateSlot(i, material);
            return {i, generation};
        }
    }
    // no slot, re-allocate and provide more space.
    materials.resize(materials.size()*2);

    // append it to the end of the vector
    materials.generations[numGen] = 1;
    materials.items[numGen] = material;
    return {numGen, 1};
}

uint8_t zCMaterialManager::UpdateSlot(uint32_t slot, zCMaterial* material) {
    auto generation = ++materials.generations[slot];
    if (generation > std::numeric_limits<uint8_t>::max()) {
        // wrap around to 1. Materials are likely not loaded multiple hundred times and still be in use.
        generation = 1;
    }
    materials.items[slot] = material;
    return static_cast<uint8_t>(generation);
}

void zCMaterialManager::DeleteMaterial(const MaterialHandle handle) {
    if (handle.isValid()) {
        // already registered this pointer
        const uint32_t index = handle.idxgen.index;
        UpdateSlot(index, nullptr);
    }
}

void zCMaterialManager::DeleteMaterial(zCMaterial* material) {
    if (const auto handle = FindMaterialHandle(material)) {
        DeleteMaterial(handle);
    }
}

MaterialHandle zCMaterialManager::AddMaterial(zCMaterial* material) {
    if (const auto handle = FindMaterialHandle(material); handle.idxgen.generation != 0) {
        // already registered this pointer
        const uint32_t index = handle.idxgen.index;
        const auto generation = UpdateSlot(index, material);
        return {index, generation};
    }
    
    return InsertMaterial(material);
}
    
zCMaterial* zCMaterialManager::GetMaterial(const MaterialHandle handle) const {
    const auto index = handle.idxgen.index;
    const auto generation = handle.idxgen.generation;
    if (generation == 0) {
        // invalid handle
        return nullptr;
    }
    if (generation != materials.generations[index]) {
        // handle is dead
        return nullptr;
    }
    return materials.items[index];
}
