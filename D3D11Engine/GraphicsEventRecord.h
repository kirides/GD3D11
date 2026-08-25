#pragma once

struct ID3DUserDefinedAnnotation;
struct GraphicsEventName;

class GraphicsEventRecord {
public:
    GraphicsEventRecord() = default;

    GraphicsEventRecord( ID3DUserDefinedAnnotation* userAnnotation, GraphicsEventName region );

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
