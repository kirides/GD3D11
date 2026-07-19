#include "InstancingUtils.h"
#include "../WorldObjects.h"

void RenderView::Init()
{
    buckets.resize(500);
    for (auto& b : buckets ) {
        b.instances.reserve(20);
    }
}

void RenderView::Reset()
{
    for (auto& b : buckets ) {
        b.instances.clear();
    }
}
