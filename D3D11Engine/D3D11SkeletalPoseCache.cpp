#include "pch.h"
#include "D3D11SkeletalPoseCache.h"

#include "D3D11GraphicsEngineBase.h"
#include "D3D11VertexBuffer.h"
#include "Engine.h"
#include "GothicAPI.h"
#include "WorldObjects.h"
#include "zCModel.h"

namespace {
    constexpr uint32_t kMinPoseCapacity = 1024;

    bool CreatePoseBuffer( std::unique_ptr<D3D11VertexBuffer>& buffer, uint32_t matrixCount, const char* name ) {
        auto newBuffer = std::make_unique<D3D11VertexBuffer>();
        if ( XR_SUCCESS != newBuffer->Init( nullptr, matrixCount * sizeof( XMFLOAT4X4 ),
            D3D11VertexBuffer::B_SHADER_RESOURCE, D3D11VertexBuffer::U_DEFAULT, D3D11VertexBuffer::CA_NONE,
            name, sizeof( XMFLOAT4X4 ) ) ) {
            return false;
        }
        // Init reports success even when CreateBuffer failed, so the views are the real test.
        if ( !newBuffer->GetVertexBuffer() || !newBuffer->GetShaderResourceView() ) {
            return false;
        }
        buffer = std::move( newBuffer );
        return true;
    }

    void UploadRange( D3D11VertexBuffer& buffer, const std::vector<XMFLOAT4X4>& matrices, uint32_t first, uint32_t end ) {
        const D3D11_BOX box = { static_cast<UINT>( first * sizeof( XMFLOAT4X4 ) ), 0, 0,
            static_cast<UINT>( end * sizeof( XMFLOAT4X4 ) ), 1, 1 };
        reinterpret_cast<D3D11GraphicsEngineBase*>( Engine::GraphicsEngine )->GetContext()->UpdateSubresource(
            buffer.GetVertexBuffer().Get(), 0, &box, matrices.data() + first, 0, 0 );
    }
}

D3D11SkeletalPoseCache::D3D11SkeletalPoseCache() = default;
D3D11SkeletalPoseCache::~D3D11SkeletalPoseCache() = default;

void D3D11SkeletalPoseCache::BeginFrame() {
    m_Entries.clear();
    m_Bones.clear();
    m_PrevBones.clear();
    m_UploadedCount = 0;
    m_Frame = Engine::GAPI->GetFrameNumber();
}

D3D11SkeletalPoseCache::Pose D3D11SkeletalPoseCache::Acquire( SkeletalVobInfo* vi, zCModel* model ) {
    if ( !vi || !model ) return {};
    if ( m_Frame != Engine::GAPI->GetFrameNumber() ) {
        BeginFrame();   // a caller outside OnStartWorldRendering's frame must not read last frame's poses
    }

    zCArray<zCModelNodeInst*>* nodeList = model->GetNodeList();
    const uint32_t nodeCount = nodeList ? static_cast<uint32_t>( nodeList->NumInArray ) : 0;

    // Keyed on the pointer, so a vob freed and reallocated mid-frame must not inherit the old pose.
    auto it = m_Entries.find( vi );
    if ( it != m_Entries.end() && it->second.Model == model && it->second.Pose.Count == nodeCount ) {
        return it->second.Pose;
    }
    if ( nodeCount == 0 ) return {};

    // GetBoneTransforms reserves to the exact size, which would reallocate on every vob.
    const size_t required = m_Bones.size() + nodeCount;
    if ( m_Bones.capacity() < required ) {
        m_Bones.reserve( std::max( required, m_Bones.capacity() * 2 ) );
        m_PrevBones.reserve( m_Bones.capacity() );
    }

    Pose pose;
    pose.Offset = static_cast<uint32_t>( m_Bones.size() );
    model->GetBoneTransforms( &m_Bones );
    pose.Count = static_cast<uint32_t>( m_Bones.size() ) - pose.Offset;

    const auto current = m_Bones.begin() + pose.Offset;
    if ( vi->HasValidPrevTransforms && !vi->PrevBoneTransforms.empty() ) {
        const size_t copyCount = std::min<size_t>( vi->PrevBoneTransforms.size(), pose.Count );
        m_PrevBones.insert( m_PrevBones.end(), vi->PrevBoneTransforms.begin(), vi->PrevBoneTransforms.begin() + copyCount );
        m_PrevBones.insert( m_PrevBones.end(), current + copyCount, m_Bones.end() );
    } else {
        m_PrevBones.insert( m_PrevBones.end(), current, m_Bones.end() );
    }

    m_Entries[vi] = Entry{ model, pose };
    return pose;
}

bool D3D11SkeletalPoseCache::Grow( uint32_t required ) {
    const uint32_t capacity = std::max( { required, m_Capacity + m_Capacity / 2, kMinPoseCapacity } );
    if ( !CreatePoseBuffer( m_BoneBuffer, capacity, "SkeletalPoseBones" )
        || !CreatePoseBuffer( m_PrevBoneBuffer, capacity, "SkeletalPosePrevBones" ) ) {
        m_BoneBuffer.reset();
        m_PrevBoneBuffer.reset();
        m_Capacity = 0;
        m_BufferFailed = true;
        Logging::Err( "Skeletal pose buffers ({} bones) could not be created; skinned meshes stop drawing", capacity );
        return false;
    }

    Logging::Inf( "Skeletal pose buffers grown to {} bones ({} KB each)", capacity, capacity * sizeof( XMFLOAT4X4 ) / 1024 );
    m_Capacity = capacity;
    m_UploadedCount = 0;   // the new buffers hold nothing yet
    return true;
}

bool D3D11SkeletalPoseCache::Flush() {
    if ( m_BufferFailed ) return false;

    const uint32_t count = static_cast<uint32_t>( m_Bones.size() );
    if ( ( count > m_Capacity || !m_BoneBuffer ) && !Grow( count ) ) {
        return false;
    }

    if ( m_UploadedCount < count ) {
        UploadRange( *m_BoneBuffer, m_Bones, m_UploadedCount, count );
        UploadRange( *m_PrevBoneBuffer, m_PrevBones, m_UploadedCount, count );
        m_UploadedCount = count;
    }
    return true;
}

ID3D11ShaderResourceView* D3D11SkeletalPoseCache::GetBonesSRV() const {
    return m_BoneBuffer ? m_BoneBuffer->GetShaderResourceView().Get() : nullptr;
}

ID3D11ShaderResourceView* D3D11SkeletalPoseCache::GetPrevBonesSRV() const {
    return m_PrevBoneBuffer ? m_PrevBoneBuffer->GetShaderResourceView().Get() : nullptr;
}
