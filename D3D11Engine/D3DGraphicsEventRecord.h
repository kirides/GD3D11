#pragma once

#include "GraphicsEventRecord.h"

class D3DGraphicsEventRecord :
    public GraphicsEventRecord {
public:
    D3DGraphicsEventRecord() = default;

    D3DGraphicsEventRecord(
        ID3DUserDefinedAnnotation* userAnnotation, 
        GraphicsEventName region )
        : m_Annotation( userAnnotation )
    {
        if ( m_Annotation ) {
            m_Annotation->BeginEvent( region.wide );
        }
    }
    ~D3DGraphicsEventRecord() override {
        if ( m_Annotation ) {
            m_Annotation->EndEvent();
        }
        m_Annotation = nullptr;
    }

    D3DGraphicsEventRecord( const D3DGraphicsEventRecord& other ) = delete;
    D3DGraphicsEventRecord& operator=( const D3DGraphicsEventRecord& ) = delete;

    D3DGraphicsEventRecord( D3DGraphicsEventRecord&& other ) noexcept = default;
    D3DGraphicsEventRecord& operator=( D3DGraphicsEventRecord&& other ) noexcept = default;

private:
    ID3DUserDefinedAnnotation* m_Annotation;
};
