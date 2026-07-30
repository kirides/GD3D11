#include "MyDirectDrawSurface7.h"
#include "../Engine.h"
#include "../GothicAPI.h"
#include "../D3D11GraphicsEngineBase.h"
#include "../D3D11Texture.h"
#include "../zCTexture.h"
#include "../D3D11_Helpers.h"
#include "../zFILE_VDFS.h"
#include "../ThreadPool.h"

#define DebugWriteTex(x)  DebugWrite(x)

const std::string LEAF_SUBSTR[] = { "Treetop", "Bush", "Leaf" };

/** Reads an already-resolved replacement file and builds the GPU texture from it. Defined below. */
static bool LoadResource( const std::string& path, const std::string& name, std::vector<uint8_t>& storage, GfxTexture** ppTexture );

/** One decoded replacement texture, shared by every surface that resolved to the same file.

    Without this, the same '<tex>_NORMAL.DDS' was read, decoded and uploaded once per Unlock of every
    mip level of its diffuse texture (Gothic unlocks the whole chain through FakeDirectDrawSurface7,
    which forwards to the owning surface) — each load throwing the previous one away, and each one
    blocking on the previous worker job through WaitForPendingAdditionalResources. */
struct SharedAdditionalTexture {
    /** Cache key: the resolved VDFS path this was loaded from. */
    std::string Path;

    /** Owned. Null while not yet loaded, and stays null if the load failed — Loaded still flips, so a
        broken file isn't retried by every surface that references it. */
    GfxTexture* Texture = nullptr;

    /** Guards Texture/Loaded. Held across the read+decode+upload, so a second worker asking for the
        same file waits for the first instead of duplicating the work. */
    std::mutex LoadMutex;
    bool Loaded = false;

    ~SharedAdditionalTexture();

    /** Loads on first call, no-ops afterwards. Returns the (possibly null) texture. */
    GfxTexture* EnsureLoaded( const std::string& name, std::vector<uint8_t>& storage );
};

namespace {
    /** Path-keyed cache of the decoded replacement textures. Weak on purpose: an entry lives exactly
        as long as some surface still references it, so a texture that Gothic caches out gives its
        normal/ORM maps back instead of pinning them for the rest of the session — this is a 32-bit
        process and replacement packs are large. */
    std::mutex s_AdditionalTextureCacheMutex;
    std::unordered_map<std::string, std::weak_ptr<SharedAdditionalTexture>> s_AdditionalTextureCache;

    /** Returns the shared entry for 'path', creating (but not loading) it if this is the first user. */
    std::shared_ptr<SharedAdditionalTexture> AcquireAdditionalTexture( const std::string& path ) {
        std::scoped_lock lock( s_AdditionalTextureCacheMutex );
        std::weak_ptr<SharedAdditionalTexture>& slot = s_AdditionalTextureCache[path];
        if ( std::shared_ptr<SharedAdditionalTexture> existing = slot.lock() ) {
            return existing;
        }

        auto entry = std::make_shared<SharedAdditionalTexture>();
        entry->Path = path;
        slot = entry;
        return entry;
    }

    /** True if this entry has nothing left to do (a null entry means "no such replacement file"). */
    bool IsAdditionalTextureResolved( const std::shared_ptr<SharedAdditionalTexture>& entry ) {
        if ( !entry ) return true;
        std::scoped_lock lock( entry->LoadMutex );
        return entry->Loaded;
    }

    /** Texture of an already-resolved entry. Only valid after IsAdditionalTextureResolved(). */
    GfxTexture* PeekAdditionalTexture( const std::shared_ptr<SharedAdditionalTexture>& entry ) {
        if ( !entry ) return nullptr;
        std::scoped_lock lock( entry->LoadMutex );
        return entry->Texture;
    }
}

SharedAdditionalTexture::~SharedAdditionalTexture() {
    {
        std::scoped_lock lock( s_AdditionalTextureCacheMutex );
        // Only drop our own slot: a surface may already have raced in and put a fresh entry for the
        // same path there, in which case that one is still alive and must be left alone.
        if ( auto it = s_AdditionalTextureCache.find( Path );
             it != s_AdditionalTextureCache.end() && it->second.expired() ) {
            s_AdditionalTextureCache.erase( it );
        }
    }

    // May run on a worker thread (last reference dropped by a finishing load job). Both backends'
    // texture destructors are safe there: D3D11 just releases COM objects, D3D12 pushes the resource
    // and its SRV slot onto the mutex-guarded deferred-cleanup queue.
    delete Texture;
}

GfxTexture* SharedAdditionalTexture::EnsureLoaded( const std::string& name, std::vector<uint8_t>& storage ) {
    std::scoped_lock lock( LoadMutex );
    if ( !Loaded ) {
        Loaded = true;
        LoadResource( Path, name, storage, &Texture ); // leaves Texture null on failure
    }
    return Texture;
}

