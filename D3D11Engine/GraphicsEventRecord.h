#pragma once
#include "widenarrow.h"

struct ID3DUserDefinedAnnotation;

class GraphicsEventRecord {
public:
    GraphicsEventRecord() = default;

    GraphicsEventRecord( ID3DUserDefinedAnnotation* userAnnotation, const WideNarrowChars& region );

    ~GraphicsEventRecord() {
        End();
    }

    GraphicsEventRecord( const GraphicsEventRecord& ) = delete;
    GraphicsEventRecord& operator=( const GraphicsEventRecord& ) = delete;
    GraphicsEventRecord( GraphicsEventRecord&& other ) noexcept = delete;
    GraphicsEventRecord& operator=( GraphicsEventRecord&& other ) noexcept = delete;

private:
    void End();

    ID3DUserDefinedAnnotation* m_Annotation = nullptr;
};
