#pragma once
#include <tracy/public/tracy/TracyD3D12.hpp>
#include "D3D12Rhi.h"

inline TracyD3D12Ctx s_tracyD3D12Ctx = nullptr;
#ifdef TRACY_ENABLE
// Only D3D12 creates the GPU context; on Vulkan the zones stay inactive and never touch the list.
#define TracyD3D12ZoneNX( cmdList, name ) TracyD3D12NamedZone( s_tracyD3D12Ctx, TracyD3D12UnnamedZone, \
    s_tracyD3D12Ctx ? D3D12Rhi::Native( cmdList ) : nullptr, name, s_tracyD3D12Ctx != nullptr );
#define TracyD3D12ZoneCGX( cmdList, name ) TracyD3D12ZoneNX( cmdList, name ) ZoneScopedN( name );

#define TracyD3D12CollectHere if ( s_tracyD3D12Ctx ) { TracyD3D12Collect( s_tracyD3D12Ctx ) }
#define TracyD3D12BeginFrame TracyD3D12NewFrame( s_tracyD3D12Ctx )

#define ZoneTextStatic(text) ZoneText(text, std::size(text) - 1)

#else

#define TracyD3D12ZoneNX( cmdList, name ) (void)0;
#define TracyD3D12ZoneCGX( cmdList, name ) (void)0;

#define TracyD3D12CollectHere (void)0;
#define TracyD3D12BeginFrame (void)0

#define ZoneTextStatic(text) (void)0;

#endif
