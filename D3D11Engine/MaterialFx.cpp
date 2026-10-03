#include "pch.h"
#include "MaterialFx.h"
#include "Engine.h"
#include "GothicAPI.h"
#include "zCMaterial.h"

namespace {
    std::mutex g_Mutex;
    std::array<zCMaterial*, MaterialFx::kMaxSlots> g_SlotMaterial = {};   // slot 0 = "none", never handed out
    std::unordered_map<zCMaterial*, uint32_t> g_MaterialSlot;
    std::array<MaterialFx::Entry, MaterialFx::kMaxSlots> g_Uploaded = {};   // what the GPU table holds
    uint32_t g_UploadedCount = 0;
    uint32_t g_HighestSlot = 0;
    bool g_OverflowLogged = false;

    bool HasStage( zCMaterial* mat ) {
        return mat->HasTexAniMap() || ( mat->GetEnvMapEnabled() && mat->GetEnvMapStrength() > 0.0f ) || mat->GetDetailTexture();
    }

    uint32_t AcquireSlot( zCMaterial* mat ) {
        std::lock_guard lock( g_Mutex );
        if ( auto it = g_MaterialSlot.find( mat ); it != g_MaterialSlot.end() ) return it->second;
        for ( uint32_t s = 1; s < MaterialFx::kMaxSlots; ++s ) {
            if ( g_SlotMaterial[s] ) continue;
            g_SlotMaterial[s] = mat;
            g_MaterialSlot.emplace( mat, s );
            g_HighestSlot = std::max( g_HighestSlot, s );
            return s;
        }
        if ( !g_OverflowLogged ) {
            g_OverflowLogged = true;
            Logging::Wrn( "MaterialFx: more than {} materials with scroll, env map or detail texture; the rest lose them on D3D12",
                MaterialFx::kMaxSlots - 1 );
        }
        return 0;
    }
}

bool MaterialFx::GetOffset( zCMaterial* mat, float2& offset ) {
    if ( !mat || !mat->HasTexAniMap() ) return false;
    const XMFLOAT2 delta = mat->GetTexAniMapDelta();
    const double time = Engine::GAPI->GetTotalTime();
    const double u = delta.x * time;
    const double v = delta.y * time;
    offset = float2( static_cast<float>( u - std::floor( u ) ), static_cast<float>( v - std::floor( v ) ) );
    return true;
}

float MaterialFx::GetEnvStrength( zCMaterial* mat ) {
    if ( !mat || !Engine::GAPI->GetRendererState().RendererSettings.EnvMapping || !mat->GetEnvMapEnabled() ) return 0.0f;
    const float strength = std::max( mat->GetEnvMapStrength(), 0.0f );
    return mat->GetMatGroup() == zMAT_GROUP_WATER ? -strength : strength;   // ZenGin adds the water env stage
}

float MaterialFx::GetEnvGlobal() {
    const auto& s = Engine::GAPI->GetRendererState().RendererSettings;
    return s.EnvMapping ? Engine::GAPI->GetSkyLightIntensity() * s.EnvMappingStrength : 0.0f;
}

zCTexture* MaterialFx::GetDetailTexture( zCMaterial* mat, float& scale ) {
    if ( !mat || !Engine::GAPI->GetRendererState().RendererSettings.DetailTextures ) return nullptr;
    zCTexture* tex = mat->GetDetailTexture();
    scale = mat->GetDetailTextureScale();
    return tex && std::isfinite( scale ) && scale > 0.0f ? tex : nullptr;
}

uint32_t MaterialFx::PackDiffuseIndex( uint32_t diffuseSlot, zCMaterial* mat ) {
    if ( !mat || diffuseSlot > kDiffuseSlotMask || !HasStage( mat ) ) return diffuseSlot;
    return diffuseSlot | ( AcquireSlot( mat ) << kSlotShift );
}

bool MaterialFx::FillTable( Entry* table, uint32_t& count, uint32_t envCubeSlot, const std::function<uint32_t( zCTexture* )>& resolveTexture ) {
    std::lock_guard lock( g_Mutex );
    count = g_MaterialSlot.empty() ? 1u : g_HighestSlot + 1u;
    const float envGlobal = GetEnvGlobal();
    table[0] = {};
    table[0].EnvCubeSlot = envGlobal > 0.0f ? envCubeSlot : 0xFFFFFFFFu;
    table[0].EnvGlobal = envGlobal;
    for ( uint32_t s = 1; s < count; ++s ) {
        Entry& e = table[s];
        e = {};
        zCMaterial* mat = g_SlotMaterial[s];
        if ( !mat ) continue;
        float2 offset;
        if ( GetOffset( mat, offset ) ) { e.ScrollU = offset.x; e.ScrollV = offset.y; }
        e.EnvStrength = GetEnvStrength( mat );
        float scale;
        if ( zCTexture* detail = GetDetailTexture( mat, scale ) ) {
            e.DetailSlot = resolveTexture( detail );
            e.DetailScale = e.DetailSlot != UINT32_MAX ? scale : 0.0f;
        }
    }
    if ( count == g_UploadedCount && memcmp( table, g_Uploaded.data(), count * sizeof( Entry ) ) == 0 ) return false;
    memcpy( g_Uploaded.data(), table, count * sizeof( Entry ) );
    g_UploadedCount = count;
    return true;
}

void MaterialFx::OnMaterialDeleted( zCMaterial* mat ) {
    std::lock_guard lock( g_Mutex );
    if ( auto it = g_MaterialSlot.find( mat ); it != g_MaterialSlot.end() ) {
        g_SlotMaterial[it->second] = nullptr;
        g_MaterialSlot.erase( it );
    }
}
