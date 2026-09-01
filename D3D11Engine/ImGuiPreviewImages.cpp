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

namespace {
    // What the mouse was last pointing at, and when. Hints are collected while the settings window
    // draws and consumed by DrawPinned afterwards, so only the winning image is ever loaded.
    std::string s_HintName;
    std::string s_HintCaption;
    double s_HintTime = -1.0;
}

std::string ImPreview::NameOfToggle( std::string_view name, bool value ) {
    std::string out{ name };
    out += value ? "_On" : "_Off";
    return out;
}

void ImPreview::Hint( std::string_view name, std::string_view caption ) {
    if ( name.empty() ) {
        return;
    }
    s_HintName.assign( name );
    s_HintCaption.assign( caption );
    s_HintTime = ImGui::GetTime();
}

void ImPreview::DrawPinned( const ImVec2& anchorMin, const ImVec2& anchorMax ) {
    // Keep the panel up briefly after the mouse leaves a row, so crossing the gap between two rows
    // doesn't make it blink.
    constexpr double lingerSeconds = 0.15;
    if ( s_HintTime < 0.0 || ImGui::GetTime() - s_HintTime > lingerSeconds ) {
        return;
    }

    GfxTexture* tex = GetOrLoad( s_HintName );
    if ( !tex ) {
        return;
    }

    const ImGuiStyle& style = ImGui::GetStyle();
    const float width = Size + style.WindowPadding.x * 2.0f;
    const ImVec2 workPos = ImGui::GetMainViewport()->WorkPos;
    const ImVec2 workSize = ImGui::GetMainViewport()->WorkSize;

    // Pinned beside the settings window, flipping to its other side when there is no room.
    float x = anchorMax.x + style.ItemSpacing.x;
    if ( x + width > workPos.x + workSize.x ) {
        x = anchorMin.x - style.ItemSpacing.x - width;
    }
    x = std::clamp( x, workPos.x, std::max( workPos.x, workPos.x + workSize.x - width ) );

    ImGui::SetNextWindowPos( ImVec2( x, anchorMin.y ) );
    ImGui::SetNextWindowBgAlpha( 0.95f );
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize
        | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoCollapse
        | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings
        | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;
    if ( ImGui::Begin( "##SettingPreview", nullptr, flags ) ) {
        ImGui::Image( ToImTextureID( tex ), ImVec2( Size, Size ) );
        if ( !s_HintCaption.empty() ) {
            ImGui::TextUnformatted( s_HintCaption.c_str() );
        }
    }
    ImGui::End();
}

void ImPreview::Reset() {
    s_Previews.clear();
    s_HintTime = -1.0;
}
