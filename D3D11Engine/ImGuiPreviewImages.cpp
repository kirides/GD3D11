#include "ImGuiPreviewImages.h"
#include "Engine.h"
#include "BaseGraphicsEngine.h"
#include "GfxTexture.h"
#include "D3D11Texture.h"
#include "D3D12Engine/D3D12Texture.h"
#include "zFILE_VDFS.h"

#define STB_IMAGE_IMPLEMENTATION
#include "vendor/stb/stb_image.h"

namespace {
    // Name -> texture, with a null entry meaning "looked, not shipped". That negative caching is what
    // keeps a missing preview at one VDFS probe per name instead of one per frame.
    struct PreviewNameHash {
        using is_transparent = void;
        size_t operator()( std::string_view s ) const noexcept { return std::hash<std::string_view>{}( s ); }
    };
    std::unordered_map<std::string, std::unique_ptr<GfxTexture>, PreviewNameHash, std::equal_to<>> s_Previews;

    ImTextureID ToImTextureID( GfxTexture* tex ) {
        switch ( Engine::GraphicsEngine->GetBackendAPI() ) {
        case EGraphicsEngineBackend::D3D11:
            return (ImTextureID)(intptr_t)D3D11Texture::From( tex )->GetShaderResourceView().Get();
        case EGraphicsEngineBackend::D3D12:
            return (ImTextureID)(intptr_t)D3D12Texture::From( tex )->GetSrvGpuHandle().ptr;
        }
        return ImTextureID{};
    }

    std::unique_ptr<GfxTexture> LoadFromVdfs( const std::string& file, const std::string& debugName ) {
        auto filePtr = zFILE_VDFS::Create( R"(\System\GD3D11\Previews\)" + file );
        if ( !filePtr->Exists() || filePtr->Open( false ) != zERRORS::zERROR_NONE ) {
            return {};
        }

        std::vector<uint8_t> data;
        data.resize( filePtr->Size() );
        filePtr->Read( data.data(), static_cast<long>( data.size() ) );
        filePtr->Close();

        int width = 0, height = 0;
        unsigned char* pixels = stbi_load_from_memory( data.data(), static_cast<int>( data.size() ), &width, &height, nullptr, 4 );
        if ( !pixels ) {
            return {};
        }

        std::unique_ptr<GfxTexture> tex;
        Engine::GraphicsEngine->CreateTexture( tex );
        const XRESULT r = tex
            ? tex->Init( INT2( width, height ), GfxTexture::ETextureFormat::TF_R8G8B8A8, 1, pixels, debugName )
            : XRESULT::XR_FAILED;
        stbi_image_free( pixels );

        return r == XRESULT::XR_SUCCESS ? std::move( tex ) : nullptr;
    }

    GfxTexture* GetOrLoad( std::string_view name ) {
        if ( name.empty() || !Engine::GraphicsEngine ) {
            return nullptr;
        }
        if ( auto it = s_Previews.find( name ); it != s_Previews.end() ) {
            return it->second.get();
        }

        std::string key{ name };
        std::unique_ptr<GfxTexture> tex;
        if ( key.find( '.' ) != std::string::npos ) {
            tex = LoadFromVdfs( key, key );
        } else {
            for ( const char* ext : { ".jpg", ".png" } ) {
                tex = LoadFromVdfs( key + ext, key );
                if ( tex ) break;
            }
        }
        return s_Previews.emplace( std::move( key ), std::move( tex ) ).first->second.get();
    }
}

bool ImPreview::Show( std::string_view name, std::string_view hoverName ) {
    GfxTexture* base = GetOrLoad( name );
    if ( !base ) {
        return false;
    }

    GfxTexture* alt = ( hoverName.empty() || hoverName == name ) ? nullptr : GetOrLoad( hoverName );

    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const bool hovered = alt && ImGui::IsMouseHoveringRect( pos, ImVec2( pos.x + Size, pos.y + Size ) );

    ImGui::Image( ToImTextureID( hovered ? alt : base ), ImVec2( Size, Size ) );
    if ( alt ) {
        ImGui::TextDisabled( hovered ? "comparing" : "hover to compare" );
    }
    return true;
}

bool ImPreview::ShowToggle( std::string_view name, bool value ) {
    std::string on{ name }; on += "_On";
    std::string off{ name }; off += "_Off";
    return value ? Show( on, off ) : Show( off, on );
}

bool ImPreview::Exists( std::string_view name ) {
    return GetOrLoad( name ) != nullptr;
}

void ImPreview::Reset() {
    s_Previews.clear();
}
