#pragma once
#include <tracy/public/tracy/TracyD3D12.hpp>

inline TracyD3D12Ctx s_tracyD3D12Ctx = nullptr;
#define TracyD3D12ZoneNX( cmdList, name ) TracyD3D12Zone(s_tracyD3D12Ctx, cmdList, name);
#define TracyD3D12ZoneCGX( cmdList, name ) TracyD3D12Zone(s_tracyD3D12Ctx, cmdList, name); ZoneScopedN( name );

#define TracyD3D12CollectHere() TracyD3D12Collect( s_tracyD3D12Ctx )
#define TracyD3D12BeginFrame() TracyD3D12NewFrame( s_tracyD3D12Ctx )

#define ZoneTextStatic(text) ZoneText(text, std::size(text))
