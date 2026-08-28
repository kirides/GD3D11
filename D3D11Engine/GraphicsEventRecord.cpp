#include "pch.h"
#include "GraphicsEventRecord.h"
#include "BaseGraphicsEngine.h"

GraphicsEventRecord::GraphicsEventRecord( ID3DUserDefinedAnnotation* userAnnotation, const WideNarrowChars& region )
    : m_Annotation( userAnnotation ) {
    if ( m_Annotation ) {
        m_Annotation->BeginEvent( region.wide );
    }
}

void GraphicsEventRecord::End() {
    if ( m_Annotation ) {
        m_Annotation->EndEvent();
        m_Annotation = nullptr;
    }
}