MyDirectDrawSurface7::MyDirectDrawSurface7() {
    refCount = 1;
    EngineTexture = nullptr;
    Normalmap = nullptr;
    FxMap = nullptr;
    LockedData = nullptr;
    GothicTexture = nullptr;
    IsReady = false;
    TextureType = ETextureType::TX_UNDEF;
    AvailableMaterials = EAdditionalMaterial::None;
    AdditionalResourcesResolved = false;
    LockType = 0;

    // Check for test-bind mode to figure out what zCTexture-Object we are associated with
    std::string bound;
    if ( Engine::GAPI->IsInTextureTestBindMode( bound ) ) {
        Engine::GAPI->SetTextureTestBindMode( false, "" );
        return;
    }
}

MyDirectDrawSurface7::~MyDirectDrawSurface7() {
    IsReady = false;

    // A worker may still be loading this surface's normal/ORM maps and will publish them into
    // Normalmap/FxMap — never free the surface out from under it. Common on texture cache
    // invalidation, where surfaces are destroyed while their loads are still queued.
    WaitForPendingAdditionalResources();

    // Release mip-map chain first
    for ( LPDIRECTDRAWSURFACE7 mipmap : attachedSurfaces ) {
        mipmap->Release();
    }

    // Sometimes gothic doesn't unlock a surface or this is a movie-buffer
    delete[] LockedData;

    delete EngineTexture;

    // The maps themselves belong to the shared cache — dropping our share is all that's needed, and
    // only deletes them if no other surface still uses the same replacement file.
    Normalmap.store( nullptr, std::memory_order_release );
    FxMap.store( nullptr, std::memory_order_release );
    NormalmapRef.reset();
    FxMapRef.reset();
}

/** Returns the engine texture of this surface */
GfxTexture* MyDirectDrawSurface7::GetEngineTexture() {
    return EngineTexture;
}

/** Returns the engine texture of this surface. Null until the async load publishes it. */
GfxTexture* MyDirectDrawSurface7::GetNormalmap() {
    return Normalmap.load( std::memory_order_acquire );
}

/** Returns the fx-map for this surface. Null until the async load publishes it. */
GfxTexture* MyDirectDrawSurface7::GetFxMap() {
    return FxMap.load( std::memory_order_acquire );
}

/** Binds this texture */
void MyDirectDrawSurface7::BindToSlot( int slot ) {
    if ( !IsReady ) {
        // Don't bind half-loaded textures! Clear just the diffuse slot.
        Engine::GraphicsEngine->BindSurfaceTextures( slot, nullptr, nullptr, 1 );
        return;
    }

    Engine::GraphicsEngine->BindSurfaceTextures( slot, EngineTexture, Normalmap, 2 );
}

namespace {
    static constexpr char vdfsWorkPath[] { R"(\_WORK\DATA\TEXTURES\REPLACEMENTS\)" };
    static constexpr char systemWorkPath[] { R"(\SYSTEM\GD3D11\TEXTURES\REPLACEMENTS\)" };

    /** In-flight async additional-resource loads, keyed by surface. Registered/pruned/waited by whoever
        calls LoadAdditionalResources (the game thread, or Gothic's own loader thread via the deferred
        Unlock path) — but never by the worker job itself, which is what lets a waiter hold the mutex
        while waiting on a job without deadlocking against it. Mirrors WorldConverter's node-visual
        registry. */
    struct PendingSurfaceLoad {
        MyDirectDrawSurface7* Surface;
        TaskHandle<void> Task;
    };
    std::mutex s_PendingSurfaceLoadsMutex;
    std::vector<PendingSurfaceLoad> s_PendingSurfaceLoads;

    /** Caller must hold s_PendingSurfaceLoadsMutex. */
    void PruneFinishedSurfaceLoadsLocked() {
        std::erase_if( s_PendingSurfaceLoads, []( const PendingSurfaceLoad& p ) {
            return !p.Task.future.valid()
                || p.Task.future.wait_for( std::chrono::seconds( 0 ) ) == std::future_status::ready;
        } );
    }

    /** Probes both replacement roots for '<textureName><suffix>' and returns the path that exists, or
        an empty string. Existence-only: no open, no read, no decode — cheap enough to keep on the game
        thread, which is what lets the caller decide up front whether a worker job is needed at all. */
    std::string ResolveResourcePath( const std::string& textureName, const char* suffix, const std::string& gameName ) {
        std::string candidate;
        candidate.reserve( 255 );

        candidate.append( vdfsWorkPath ).append( textureName ).append( suffix );
        if ( auto file = zFILE_VDFS::Create( candidate ); file && file->Exists() ) {
            return candidate;
        }

        candidate.clear();
        candidate.append( systemWorkPath ).append( "Normalmaps_" ).append( gameName ).append( "\\" )
                 .append( textureName ).append( suffix );
        if ( auto file = zFILE_VDFS::Create( candidate ); file && file->Exists() ) {
            return candidate;
        }
        return {};
    }
}

/** Reads an already-resolved replacement file and builds the GPU texture from it. Runs on a worker
    thread: the VDFS is read-only and thread safe, and both backends' GfxTexture::Init only use
    free-threaded device calls (D3D11 CreateDDSTextureFromMemory / D3D12 CreateResource) plus the
    internally-locked D3D12 upload batch — neither touches an immediate context. */
