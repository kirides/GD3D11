#include "pch.h"
#include "WaterSwim.h"
#include "HookedFunctions.h"
#include "GothicAPI.h"
#include "OceanSpectrum.h"
#include "zCPolygon.h"
#include "zCMaterial.h"

#ifndef BUILD_SPACER
namespace {
    /** zCCollObjectCharacter::zTSpatialState; the character AI reads the water surface from m_fWaterY. */
    struct SpatialState {
        float FloorY;
        float WaterY;
        float CeilingY;
        float LastFloorY;
        zCPolygon* FloorPoly;
        zCPolygon* WaterPoly;
        zCPolygon* CeilingPoly;
    };

    using FindFloorWaterCeilingFn = void( __fastcall* )(void* self, const XMFLOAT3* testLocation, SpatialState* state);
    FindFloorWaterCeilingFn s_Original = reinterpret_cast<FindFloorWaterCeilingFn>(GothicMemoryLocations::zCCollObjectCharacter::FindFloorWaterCeiling);

    void __fastcall HookedFindFloorWaterCeiling( void* self, const XMFLOAT3* testLocation, SpatialState* state ) {
        s_Original( self, testLocation, state );

        const OceanFFT::Surface& sea = OceanFFT::GetSurface();
        if ( !sea.Active() || !state || !testLocation || !state->WaterPoly ) return;
        const float depth = state->WaterY - state->FloorY;
        if ( depth <= 0.0f ) return;   // no water here

        zCMaterial* material = state->WaterPoly->GetMaterial();
        const MaterialInfo* info = material ? Engine::GAPI->GetMaterialInfoFrom( material ) : nullptr;
        if ( !info || info->MaterialType != MaterialInfo::MT_Ocean ) return;

        // The surface the player sees: calmer in the shallows, never below the floor
        const float height = sea.HeightAt( testLocation->x, testLocation->z, OceanFFT::WaveWeightForDepth( depth ) );
        state->WaterY = std::max( state->WaterY + height, state->FloorY + 1.0f );
    }
}
#endif

void WaterSwim::Hook() {
#ifndef BUILD_SPACER
    DetourAttachTyped( &s_Original, HookedFindFloorWaterCeiling );
#endif
}
