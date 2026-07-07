#pragma once
#include <mutex>
#include <vector>

struct MaterialHandle;
class zCMaterial;

template <typename T>
struct GenerationalVectors {
    std::vector<T> items;
    std::vector<uint16_t> generations;

    size_t size() const { return items.size(); }
    void resize(size_t size) {
        items.resize(size);
        generations.resize(size);
    }
};

class zCMaterialManager
{
private:
    GenerationalVectors<zCMaterial*> materials;
    
    MaterialHandle InsertMaterial(zCMaterial* material);

    // updates the slot and returns the new generation value.
    uint8_t UpdateSlot(uint32_t slot, zCMaterial* material);
public:
    zCMaterialManager() {
        materials.resize(512);
    }

    ~zCMaterialManager() = default;
    
    zCMaterialManager(zCMaterialManager&&) = delete;
    zCMaterialManager& operator=( zCMaterialManager&& ) = delete;

    zCMaterialManager(const zCMaterialManager&) = delete;
    zCMaterialManager& operator=( const zCMaterialManager& ) = delete;
    
    MaterialHandle FindMaterialHandle(zCMaterial* material) const;
    void DeleteMaterial(zCMaterial* material);
    void DeleteMaterial(MaterialHandle handle);
    MaterialHandle AddMaterial(zCMaterial* material);
    zCMaterial* GetMaterial(MaterialHandle handle) const;
};
