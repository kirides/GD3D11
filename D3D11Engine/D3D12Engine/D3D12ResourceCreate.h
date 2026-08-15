#pragma once
#include <D3D12MemAlloc.h>
#include "D3D12StateCache.h"

// Shared helper for creating textures that are exempt from the legacy-to-enhanced-barrier handoff
// (D3D12Barrier.cpp's BridgeLegacyResourceToCommon / BARRIER_INTEROP_INVALID_LAYOUT) entirely, rather
// than needing that bridge at all. When the device supports enhanced barriers, the resource is created
// directly in D3D12_BARRIER_LAYOUT tracking (ID3D12Device10::CreateCommittedResource3, via
// D3D12MA::Allocator::CreateResource3) instead of legacy D3D12_RESOURCE_STATES tracking -- it never has
// legacy-tracked history to hand off from, so its first TransitionBarrier()/UAVBarrier() call finds it
// already enhanced-tracked and BridgeLegacyResourceToCommon's NeedsLegacyHandoff correctly never fires
// for it. Falls back to the legacy CreateResource(D3D12_RESOURCE_STATES) when enhanced barriers are
// unavailable, same "always have a fallback" policy as every other enhanced-barrier code path here.
//
// NOT for resources that must stay on the fully-legacy barrier path on purpose (D3D12Fsr3.cpp's FFX
// interop resources -- FFX's own DX12 backend issues legacy barriers against them and expects the
// exact legacy state, see the comment in D3D12Fsr3.cpp) -- those keep calling CreateResource directly.
namespace D3D12ResourceCreate {

    /** Maps a creation-time initial D3D12_RESOURCE_STATES to the equivalent D3D12_BARRIER_LAYOUT. Covers
        only the states actually used as creation-time initial states in this backend; extend as needed
        rather than guessing at unused ones. */
    inline D3D12_BARRIER_LAYOUT ToInitialLayout( D3D12_RESOURCE_STATES state ) {
        // Combined shader-read mask (e.g. D3D12Taa.cpp's kPrevDepthReadState = PIXEL | NON_PIXEL) checked
        // first via a bit test rather than the switch below, since neither individual case label equals
        // the OR'd value.
        constexpr D3D12_RESOURCE_STATES kAnyShaderRead =
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        if ( ( state & kAnyShaderRead ) != 0 && ( state & ~kAnyShaderRead ) == 0 )
            return D3D12_BARRIER_LAYOUT_SHADER_RESOURCE;

        switch ( state ) {
            case D3D12_RESOURCE_STATE_RENDER_TARGET: return D3D12_BARRIER_LAYOUT_RENDER_TARGET;
            case D3D12_RESOURCE_STATE_DEPTH_WRITE: return D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE;
            case D3D12_RESOURCE_STATE_DEPTH_READ: return D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_READ;
            case D3D12_RESOURCE_STATE_UNORDERED_ACCESS: return D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS;
            case D3D12_RESOURCE_STATE_COPY_DEST: return D3D12_BARRIER_LAYOUT_COPY_DEST;
            case D3D12_RESOURCE_STATE_COPY_SOURCE: return D3D12_BARRIER_LAYOUT_COPY_SOURCE;
            case D3D12_RESOURCE_STATE_COMMON: return D3D12_BARRIER_LAYOUT_COMMON;
            default: return D3D12_BARRIER_LAYOUT_COMMON;   // caller should transition explicitly afterward
        }
    }

    /** D3D12_RESOURCE_DESC1 shares D3D12_RESOURCE_DESC's field layout exactly except for the trailing
        SamplerFeedbackMipRegion, which nothing in this backend uses (left zeroed). */
    inline D3D12_RESOURCE_DESC1 ToDesc1( const D3D12_RESOURCE_DESC& desc ) {
        D3D12_RESOURCE_DESC1 desc1 = {};
        std::memcpy( &desc1, &desc, sizeof( D3D12_RESOURCE_DESC ) );
        return desc1;
    }

    inline HRESULT CreateTexture( D3D12MA::Allocator* allocator, const D3D12MA::ALLOCATION_DESC& allocDesc,
        const D3D12_RESOURCE_DESC& desc, D3D12_RESOURCE_STATES legacyState, const D3D12_CLEAR_VALUE* clearValue,
        D3D12MA::Allocation** outAlloc, REFIID riid, void** outResource ) {
        if ( D3D12CmdList::EnhancedBarriersSupported() ) {
            const D3D12_RESOURCE_DESC1 desc1 = ToDesc1( desc );
            const HRESULT hr = allocator->CreateResource3( &allocDesc, &desc1, ToInitialLayout( legacyState ),
                clearValue, 0, nullptr, outAlloc, riid, outResource );
            if ( SUCCEEDED( hr ) ) return hr;
            // CreateResource3 failing despite EnhancedBarriersSupported() shouldn't happen (that flag is
            // itself gated on device capability), but fall through to the legacy call rather than fail
            // outright -- consistent with the rest of this backend's fallback policy.
        }
        return allocator->CreateResource( &allocDesc, &desc, legacyState, clearValue, outAlloc, riid, outResource );
    }

} // namespace D3D12ResourceCreate
