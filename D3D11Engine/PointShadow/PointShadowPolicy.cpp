#include "../pch.h"
#include "PointShadowPolicy.h"

#include "../Engine.h"
#include "../GothicAPI.h"
#include "../WorldObjects.h"
#include "../zCVobLight.h"
#include "PointShadowCasters.h"

namespace PointShadowPolicy {

    int CurrentShadowMode( const VobLightInfo* info ) {
        auto mode = static_cast<int>( Engine::GAPI->GetRendererState().RendererSettings.EnablePointlightShadows );
        // Only PLS_UPDATE_DYNAMIC downgrades a static-flagged light to PLS_STATIC_ONLY; PLS_FULL must stay
        // FULL (it's the no-caching-shortcuts escape hatch, and downgrading it would make it never
        // re-render). Skipped when PLSC_STATIC_LIGHTS opts static lights into real VOB/NPC casters - they
        // need the full dynamic-overlay machinery to actually receive one.
        if ( mode == GothicRendererSettings::PLS_UPDATE_DYNAMIC ) {
            if ( info->IsStaticVobLight && !PointShadowCasters::AllowsDynamicCasters( info ) ) {
                return GothicRendererSettings::EPointLightShadowMode::PLS_STATIC_ONLY;
            }
        }
        return mode;
    }

    bool NeedsUpdate( int shadowMode, int lastShadowMode, const LightUpdateState& state ) {
        // Report the pending mode switch, but do NOT latch it here - see the header.
        if ( shadowMode != lastShadowMode ) {
            return shadowMode > 0;
        }

        if ( shadowMode == GothicRendererSettings::PLS_STATIC_ONLY
            || shadowMode == GothicRendererSettings::PLS_UPDATE_DYNAMIC ) {
            return state.Moved || !state.StaticShadowReady || !state.DrawnOnce;
        }

        if ( shadowMode == GothicRendererSettings::PLS_FULL ) {
            // PLS_FULL always re-renders, even for a stationary light, since it never latches DrawnOnce.
            return true;
        }

        return state.Moved || !state.DrawnOnce;
    }

    bool WantsUpdate( int shadowMode, const VobLightInfo* info, DWORD lastUpdateColor ) {
        if ( shadowMode <= GothicRendererSettings::PLS_STATIC_ONLY ) {
            return false;
        }

        // If dynamic, update colorchanging lights too, because they are mostly lamps and campfires
        // They wouldn't need an update just because of the colorchange, but most of them are dominant lights so it looks better
        if ( shadowMode >= GothicRendererSettings::PLS_UPDATE_DYNAMIC )
            if ( info->Vob->GetLightColor() != lastUpdateColor )
                return true;

        return false;
    }

    bool PositionEqualEps( const XMFLOAT3& a, const XMFLOAT3& b, float eps ) {
        const XMVECTOR va = XMLoadFloat3( &a );
        const XMVECTOR vb = XMLoadFloat3( &b );
        const XMVECTOR vEps = XMVectorReplicate( eps );
        return XMVector3NearEqual( va, vb, vEps );
    }

} // namespace PointShadowPolicy
