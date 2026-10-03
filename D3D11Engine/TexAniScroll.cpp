#include "pch.h"
#include "TexAniScroll.h"
#include "Engine.h"
#include "GothicAPI.h"
#include "zCMaterial.h"

namespace {
    std::mutex g_Mutex;
    std::array<zCMaterial*, TexAniScroll::kMaxSlots> g_SlotMaterial = {};   // slot 0 = "no scroll", never handed out
    std::unordered_map<zCMaterial*, uint32_t> g_MaterialSlot;
    bool g_OverflowLogged = false;

    uint32_t AcquireSlot( zCMaterial* mat ) {
        std::lock_guard lock( g_Mutex );
        if ( auto it = g_MaterialSlot.find( mat ); it != g_MaterialSlot.end() ) return it->second;
        for ( uint32_t s = 1; s < TexAniScroll::kMaxSlots; ++s ) {
            if ( g_SlotMaterial[s] ) continue;
            g_SlotMaterial[s] = mat;
            g_MaterialSlot.emplace( mat, s );
            return s;
        }
        if ( !g_OverflowLogged ) {
            g_OverflowLogged = true;
            Logging::Wrn( "texAniMap: more than {} scrolling materials, the rest stay still on D3D12", TexAniScroll::kMaxSlots - 1 );
        }
        return 0;
    }
}

bool TexAniScroll::GetOffset( zCMaterial* mat, float2& offset ) {
    if ( !mat || !mat->HasTexAniMap() ) return false;
    const XMFLOAT2 delta = mat->GetTexAniMapDelta();
    const double time = Engine::GAPI->GetTotalTime();
    const double u = delta.x * time;
    const double v = delta.y * time;
    offset = float2( static_cast<float>( u - std::floor( u ) ), static_cast<float>( v - std::floor( v ) ) );
    return true;
}

uint32_t TexAniScroll::PackDiffuseIndex( uint32_t diffuseSlot, zCMaterial* mat ) {
    if ( !mat || !mat->HasTexAniMap() || diffuseSlot > kDiffuseSlotMask ) return diffuseSlot;
    return diffuseSlot | ( AcquireSlot( mat ) << kSlotShift );
}

bool TexAniScroll::FillTable( float4 ( &table )[kMaxSlots] ) {
    std::lock_guard lock( g_Mutex );
    if ( g_MaterialSlot.empty() ) return false;
    for ( uint32_t s = 0; s < kMaxSlots; ++s ) {
        float2 offset;
        if ( !GetOffset( g_SlotMaterial[s], offset ) ) offset = float2( 0.0f, 0.0f );
        table[s] = float4( offset.x, offset.y, 0.0f, 0.0f );
    }
    return true;
}

void TexAniScroll::OnMaterialDeleted( zCMaterial* mat ) {
    std::lock_guard lock( g_Mutex );
    if ( auto it = g_MaterialSlot.find( mat ); it != g_MaterialSlot.end() ) {
        g_SlotMaterial[it->second] = nullptr;
        g_MaterialSlot.erase( it );
    }
}
