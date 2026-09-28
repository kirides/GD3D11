#pragma once
#include "../RHI/RhiCmdList.h"
#include "D3D12Barrier.h"

// The D3D12 renderer's command lists go through the backend-neutral redundant-state filter.
using D3D12CmdList = Rhi::CmdList;