static bool LoadResource(
    const std::string& path,
    const std::string& name,
    std::vector<uint8_t>& storage,
    GfxTexture** ppTexture) {
    auto file = zFILE_VDFS::Create( path );
    if ( !file->Exists() ) {
        return false;
    }
    auto retOpen = file->Open( false );
    if ( retOpen != zERROR_NONE ) {
        return false;
    }
    auto size = file->Size();
    storage.resize(size);

    // assume single op file read.
    auto numRead = file->Read( storage.data(), size );
    (void)file->Close();

    if (numRead != size) {
        LogWarn() << "Failed to load " << name << ": short read";
        return false;
    }

    Engine::GraphicsEngine->CreateTexture( ppTexture );
    if ( XR_SUCCESS != (*ppTexture)->Init( storage.data(), size, name ) ) {
        SAFE_DELETE( *ppTexture );
        LogWarn() << "Failed to load " << name << ": init failed";
        return false;
    }
    return true;
}

void MyDirectDrawSurface7::WaitForPendingAdditionalResources() {
    std::scoped_lock lock( s_PendingSurfaceLoadsMutex );
    for ( auto& pending : s_PendingSurfaceLoads ) {
        if ( pending.Surface == this && pending.Task.future.valid() ) {
            pending.Task.future.wait();
        }
    }
    PruneFinishedSurfaceLoadsLocked();
}

/** Loads additional resources if possible */
void MyDirectDrawSurface7::LoadAdditionalResources( zCTexture* ownedTexture ) {
    // Gothic unlocks every mip level of a texture and each of them ends up here (the mip surfaces are
    // views onto this one). Everything below — the VDFS probes, the cache lookups, the worker job —
    // is keyed off TextureName, which is latched from the first zCTexture this surface is associated
    // with and never changes afterwards, so every repeat call would redo identical work. A re-cached
    // texture gets a brand new surface, so nothing needs this to be re-resolvable.
    if ( AdditionalResourcesResolved ) {
        return;
    }

    // A worker may still be loading this surface's maps from an earlier call (the texture got
    // re-cached). It owns Normalmap/FxMap until it publishes them, so join it before touching either.
    WaitForPendingAdditionalResources();

    if ( !GothicTexture ) {
#ifndef PUBLIC_RELEASE
        // The name latched here is permanent, so a wrong zCTexture means this surface looks for (and
        // caches under) somebody else's replacement maps forever. That can only happen if the loading
        // texture leaked across threads or past its load - see GothicAPI::ScopedLoadingTexture.
        if ( MyDirectDrawSurface7* owner = ownedTexture->GetSurface(); owner && owner != this ) {
            LogWarn() << "LoadAdditionalResources: '" << ownedTexture->GetNameView()
                << "' does not own this surface - texture name would be wrong, skipping";
            return;
        }
#endif

        GothicTexture = ownedTexture;
        TextureName = GothicTexture->GetNameWithoutExtView();

        // Find texture type
        if ( Toolbox::StringContainsOneOf( TextureName, LEAF_SUBSTR, std::size( LEAF_SUBSTR ) ) ) {
            TextureType = ETextureType::TX_LEAF;
        }

    }

    Normalmap.store( nullptr, std::memory_order_release );
    FxMap.store( nullptr, std::memory_order_release );
    AvailableMaterials.store( EAdditionalMaterial::None, std::memory_order_release );
    NormalmapRef.reset();
    FxMapRef.reset();

    AdditionalResourcesResolved = true;

    // Set texture name
    if ( EngineTexture ) {
        EngineTexture->SetDebugName( TextureName.c_str() );
    }

    if ( TextureName.empty() || !Engine::GAPI->GetRendererState().RendererSettings.AllowNormalmaps ) {
        return;
    }

    // --- Probe on the game thread ---
    // Only existence checks happen here. The overwhelmingly common case for vanilla content is that
    // nothing exists, and that case still costs exactly what it did before: a couple of VDFS index
    // lookups and no job at all.
    const std::string& gameName = Engine::GAPI->GetGameName();

    const std::string normalPath = ResolveResourcePath( TextureName, "_NORMAL.DDS", gameName );

    std::string materialPath;
    const char* materialSuffix = "";
    EAdditionalMaterial materialType = EAdditionalMaterial::None;
    {
        // D3D12 prefers a full PBR set and falls back through the partial variants; D3D11 only knows
        // the legacy specular map. First hit wins, exactly as the synchronous version did.
        const std::pair<const char*, EAdditionalMaterial> d3d12Variants[] = {
            { "_ORM.DDS",   EAdditionalMaterial::ORM },
            { "_OR.DDS",    EAdditionalMaterial::AoRoughness },
            { "_ROUGH.DDS", EAdditionalMaterial::Roughness },
        };
        const std::pair<const char*, EAdditionalMaterial> d3d11Variants[] = {
            { "_FX.DDS",    EAdditionalMaterial::Specular },
        };
        const bool isD3D12 = Engine::GraphicsEngine->GetBackendAPI() == EGraphicsEngineBackend::D3D12;
        using VariantSpan = std::span<const std::pair<const char*, EAdditionalMaterial>>;
        const VariantSpan variants = isD3D12 ? VariantSpan{ d3d12Variants } : VariantSpan{ d3d11Variants };

        for ( const auto& [suffix, type] : variants ) {
            materialPath = ResolveResourcePath( TextureName, suffix, gameName );
            if ( !materialPath.empty() ) {
                materialSuffix = suffix;
                materialType = type;
                break;
            }
        }
    }

    if ( normalPath.empty() && materialPath.empty() ) {
        return; // No replacements for this texture — nothing to hand off.
    }

    // --- Claim the shared cache entries ---
    // Refcounting happens here, on the calling thread, so the entries can't be destroyed underneath a
    // worker that is still decoding into them. Whether they are already loaded decides if a job is
    // needed at all.
    NormalmapRef = normalPath.empty() ? nullptr : AcquireAdditionalTexture( normalPath );
    FxMapRef = materialPath.empty() ? nullptr : AcquireAdditionalTexture( materialPath );

    // Another surface already loaded these files (or this texture was cached back in while another
    // surface still held them). Publish straight away — no file IO, no upload, no job.
    if ( IsAdditionalTextureResolved( NormalmapRef ) && IsAdditionalTextureResolved( FxMapRef ) ) {
        GfxTexture* fx = PeekAdditionalTexture( FxMapRef );
        Normalmap.store( PeekAdditionalTexture( NormalmapRef ), std::memory_order_release );
        AvailableMaterials.store( fx ? materialType : EAdditionalMaterial::None, std::memory_order_release );
        FxMap.store( fx, std::memory_order_release );
        return;
    }

    // --- Read + decode + upload on a worker ---
    // This is the part that cost up to 20ms per cache-in burst on the game thread. Until it lands the
    // surface simply renders without its extra maps, which is the same state it is in for any texture
    // that has no replacement files.
    if ( !Engine::WorkerThreadPool ) {
        std::vector<uint8_t> storage;
        GfxTexture* nrm = NormalmapRef ? NormalmapRef->EnsureLoaded( TextureName + "_NORMAL.DDS", storage ) : nullptr;
        GfxTexture* fx = FxMapRef ? FxMapRef->EnsureLoaded( TextureName + materialSuffix, storage ) : nullptr;
        Normalmap.store( nrm, std::memory_order_release );
        AvailableMaterials.store( fx ? materialType : EAdditionalMaterial::None, std::memory_order_release );
        FxMap.store( fx, std::memory_order_release );
        return;
    }

    auto task = Engine::WorkerThreadPool->enqueue(
        [this, nrmRef = NormalmapRef, fxRef = FxMapRef, materialType, materialSuffix, name = TextureName]( const std::stop_token& token ) {
            if ( token.stop_requested() ) {
                // Cancelled before a worker picked it up (world change / pool flush). Leave the maps
                // null; the surface renders diffuse-only until the texture is cached in again.
                return;
            }

            std::vector<uint8_t> storage;

            // EnsureLoaded does the work at most once per file across the whole process; a second
            // worker that wants the same file waits here instead of decoding it a second time.
            GfxTexture* nrm = nrmRef ? nrmRef->EnsureLoaded( name + "_NORMAL.DDS", storage ) : nullptr;
            GfxTexture* fx = fxRef ? fxRef->EnsureLoaded( name + materialSuffix, storage ) : nullptr;

            // Publish. AvailableMaterials must be visible before FxMap, because every reader gates on
            // GetFxMap() being non-null and then trusts GetAvailableMaterials() to say what its
            // channels mean — the reverse order could decode an ORM map as a roughness-only one.
            Normalmap.store( nrm, std::memory_order_release );
            AvailableMaterials.store( fx ? materialType : EAdditionalMaterial::None, std::memory_order_release );
            FxMap.store( fx, std::memory_order_release );
        } );

    std::scoped_lock lock( s_PendingSurfaceLoadsMutex );
    PruneFinishedSurfaceLoadsLocked();
    s_PendingSurfaceLoads.emplace_back( this, std::move( task ) );
}

