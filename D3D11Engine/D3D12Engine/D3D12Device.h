#pragma once
#include <string>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

/** Owns the core D3D12 device objects: DXGI factory, selected adapter, the device, and the
    direct + copy command queues.

    Device creation uses dynamic d3d12.dll / dxgi.dll loading (mirroring the D3D11 backend's
    dynamic dxgi/d3d11 approach) so D3D12 stays a *soft* dependency: a machine without it — or
    a GPU below Feature Level 11_0 — simply falls back to the D3D11 backend. This class is the
    foundation the D3D12GraphicsEngine builds swapchain / descriptors / upload rings on top of. */
class D3D12Device {
public:
    /** Cheap availability probe: dynamically loads d3d12.dll, selects the best adapter and
        attempts a throwaway Feature-Level-11_0 device on it. Fills outDescription / outReason
        when provided. Retains nothing — safe to call before committing to the D3D12 backend
        (used by Engine::CreateGraphicsEngine to decide between backends). */
    static bool IsAvailable( std::string* outDescription = nullptr, std::string* outReason = nullptr );

    D3D12Device() = default;
    ~D3D12Device() = default;

    D3D12Device( const D3D12Device& ) = delete;
    D3D12Device& operator=( const D3D12Device& ) = delete;

    /** Creates the DXGI factory, selects the best adapter, creates the device (FL11_0 minimum)
        and the direct + copy command queues. Returns false (having logged the reason) on any
        failure so the caller can fall back to D3D11. */
    bool Init();

    const std::string& GetDeviceDescription() const { return m_DeviceDescription; }

    ID3D12Device*       GetDevice()      const { return m_Device.Get(); }
    ID3D12CommandQueue* GetDirectQueue() const { return m_DirectQueue.Get(); }
    ID3D12CommandQueue* GetCopyQueue()   const { return m_CopyQueue.Get(); }
    IDXGIFactory4*      GetFactory()     const { return m_Factory.Get(); }
    IDXGIAdapter1*      GetAdapter()     const { return m_Adapter.Get(); }

private:
    Microsoft::WRL::ComPtr<IDXGIFactory4>       m_Factory;
    Microsoft::WRL::ComPtr<IDXGIAdapter1>       m_Adapter;
    Microsoft::WRL::ComPtr<ID3D12Device>        m_Device;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue>  m_DirectQueue;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue>  m_CopyQueue;
    std::string m_DeviceDescription;
};
