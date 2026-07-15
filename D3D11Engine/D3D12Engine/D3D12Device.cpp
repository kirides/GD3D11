#include "../pch.h"
#include "D3D12Device.h"

#include <map>

using Microsoft::WRL::ComPtr;

namespace {

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
#ifdef _DEBUG
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
        return SUCCEEDED( createDevice( adapter, D3D_FEATURE_LEVEL_11_0, __uuidof( ID3D12Device ), nullptr ) );
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
    PFN_D3D12_CREATE_DEVICE createDevice = LoadD3D12CreateDevice();
    if ( !createDevice ) {
        LogWarn() << "D3D12Device::Init: d3d12.dll / D3D12CreateDevice unavailable.";
        return false;
    }

#ifdef _DEBUG
    // Enable the debug layer before device creation when available (best-effort).
    if ( HMODULE d3d12 = GetModuleHandleA( "d3d12.dll" ) ) {
        auto getDebug = reinterpret_cast<PFN_D3D12_GET_DEBUG_INTERFACE>( GetProcAddress( d3d12, "D3D12GetDebugInterface" ) );
        ComPtr<ID3D12Debug> debug;
        if ( getDebug && SUCCEEDED( getDebug( IID_PPV_ARGS( debug.ReleaseAndGetAddressOf() ) ) ) ) {
            debug->EnableDebugLayer();
            LogInfo() << "D3D12 debug layer enabled.";
        }
    }
#endif

    if ( !CreateFactory( m_Factory ) ) {
        LogWarn() << "D3D12Device::Init: failed to create DXGI factory.";
        return false;
    }

    if ( !SelectAdapter( m_Factory.Get(), createDevice, m_Adapter, m_DeviceDescription ) ) {
        LogWarn() << "D3D12Device::Init: no Feature-Level-11_0-capable GPU found.";
        return false;
    }

    HRESULT hr = createDevice( m_Adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS( m_Device.ReleaseAndGetAddressOf() ) );
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

    return true;
}