HRESULT MyDirectDrawSurface7::QueryInterface( REFIID riid, LPVOID* ppvObj ) {
    DebugWriteTex( "IDirectDrawSurface7(%p)::QueryInterface(%s)" );
    return S_OK;
}

ULONG MyDirectDrawSurface7::AddRef() {
    DebugWriteTex( "IDirectDrawSurface7(%p)::AddRef(%i)" );
    return refCount.fetch_add( 1, std::memory_order_relaxed ) + 1;
}

ULONG MyDirectDrawSurface7::Release() {
    DebugWriteTex( "IDirectDrawSurface7(%p)::Release(%i)" );
    // acq_rel so everything the releasing thread did to this surface is visible to whoever ends up
    // running the destructor.
    ULONG remaining = refCount.fetch_sub( 1, std::memory_order_acq_rel ) - 1;
    if ( remaining == 0 ) {
        delete this;
        return 0;
    }

    return remaining;
}

HRESULT MyDirectDrawSurface7::AddAttachedSurface( LPDIRECTDRAWSURFACE7 lpDDSAttachedSurface ) {
    DebugWriteTex( "IDirectDrawSurface7(%p)::AddAttachedSurface()" );
    lpDDSAttachedSurface->AddRef();
    attachedSurfaces.push_back( static_cast<MyDirectDrawSurface7*>(lpDDSAttachedSurface) );
    return S_OK;
}

