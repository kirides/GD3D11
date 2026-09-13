#pragma once
#include "widenarrow.h"

struct ID3DUserDefinedAnnotation;

class GraphicsEventRecord {
public:
    GraphicsEventRecord() = default;

    GraphicsEventRecord( ID3DUserDefinedAnnotation* userAnnotation, const WideNarrowChars& region );

    /** An event the backend already began; end( context ) closes it. */
    GraphicsEventRecord( void* context, void (*end)( void* ) ) : m_EndContext( context ), m_EndCallback( end ) {}

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
    void* m_EndContext = nullptr;
    void (*m_EndCallback)( void* ) = nullptr;
};
