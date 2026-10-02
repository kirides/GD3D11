#pragma once
#include <map>
#include <memory>

class GfxTexture;
struct WorldMeshSectionInfo;

/** World-space shoreline field, baked from the world mesh at world load. Per texel (RGBA16F): signed distance to
    the nearest shoreline in world units (positive over water), water depth, and the seaward direction, whose
    length drops where two shores meet. Drives the shore waves and foam in WaterShading.hlsl. */
class ShoreField {
public:
    ~ShoreField();

    /** Null when the world has no flat water near land, or the bake fails. */
    static std::unique_ptr<ShoreField> Bake( const std::map<int, std::map<int, WorldMeshSectionInfo>>& sections );

    GfxTexture* GetTexture() const { return Texture.get(); }

    /** xy = world xz of the field's corner, zw = 1 / its extent: uv = ( xz - xy ) * zw. */
    const DirectX::XMFLOAT4& GetMapping() const { return Mapping; }

private:
    std::unique_ptr<GfxTexture> Texture;
    DirectX::XMFLOAT4 Mapping = {};
};
