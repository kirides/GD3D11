#include "../pch.h"
#include "D3D12Device.h"
#include "D3D12TracyDebug.h"

#include <map>

using Microsoft::WRL::ComPtr;

namespace {

    UINT agilitySdkVersion = 0;
    const char* agilitySdkDeployPath = "GD3D11\\D3D12\\";

    UINT GetD3D12CoreSDKVersion( const std::string& dllDirectory ) {
        std::string coreDllPath = dllDirectory + "d3d12Core.dll";

        // Load your custom d3d12Core.dll temporarily to read its metadata
        HMODULE hCore = LoadLibraryExA( coreDllPath.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS );
        if ( !hCore ) {
            return 0; // File not found or failed to load
        }

        // Retrieve the exported version symbol
        auto pSDKVersion = reinterpret_cast<const UINT*>(GetProcAddress( hCore, "D3D12SDKVersion" ));
        UINT version = pSDKVersion ? *pSDKVersion : 0;

        FreeLibrary( hCore );
        return version;
    }

    std::string GetAgilitySdkPath() {
        std::string sdkPath;
        sdkPath.resize( MAX_PATH );
        sdkPath.resize( GetModuleFileNameA( nullptr, sdkPath.data(), sdkPath.size() - 1 ) );
        auto lastTerminator = sdkPath.find_last_of( '\\' );
        if ( lastTerminator != std::string::npos ) {
            sdkPath.erase( lastTerminator+1 ); // keep the \, so we can just append agilitySdkDeployPath
        }
        sdkPath.append( agilitySdkDeployPath );
        return sdkPath;
    }

    void InitAgilitySdkVersion() {
        agilitySdkVersion = GetD3D12CoreSDKVersion( GetAgilitySdkPath() );
    }

    void InitAgilitySdk() {
        InitAgilitySdkVersion();
        if ( agilitySdkVersion != 0 ) {

            if ( auto hD3d12 = LoadLibraryA( "d3d12.dll" ) ) {
                auto pfnD3D12GetInterface = reinterpret_cast<PFN_D3D12_GET_INTERFACE>(GetProcAddress( hD3d12, "D3D12GetInterface" ));

                if ( pfnD3D12GetInterface ) {
                    Microsoft::WRL::ComPtr<ID3D12SDKConfiguration> sdkConfig;
                    HRESULT hr = pfnD3D12GetInterface( CLSID_D3D12SDKConfiguration, IID_PPV_ARGS( &sdkConfig ) );
                    if ( SUCCEEDED( hr ) && sdkConfig ) {
                        hr = sdkConfig->SetSDKVersion( agilitySdkVersion, agilitySdkDeployPath );
                        if ( !SUCCEEDED( hr ) ) {
                            LogWarn() << "Failed to initialize agility SDK (version " << agilitySdkVersion << ", result: " << hr << " ); using system D3D12.";
                        } else {
                            LogInfo() << "D3D12 Agility SDK " << agilitySdkVersion << " activated from " << agilitySdkDeployPath;
                        }
                    }
                }
            }
        } else {
            LogInfo() << "D3D12 Agility SDK core not deployed; using the system D3D12 runtime.";
        }
    }

    // dxgi.dll factory-creation function pointers (dynamically resolved, like the D3D11 backend).
    typedef HRESULT( WINAPI* PFN_CREATE_DXGI_FACTORY1 )( REFIID riid, void** ppFactory );
    typedef HRESULT( WINAPI* PFN_CREATE_DXGI_FACTORY2 )( UINT flags, REFIID riid, void** ppFactory );

    /** Dynamically loads d3d12.dll and resolves D3D12CreateDevice. Returns nullptr if the DLL or
        the entry point is missing (i.e. the OS has no D3D12), so the caller can fall back. */
    PFN_D3D12_CREATE_DEVICE LoadD3D12CreateDevice() {
        HMODULE d3d12 = LoadLibraryA( "d3d12.dll" );
        if ( !d3d12 ) return nullptr;
        return reinterpret_cast<PFN_D3D12_CREATE_DEVICE>( GetProcAddress( d3d12, "D3D12CreateDevice" ) );
    }

