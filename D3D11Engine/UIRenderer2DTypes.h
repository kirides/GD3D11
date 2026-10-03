#pragma once
#include <cstdint>
#include <span>
#include <DirectXMath.h>

class GfxTexture;
class zCMaterial;
struct MeshInfo;
struct SkeletalMeshInfo;

/** One 2D UI vertex in Gothic UI pixels (before GothicUIScale). Layout shared by VS_UI2D.hlsl and D3D12/UI2D.hlsl. */
struct UIVertex2D {
    float X, Y;
    float U, V;
    uint32_t Color;    // Gothic BGRA
    uint32_t Params;   // UIVertexParams bits
};

namespace UIVertexParams {
    constexpr uint32_t TextureIndexMask = 0x000FFFFF;   // bindless SRV slot (D3D12 only)
    constexpr uint32_t Linear = 1u << 20;
    constexpr uint32_t Wrap = 1u << 21;
    constexpr uint32_t ModeShift = 22;
    constexpr uint32_t ModeBlend = 0u << ModeShift;     // (rgb*a, a)
    constexpr uint32_t ModeAdd = 1u << ModeShift;       // (rgb*a, 0)
    constexpr uint32_t ModeOpaque = 2u << ModeShift;    // (rgb, 1)
    constexpr uint32_t ModeTexOnly = 3u << ModeShift;   // texture rgb only, for the MUL blends
    constexpr uint32_t IgnoreTexAlpha = 1u << 24;       // zRND_ALPHA_SOURCE_CONSTANT: alpha from the vertex only
    constexpr uint32_t Untextured = 1u << 25;
}

/** Blend state of a batch. BLEND, ADD and opaque all share the premultiplied state. */
enum class EUIBlend2D : uint8_t {
    Premultiplied,  // ONE / INV_SRC_ALPHA
    Mul,            // DEST_COLOR / ZERO
    Mul2,           // DEST_COLOR / SRC_COLOR
};

struct UIBatch2D {
    GfxTexture* Texture = nullptr;   // bound per batch when the backend has no bindless index
    uint32_t FirstVertex = 0;
    uint32_t VertexCount = 0;
    EUIBlend2D Blend = EUIBlend2D::Premultiplied;
    bool Items = false;              // inventory item previews: FirstItem/ItemCount instead of vertices
    uint32_t FirstItem = 0;
    uint32_t ItemCount = 0;
    float MinX = 0, MinY = 0, MaxX = 0, MaxY = 0;
};

/** One placement of item geometry: object space to the slot's clip space (column vectors), plus the slot. */
struct UIItemInstance {
    DirectX::XMFLOAT4X4 ClipFromObject;
    float RectX, RectY, RectW, RectH;   // UI pixels
};

/** One sub-mesh draw of an item preview. Exactly one of Mesh/SkinnedMesh is set. */
struct UIItemDraw {
    const MeshInfo* Mesh = nullptr;
    const SkeletalMeshInfo* SkinnedMesh = nullptr;
    GfxTexture* Texture = nullptr;
    zCMaterial* Material = nullptr;   // for its texAniMap scroll
    uint32_t Instance = 0;
    uint32_t BoneOffset = 0;   // SkinnedMesh only, into UIItemFrame::Bones
    uint32_t BoneCount = 0;
};

/** One recorded item preview: a contiguous run of draws inside one slot rect. */
struct UIItemPreview {
    uint32_t FirstDraw = 0;
    uint32_t DrawCount = 0;
    float RectX = 0, RectY = 0, RectW = 0, RectH = 0;   // UI pixels
};

/** Item data handed to DrawUI2D; item batches index Items by FirstItem/ItemCount. */
struct UIItemFrame {
    std::span<const UIItemPreview> Items;
    std::span<const UIItemDraw> Draws;
    std::span<const UIItemInstance> Instances;
    std::span<const DirectX::XMFLOAT4X4> Bones;
};

/** Maps slot NDC into target NDC: x' = x*sx + w*ox, y' = y*sy + w*oy. out = { sx, sy, ox, oy }. */
inline void ComputeUIItemRemap( const UIItemInstance& instance, float targetWidth, float targetHeight, float uiScale, float out[4] ) {
    const float x = instance.RectX * uiScale, y = instance.RectY * uiScale;
    const float w = instance.RectW * uiScale, h = instance.RectH * uiScale;
    out[0] = w / targetWidth;
    out[1] = h / targetHeight;
    out[2] = (2.0f * x + w) / targetWidth - 1.0f;
    out[3] = 1.0f - (2.0f * y + h) / targetHeight;
}

/** The zCRnd_D3D::xd3d_actStatus fields zCRnd_D3D::DrawPolySimple reads. */
struct UIPolygonState {
    int AlphaFunc = 0;               // zTRnd_AlphaBlendFunc
    bool AlphaSourceConstant = false;
    float AlphaFactor = 1.0f;
    bool Bilinear = true;
};
