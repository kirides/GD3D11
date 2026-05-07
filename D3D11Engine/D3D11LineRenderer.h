#pragma once
#include "BaseLineRenderer.h"

class D3D11VertexBuffer;
class D3D11LineRenderer :
    public BaseLineRenderer {
public:
    D3D11LineRenderer();
    ~D3D11LineRenderer();

    /** Adds a line to the list */
    virtual XRESULT AddLine( const LineVertex& v1, const LineVertex& v2 );
    virtual XRESULT AddLineScreenSpace( const LineVertex& v1, const LineVertex& v2 );

    /** Flushes the cached lines */
    virtual XRESULT Flush();
    virtual XRESULT FlushScreenSpace();

    /** Clears the line cache */
    virtual XRESULT ClearCache();

private:
    /** Line cache */
    std::vector<LineVertex> LineCache;
    std::vector<LineVertex> ScreenSpaceLineCache;

    /** Buffer to hold the lines on the GPU */
    D3D11VertexBuffer* LineBuffer;
    unsigned int LineBufferSize; // Size in elements the line buffer can hold
};