    /** Creates a DXGI factory via dynamically-resolved dxgi.dll exports (CreateDXGIFactory2, else
        CreateDXGIFactory1). Returns false if dxgi.dll or both entry points are unavailable. */
    bool CreateFactory( ComPtr<IDXGIFactory4>& outFactory ) {
        HMODULE dxgi = LoadLibraryA( "dxgi.dll" );
        if ( !dxgi ) return false;

        auto CreateDXGIFactory1Func = reinterpret_cast<PFN_CREATE_DXGI_FACTORY1>( GetProcAddress( dxgi, "CreateDXGIFactory1" ) );
        auto CreateDXGIFactory2Func = reinterpret_cast<PFN_CREATE_DXGI_FACTORY2>( GetProcAddress( dxgi, "CreateDXGIFactory2" ) );
        if ( !CreateDXGIFactory1Func && !CreateDXGIFactory2Func ) return false;

        UINT flags = 0;
#ifdef DEBUG_D3D11
        flags |= DXGI_CREATE_FACTORY_DEBUG;
#endif
        HRESULT hr = CreateDXGIFactory2Func
            ? CreateDXGIFactory2Func( flags, IID_PPV_ARGS( outFactory.ReleaseAndGetAddressOf() ) )
            : CreateDXGIFactory1Func( IID_PPV_ARGS( outFactory.ReleaseAndGetAddressOf() ) );
        return SUCCEEDED( hr );
    }

    /** Returns true if the adapter supports a FL11_0 D3D12 device (capability check only — passing
        nullptr for the device output means "test support without creating"). */
    bool AdapterSupportsD3D12( PFN_D3D12_CREATE_DEVICE createDevice, IDXGIAdapter1* adapter ) {
        Microsoft::WRL::ComPtr<ID3D12Device> tempDevice;

        // Actually attempt to create a temporary device instance
        HRESULT hr = createDevice(
            adapter,
            D3D_FEATURE_LEVEL_12_0,
            __uuidof(ID3D12Device),
            IID_PPV_ARGS_Helper( &tempDevice )
        );

        return SUCCEEDED( hr );
    }

    std::string DescriptionToNarrow( const DXGI_ADAPTER_DESC1& desc ) {
        std::wstring w( desc.Description );
        return std::string( w.begin(), w.end() );
    }

    /** Selects the best FL11_0-capable, non-software adapter. Prefers the high-performance GPU via
        IDXGIFactory6 when available, else rates candidates by VRAM + vendor (NVIDIA > AMD > Intel),
        mirroring the D3D11 backend's adapter heuristic. Fills outDescription on success. */
    bool SelectAdapter( IDXGIFactory4* factory, PFN_D3D12_CREATE_DEVICE createDevice,
        ComPtr<IDXGIAdapter1>& outAdapter, std::string& outDescription ) {
        ComPtr<IDXGIAdapter1> adapter;

        ComPtr<IDXGIFactory6> factory6;
        if ( SUCCEEDED( factory->QueryInterface( IID_PPV_ARGS( factory6.ReleaseAndGetAddressOf() ) ) ) ) {
            for ( UINT i = 0; factory6->EnumAdapterByGpuPreference( i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                IID_PPV_ARGS( adapter.ReleaseAndGetAddressOf() ) ) != DXGI_ERROR_NOT_FOUND; ++i ) {
                DXGI_ADAPTER_DESC1 desc;
                adapter->GetDesc1( &desc );
                if ( desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE ) continue;
                if ( AdapterSupportsD3D12( createDevice, adapter.Get() ) ) {
                    outAdapter = adapter;
                    outDescription = DescriptionToNarrow( desc );
                    return true;
                }
            }
        }

        // Fallback: rate by dedicated VRAM + vendor and pick the best that supports D3D12.
        std::map<uint64_t, ComPtr<IDXGIAdapter1>> candidates;
        std::map<uint64_t, std::string> descriptions;
        for ( UINT i = 0; factory->EnumAdapters1( i, adapter.ReleaseAndGetAddressOf() ) != DXGI_ERROR_NOT_FOUND; ++i ) {
            DXGI_ADAPTER_DESC1 desc;
            adapter->GetDesc1( &desc );
            if ( desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE ) continue;
            if ( !AdapterSupportsD3D12( createDevice, adapter.Get() ) ) continue;

            uint64_t rating = static_cast<uint64_t>( desc.DedicatedVideoMemory );
            if ( desc.VendorId == 0x10DE ) rating += 0x200000000; // NVIDIA
            else if ( desc.VendorId == 0x1002 ) rating += 0x100000000; // AMD > Intel IGPU
            candidates.emplace( rating, adapter );
            descriptions.emplace( rating, DescriptionToNarrow( desc ) );
        }

        if ( candidates.empty() ) return false;
        outAdapter = candidates.rbegin()->second;
        outDescription = descriptions.rbegin()->second;
        return true;
    }

} // namespace