HRESULT MyDirectDrawSurface7::AddOverlayDirtyRect( LPRECT lpRect ) {
    DebugWriteTex( "IDirectDrawSurface7(%p)::AddOverlayDirtyRect()" );
    return S_OK;
}

HRESULT MyDirectDrawSurface7::Blt( LPRECT lpDestRect, LPDIRECTDRAWSURFACE7 lpDDSrcSurface, LPRECT lpSrcRect, DWORD dwFlags, LPDDBLTFX lpDDBltFx ) {
    DebugWriteTex( "IDirectDrawSurface7(%p)::Blt()" );
    return S_OK;
}

HRESULT MyDirectDrawSurface7::BltBatch( LPDDBLTBATCH lpDDBltBatch, DWORD dwCount, DWORD dwFlags ) {
    DebugWriteTex( "IDirectDrawSurface7(%p)::BltBatch()" );
    return S_OK;
}

HRESULT MyDirectDrawSurface7::BltFast( DWORD dwX, DWORD dwY, LPDIRECTDRAWSURFACE7 lpDDSrcSurface, LPRECT lpSrcRect, DWORD dwTrans ) {
    DebugWriteTex( "IDirectDrawSurface7(%p)::BltFast()" );
    return S_OK;
}

HRESULT MyDirectDrawSurface7::DeleteAttachedSurface( DWORD dwFlags, LPDIRECTDRAWSURFACE7 lpDDSAttachedSurface ) {
    DebugWriteTex( "IDirectDrawSurface7(%p)::DeleteAttachedSurface()" );
    return S_OK;
}

HRESULT MyDirectDrawSurface7::EnumAttachedSurfaces( LPVOID lpContext, LPDDENUMSURFACESCALLBACK7 lpEnumSurfacesCallback ) {
    DebugWriteTex( "IDirectDrawSurface7(%p)::EnumAttachedSurfaces()" );
    return S_OK;
}

HRESULT MyDirectDrawSurface7::EnumOverlayZOrders( DWORD dwFlags, LPVOID lpContext, LPDDENUMSURFACESCALLBACK7 lpfnCallback ) {
    DebugWriteTex( "IDirectDrawSurface7(%p)::EnumOverlayZOrders()" );
    return S_OK;
}

HRESULT MyDirectDrawSurface7::Flip( LPDIRECTDRAWSURFACE7 lpDDSurfaceTargetOverride, DWORD dwFlags ) {
    DebugWriteTex( "IDirectDrawSurface7(%p)::Flip() #####" );
    return S_OK;
}

HRESULT MyDirectDrawSurface7::GetAttachedSurface( LPDDSCAPS2 lpDDSCaps2, LPDIRECTDRAWSURFACE7* lplpDDAttachedSurface ) {
    DebugWriteTex( "IDirectDrawSurface7(%p)::GetAttachedSurface()" );

    if ( attachedSurfaces.empty() )
        return E_FAIL;

    *lplpDDAttachedSurface = attachedSurfaces[0];
    attachedSurfaces[0]->AddRef();

    return S_OK;
}

HRESULT MyDirectDrawSurface7::GetBltStatus( DWORD dwFlags ) {
    DebugWriteTex( "IDirectDrawSurface7(%p)::GetBltStatus()" );
    return S_OK;
}

HRESULT MyDirectDrawSurface7::GetCaps( LPDDSCAPS2 lpDDSCaps2 ) {
    DebugWriteTex( "IDirectDrawSurface7(%p)::GetCaps()" );
    *lpDDSCaps2 = OriginalSurfaceDesc.ddsCaps;
    return S_OK;
}

HRESULT MyDirectDrawSurface7::GetClipper( LPDIRECTDRAWCLIPPER* lplpDDClipper ) {
    DebugWriteTex( "IDirectDrawSurface7(%p)::GetClipper()" );
    return S_OK;
}

HRESULT MyDirectDrawSurface7::GetColorKey( DWORD dwFlags, LPDDCOLORKEY lpDDColorKey ) {
    DebugWriteTex( "IDirectDrawSurface7(%p)::GetColorKey()" );
    return S_OK;
}

HRESULT MyDirectDrawSurface7::GetDC( HDC* lphDC ) {
    DebugWriteTex( "IDirectDrawSurface7(%p)::GetDC()" );
    return S_OK;
}

HRESULT MyDirectDrawSurface7::GetFlipStatus( DWORD dwFlags ) {
    DebugWriteTex( "IDirectDrawSurface7(%p)::GetFlipStatus()" );
    return S_OK;
}

HRESULT MyDirectDrawSurface7::GetOverlayPosition( LPLONG lplX, LPLONG lplY ) {
    DebugWriteTex( "IDirectDrawSurface7(%p)::GetOverlayPosition()" );
    return S_OK;
}

HRESULT MyDirectDrawSurface7::GetPalette( LPDIRECTDRAWPALETTE* lplpDDPalette ) {
    DebugWriteTex( "IDirectDrawSurface7(%p)::GetPalette()" );
    return S_OK;
}

