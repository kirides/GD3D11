#pragma once
#include <string>
class GraphicsShader
{
public:
    GraphicsShader() = default;
    virtual ~GraphicsShader() = default;

    virtual int32_t GetInputIndex( std::string_view name ) = 0;
};