bool D3D12Device::IsAvailable( std::string* outDescription, std::string* outReason ) {
    InitAgilitySdk();

    auto setReason = [&]( const char* r ) { if ( outReason ) *outReason = r; };

    PFN_D3D12_CREATE_DEVICE createDevice = LoadD3D12CreateDevice();
    if ( !createDevice ) {
        setReason( "d3d12.dll is not available (Windows 10 or newer with a D3D12 driver is required)" );
        return false;
    }

    ComPtr<IDXGIFactory4> factory;
    if ( !CreateFactory( factory ) ) {
        setReason( "failed to create a DXGI factory" );
        return false;
    }

    ComPtr<IDXGIAdapter1> adapter;
    std::string description;
    if ( !SelectAdapter( factory.Get(), createDevice, adapter, description ) ) {
        setReason( "no Feature-Level-11_0-capable GPU was found" );
        return false;
    }

    if ( outDescription ) *outDescription = description;
    return true;
}

bool D3D12Device::Init() {
    InitAgilitySdk();
    
    PFN_D3D12_CREATE_DEVICE createDevice = LoadD3D12CreateDevice();
    if ( !createDevice ) {
        LogWarn() << "D3D12Device::Init: d3d12.dll / D3D12CreateDevice unavailable.";
        return false;
    }

#ifdef DEBUG_D3D11
    // Enable the debug layer before device creation when available (best-effort).
    if ( HMODULE d3d12 = GetModuleHandleA( "d3d12.dll" ) ) {
        auto getDebug = reinterpret_cast<PFN_D3D12_GET_DEBUG_INTERFACE>( GetProcAddress( d3d12, "D3D12GetDebugInterface" ) );
        ComPtr<ID3D12Debug> debug;
        if ( getDebug && SUCCEEDED( getDebug( IID_PPV_ARGS( debug.ReleaseAndGetAddressOf() ) ) ) ) {
            debug->EnableDebugLayer();
            LogInfo() << "D3D12 debug layer enabled.";
        }

        ComPtr<ID3D12Debug1> debug1;
        if ( SUCCEEDED( debug.As( &debug1 ) ) ) {
            debug1->SetEnableGPUBasedValidation( FALSE ); // NOTE: This is REALLY expensive. Only use when actually debugging hard crashes.
        }

        // Enable DRED
        ComPtr<ID3D12DeviceRemovedExtendedDataSettings1> pDredSettings;
        if ( getDebug && SUCCEEDED( getDebug( IID_PPV_ARGS( pDredSettings.ReleaseAndGetAddressOf() ) ) ) ) {
            // Turn on auto-breadcrumbs and page fault reporting
            pDredSettings->SetAutoBreadcrumbsEnablement( D3D12_DRED_ENABLEMENT_FORCED_ON );
            pDredSettings->SetBreadcrumbContextEnablement( D3D12_DRED_ENABLEMENT_FORCED_ON );
            pDredSettings->SetPageFaultEnablement( D3D12_DRED_ENABLEMENT_FORCED_ON );
            pDredSettings->Release();
        }
    }
#endif

    if ( !CreateFactory( m_Factory ) ) {
        LogWarn() << "D3D12Device::Init: failed to create DXGI factory.";
        return false;
    }

    if ( !SelectAdapter( m_Factory.Get(), createDevice, m_Adapter, m_DeviceDescription ) ) {
        LogWarn() << "D3D12Device::Init: no Feature-Level-12_0-capable GPU found.";
        return false;
    }

    HRESULT hr = createDevice( m_Adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS( m_Device.ReleaseAndGetAddressOf() ) );
    if ( FAILED( hr ) ) {
        LogWarn() << "D3D12Device::Init: D3D12CreateDevice failed with code 0x" << std::hex << hr << ".";
        return false;
    }
    LogInfo() << "D3D12 device created on: " << m_DeviceDescription.c_str();

    // Direct (graphics) queue
    D3D12_COMMAND_QUEUE_DESC directDesc = {};
    directDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    directDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    hr = m_Device->CreateCommandQueue( &directDesc, IID_PPV_ARGS( m_DirectQueue.ReleaseAndGetAddressOf() ) );
    if ( FAILED( hr ) ) {
        LogWarn() << "D3D12Device::Init: failed to create the direct command queue (0x" << std::hex << hr << ").";
        return false;
    }

    // Copy queue (async texture / buffer uploads)
    D3D12_COMMAND_QUEUE_DESC copyDesc = {};
    copyDesc.Type = D3D12_COMMAND_LIST_TYPE_COPY;
    copyDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    hr = m_Device->CreateCommandQueue( &copyDesc, IID_PPV_ARGS( m_CopyQueue.ReleaseAndGetAddressOf() ) );
    if ( FAILED( hr ) ) {
        LogWarn() << "D3D12Device::Init: failed to create the copy command queue (0x" << std::hex << hr << ").";
        return false;
    }

    s_tracyD3D12Ctx = TracyD3D12Context( m_Device.Get(), m_DirectQueue.Get() );
    return true;
}