HRESULT MyDirectDrawSurface7::GetPixelFormat( LPDDPIXELFORMAT lpDDPixelFormat ) {
    DebugWriteTex( "IDirectDrawSurface7(%p)::GetPixelFormat()" );

    *lpDDPixelFormat = OriginalSurfaceDesc.ddpfPixelFormat;

    return S_OK;
}

HRESULT MyDirectDrawSurface7::GetSurfaceDesc( LPDDSURFACEDESC2 lpDDSurfaceDesc ) {
    *lpDDSurfaceDesc = OriginalSurfaceDesc;
    return S_OK;
}

HRESULT MyDirectDrawSurface7::Initialize( LPDIRECTDRAW lpDD, LPDDSURFACEDESC2 lpDDSurfaceDesc ) {
    DebugWriteTex( "IDirectDrawSurface7(%p)::Initialize()" );
    return S_OK;
}

HRESULT MyDirectDrawSurface7::IsLost() {
    DebugWriteTex( "IDirectDrawSurface7(%p)::IsLost()" );
    return S_OK;
}

HRESULT MyDirectDrawSurface7::Lock( LPRECT lpDestRect, LPDDSURFACEDESC2 lpDDSurfaceDesc, DWORD dwFlags, HANDLE hEvent ) {
    DebugWriteTex( "IDirectDrawSurface7(%p)::Lock()" );

    LockType = dwFlags;

    *lpDDSurfaceDesc = OriginalSurfaceDesc;

    // This has to be a backbuffer-copy
    if ( (LockType & DDLOCK_READONLY) != 0 && LockType != DDLOCK_READONLY ) // Gothic uses DDLOCK_READONLY + some other flags for getting the framebuffer. DDLOCK_READONLY only is for movie playback. 
    {
        extern bool CreatingThumbnail;

        // Assume 32-bit
        byte* data;
        INT2 buffersize;
        int pixelSize;
        Engine::GraphicsEngine->ResetPresentPending();
        Engine::GraphicsEngine->OnStartWorldRendering();
        Engine::GraphicsEngine->GetBackbufferData( CreatingThumbnail, &data, buffersize, pixelSize );
        Engine::GraphicsEngine->ResetPresentPending();

        lpDDSurfaceDesc->ddpfPixelFormat.dwRGBBitCount = 32;
        lpDDSurfaceDesc->ddpfPixelFormat.dwRBitMask = 0x00FF0000;
        lpDDSurfaceDesc->ddpfPixelFormat.dwGBitMask = 0x0000FF00;
        lpDDSurfaceDesc->ddpfPixelFormat.dwBBitMask = 0x000000FF;
        lpDDSurfaceDesc->ddpfPixelFormat.dwRGBAlphaBitMask = 0x00000000;

        lpDDSurfaceDesc->lPitch = buffersize.x * pixelSize;
        lpDDSurfaceDesc->dwWidth = buffersize.x;
        lpDDSurfaceDesc->dwHeight = buffersize.y;
        lpDDSurfaceDesc->lpSurface = data;

        LockedData = data;
        CreatingThumbnail = false;

        return S_OK;
    }

    if ( !EngineTexture )
        return S_OK;

    // Check for 16-bit surface. We allocate the texture as 32-bit, so we need to divide the size by two for that
    int redBits = Toolbox::GetNumberOfBits( OriginalSurfaceDesc.ddpfPixelFormat.dwRBitMask );
    int greenBits = Toolbox::GetNumberOfBits( OriginalSurfaceDesc.ddpfPixelFormat.dwGBitMask );
    int blueBits = Toolbox::GetNumberOfBits( OriginalSurfaceDesc.ddpfPixelFormat.dwBBitMask );
    int alphaBits = Toolbox::GetNumberOfBits( OriginalSurfaceDesc.ddpfPixelFormat.dwRGBAlphaBitMask );

    int bpp = redBits + greenBits + blueBits + alphaBits;

    if ( bpp == 24 ) {
        // Handle movie frame,
        // don't deallocate the memory after unlock, since only the changing parts in videos will get updated
        if ( !LockedData )
            LockedData = new unsigned char[EngineTexture->GetSizeInBytes( 0 )];
    } else {
        // Allocate some temporary data
        delete[] LockedData;
        LockedData = new unsigned char[EngineTexture->GetSizeInBytes( 0 )];
    }

    lpDDSurfaceDesc->lpSurface = LockedData;
    lpDDSurfaceDesc->lPitch = EngineTexture->GetRowPitchBytes( 0 );

    return S_OK;
}

