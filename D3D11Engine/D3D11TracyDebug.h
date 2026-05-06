#pragma once
#include <tracy/public/tracy/TracyD3D11.hpp>

inline TracyD3D11Ctx s_tracyD3D11Ctx = nullptr;
#define TracyD3D11ZoneNX( name ) TracyD3D11Zone(s_tracyD3D11Ctx, name);
#define TracyD3D11ZoneCGX( name ) TracyD3D11Zone(s_tracyD3D11Ctx, name); ZoneScopedN( name );

