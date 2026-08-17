#pragma once
#include "BaseLineRenderer.h"

class D3D11VertexBuffer;
class GfxVertexBuffer;
class D3D11LineRenderer :
    public BaseLineRenderer {
public:
    D3D11LineRenderer();
    ~D3D11LineRenderer() override;

    /** Flushes the cached lines */
    XRESULT Flush() override;

    XRESULT FlushScreenSpace() override;

private:
    /** Buffer to hold the lines on the GPU */
    std::unique_ptr<GfxVertexBuffer> LineBuffer;
    unsigned int LineBufferSize; // Size in elements the line buffer can hold
};