#pragma warning(push)
#pragma warning(disable: 6386)
HRESULT MyDirectDrawSurface7::Unlock( LPRECT lpRect ) {
    DebugWriteTex( "IDirectDrawSurface7(%p)::Unlock()" );

    // This has to be a backbuffer-copy
    if ( (LockType & DDLOCK_READONLY) != 0 && LockType != DDLOCK_READONLY ) {
        // Clean up
        delete[] LockedData;
        LockedData = nullptr;

        return S_OK;
    }

    // If this is a 16-bit surface, we need to convert it to 32-bit first
    int redBits = Toolbox::GetNumberOfBits( OriginalSurfaceDesc.ddpfPixelFormat.dwRBitMask );
    int greenBits = Toolbox::GetNumberOfBits( OriginalSurfaceDesc.ddpfPixelFormat.dwGBitMask );
    int blueBits = Toolbox::GetNumberOfBits( OriginalSurfaceDesc.ddpfPixelFormat.dwBBitMask );
    int alphaBits = Toolbox::GetNumberOfBits( OriginalSurfaceDesc.ddpfPixelFormat.dwRGBAlphaBitMask );

    int bpp = redBits + greenBits + blueBits + alphaBits;

    // No conversion needed
    if ( Engine::GAPI->GetMainThreadID() != GetCurrentThreadId() ) {
        EngineTexture->UpdateDataDeferred( LockedData, 0 );
        Engine::GAPI->AddFrameLoadedTexture( this );

        // Only set while this thread is inside LoadResourceData. This is used to get the zCTexture from this Surface.
        if ( zCTexture* loading = Engine::GAPI->GetLoadingTexture() ) {
            // Comming from LoadResourceData
            LoadAdditionalResources( loading );
        }
    } else {
        EngineTexture->UpdateData( LockedData, 0 );
        // Only set while this thread is inside LoadResourceData. This is used to get the zCTexture from this Surface.
        if ( zCTexture* loading = Engine::GAPI->GetLoadingTexture() ) {
            // Comming from LoadResourceData
            LoadAdditionalResources( loading );
        }
        SetReady( true ); // No need to load other stuff to get this ready
    }


    if ( bpp != 24 ) {
        // Clean up if not a movie frame
        delete[] LockedData;
        LockedData = nullptr;
    }

    return S_OK;
}
#pragma warning(pop)

HRESULT MyDirectDrawSurface7::ReleaseDC( HDC hDC ) {
    DebugWriteTex( "IDirectDrawSurface7(%p)::ReleaseDC()" );
    return S_OK;
}

HRESULT MyDirectDrawSurface7::Restore() {
    DebugWriteTex( "IDirectDrawSurface7(%p)::Restore()" );
    return S_OK;
}

HRESULT MyDirectDrawSurface7::SetClipper( LPDIRECTDRAWCLIPPER lpDDClipper ) {
    DebugWriteTex( "IDirectDrawSurface7(%p)::SetClipper()" );

    hook_infunc

        HWND hWnd;
        lpDDClipper->GetHWnd( &hWnd );
        Engine::GAPI->OnSetWindow( hWnd );

    hook_outfunc

    return S_OK;
}

HRESULT MyDirectDrawSurface7::SetColorKey( DWORD dwFlags, LPDDCOLORKEY lpDDColorKey ) {
    DebugWriteTex( "IDirectDrawSurface7(%p)::SetColorKey(%s, %s)" );
    return S_OK;
}

HRESULT MyDirectDrawSurface7::SetOverlayPosition( LONG lX, LONG lY ) {
    DebugWriteTex( "IDirectDrawSurface7(%p)::SetOverlayPosition()" );
    return S_OK;
}

HRESULT MyDirectDrawSurface7::SetPalette( LPDIRECTDRAWPALETTE lpDDPalette ) {
    DebugWriteTex( "IDirectDrawSurface7(%p)::SetPalette()" );
    return S_OK;
}

HRESULT MyDirectDrawSurface7::UpdateOverlay( LPRECT lpSrcRect, LPDIRECTDRAWSURFACE7 lpDDDestSurface, LPRECT lpDestRect, DWORD dwFlags, LPDDOVERLAYFX lpDDOverlayFx ) {
    DebugWriteTex( "IDirectDrawSurface7(%p)::UpdateOverlay()" );
    return S_OK;
}

HRESULT MyDirectDrawSurface7::UpdateOverlayDisplay( DWORD dwFlags ) {
    DebugWriteTex( "IDirectDrawSurface7(%p)::UpdateOverlayDisplay()" );
    return S_OK;
}

HRESULT MyDirectDrawSurface7::UpdateOverlayZOrder( DWORD dwFlags, LPDIRECTDRAWSURFACE7 lpDDSReference ) {
    DebugWriteTex( "IDirectDrawSurface7(%p)::UpdateOverlayZOrder()" );
    return S_OK;
}

HRESULT MyDirectDrawSurface7::GetDDInterface( LPVOID* lplpDD ) {
    DebugWriteTex( "IDirectDrawSurface7(%p)::GetDDInterface()" );
    return S_OK;
}

HRESULT MyDirectDrawSurface7::PageLock( DWORD dwFlags ) {
    DebugWriteTex( "IDirectDrawSurface7(%p)::PageLock()" );
    return S_OK;
}

HRESULT MyDirectDrawSurface7::PageUnlock( DWORD dwFlags ) {
    DebugWriteTex( "IDirectDrawSurface7(%p)::PageUnlock()" );
    return S_OK;
}

