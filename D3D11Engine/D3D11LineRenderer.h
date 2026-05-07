#pragma once
#include "BaseLineRenderer.h"

class D3D11VertexBuffer;
class D3D11LineRenderer :
    public BaseLineRenderer {
public:
    D3D11LineRenderer();
    ~D3D11LineRenderer() override;

    /** Adds a line to the list */
    XRESULT AddLine( const LineVertex& v1, const LineVertex& v2 ) override;

    XRESULT AddLineScreenSpace( const LineVertex& v1, const LineVertex& v2 ) override;

    /** Flushes the cached lines */
    XRESULT Flush() override;

    XRESULT FlushScreenSpace() override;

    /** Clears the line cache */
    XRESULT ClearCache() override;

private:
    /** Line cache */
    std::vector<LineVertex> LineCache;
    std::vector<LineVertex> ScreenSpaceLineCache;

    /** Buffer to hold the lines on the GPU */
    D3D11VertexBuffer* LineBuffer;
    unsigned int LineBufferSize; // Size in elements the line buffer can hold
};

