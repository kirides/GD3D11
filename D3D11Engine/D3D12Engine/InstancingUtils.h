#pragma once
#include <vector>

struct VobInstanceInfo;

struct RenderBucket
{
    std::vector<VobInstanceInfo> instances;
};
struct RenderView
{
    std::vector<RenderBucket> buckets;
    
    void Init();
    
    void Reset();
};
