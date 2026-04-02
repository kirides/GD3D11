#pragma once
#include <string>
#include "StringID.h"

class GraphicsShader
{
public:
    GraphicsShader() = default;
    virtual ~GraphicsShader() = default;

    virtual int32_t GetInputIndex( StringID name ) = 0;
};