HRESULT MyDirectDrawSurface7::SetSurfaceDesc( LPDDSURFACEDESC2 lpDDSurfaceDesc, DWORD dwFlags ) {
    DebugWriteTex( "IDirectDrawSurface7(%p)::SetSurfaceDesc()" );

    OriginalSurfaceDesc = *lpDDSurfaceDesc;

    // Check if this is the rendertarget or something else we dont need
    if ( lpDDSurfaceDesc->dwWidth == 0 ) {
        return S_OK;
    }

    // Create the texture object this is linked with
    Engine::GraphicsEngine->CreateTexture( &EngineTexture );


    int redBits = Toolbox::GetNumberOfBits( lpDDSurfaceDesc->ddpfPixelFormat.dwRBitMask );
    int greenBits = Toolbox::GetNumberOfBits( lpDDSurfaceDesc->ddpfPixelFormat.dwGBitMask );
    int blueBits = Toolbox::GetNumberOfBits( lpDDSurfaceDesc->ddpfPixelFormat.dwBBitMask );
    int alphaBits = Toolbox::GetNumberOfBits( lpDDSurfaceDesc->ddpfPixelFormat.dwRGBAlphaBitMask );

    int bpp = redBits + greenBits + blueBits + alphaBits;

    // Find out format
    GfxTexture::ETextureFormat format = GfxTexture::ETextureFormat::TF_B8G8R8A8;
    switch ( bpp ) {
    case 16:
    {
        switch ( OriginalSurfaceDesc.ddpfPixelFormat.dwFourCC ) {
        case 1: format = GfxTexture::ETextureFormat::TF_B5G5R5A1; break;
        case 2: format = GfxTexture::ETextureFormat::TF_B4G4R4A4; break;
        default: format = GfxTexture::ETextureFormat::TF_B5G6R5; break;
        }
        break;
    }
    case 24:
    case 32:
        format = GfxTexture::ETextureFormat::TF_B8G8R8A8;
        break;

    case 0:
    {
        // DDS-Texture
        if ( (lpDDSurfaceDesc->ddpfPixelFormat.dwFlags & DDPF_FOURCC) == DDPF_FOURCC ) {
            switch ( lpDDSurfaceDesc->ddpfPixelFormat.dwFourCC ) {
            case FOURCC_DXT1:
                format = GfxTexture::ETextureFormat::TF_DXT1;
                break;

            case FOURCC_DXT2:
            case FOURCC_DXT3:
                format = GfxTexture::ETextureFormat::TF_DXT3;
                break;

            case FOURCC_DXT4:
            case FOURCC_DXT5:
                format = GfxTexture::ETextureFormat::TF_DXT5;
                break;
            }
        }
    }
    break;
    }

    // Find out mip-level count
    unsigned int mipMapCount = 1;
    if ( lpDDSurfaceDesc->ddsCaps.dwCaps & DDSCAPS_MIPMAP ) {
        mipMapCount = lpDDSurfaceDesc->dwMipMapCount;
    }

    // Create the texture
    EngineTexture->Init( INT2( lpDDSurfaceDesc->dwWidth, lpDDSurfaceDesc->dwHeight ), format, mipMapCount, nullptr, "" );

    return S_OK;
}

HRESULT MyDirectDrawSurface7::SetPrivateData( REFGUID guidTag, LPVOID lpData, DWORD cbSize, DWORD dwFlags ) {
    DebugWriteTex( "IDirectDrawSurface7(%p)::SetPrivateData()" );
    return S_OK;
}

HRESULT MyDirectDrawSurface7::GetPrivateData( REFGUID guidTag, LPVOID lpBuffer, LPDWORD lpcbBufferSize ) {
    DebugWriteTex( "IDirectDrawSurface7(%p)::GetPrivateData()" );
    return S_OK;
}

HRESULT MyDirectDrawSurface7::FreePrivateData( REFGUID guidTag ) {
    DebugWriteTex( "IDirectDrawSurface7(%p)::FreePrivateData()" );
    return S_OK;
}

HRESULT MyDirectDrawSurface7::GetUniquenessValue( LPDWORD lpValue ) {
    DebugWriteTex( "IDirectDrawSurface7(%p)::GetUniquenessValue()" );
    return S_OK;
}

HRESULT MyDirectDrawSurface7::ChangeUniquenessValue() {
    DebugWriteTex( "IDirectDrawSurface7(%p)::ChangeUniquenessValue()" );
    return S_OK;
}

HRESULT MyDirectDrawSurface7::SetPriority( DWORD dwPriority ) {
    DebugWriteTex( "IDirectDrawSurface7(%p)::SetPriority()" );
    return S_OK;
}

HRESULT MyDirectDrawSurface7::GetPriority( LPDWORD dwPriority ) {
    DebugWriteTex( "IDirectDrawSurface7(%p)::GetPriority()" );
    return S_OK;
}

HRESULT MyDirectDrawSurface7::SetLOD( DWORD dwLOD ) {
    DebugWriteTex( "IDirectDrawSurface7(%p)::SetLOD()" );
    return S_OK;
}

HRESULT MyDirectDrawSurface7::GetLOD( LPDWORD dwLOD ) {
    DebugWriteTex( "IDirectDrawSurface7(%p)::GetLOD()" );
    return S_OK;
}

/** Returns the name of this surface */
const std::string& MyDirectDrawSurface7::GetTextureName() {
    return TextureName;
}
