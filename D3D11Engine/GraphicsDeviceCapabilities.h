#pragma once
#include <string>

/** Backend-neutral snapshot of what the device the engine actually initialized on can do.
    Every backend fills one of these during Init(); the settings that follow the hardware are
    resolved from it by GothicRendererSettings::ApplyDeviceCapabilities. */
struct GraphicsDeviceCapabilities {
    std::string DeviceDescription;
    unsigned int VendorId = 0;

    /** A vendor/DXVK driver-extension interface was successfully obtained. Starts out true because
        the honest answer is only known after trying - the backend overwrites it with what it got. */
    bool DriverExtensions = true;
    /** MultiDrawIndexedInstancedIndirect is usable (D3D11: driver extension, D3D12: ExecuteIndirect). */
    bool MultiDrawIndirect = false;
    /** SV_RenderTargetArrayIndex can be written from a vertex shader, so the shadow cascades can be
        rendered layered instead of once per cascade or through a geometry shader. */
    bool LayeredRendering = false;
    /** UAV overlap barrier control is available (driver extension). */
    bool UAVOverlap = false;
    /** The device is below feature level 11_0 (or was forced down to it). */
    bool FeatureLevel10Only = false;
    /** Typed 16-bit texture formats work natively (D3D11: Windows 10 and up). */
    bool Native16BitTextures = false;
    /** D3D12: resource binding tier 3 + SM6.6 dynamic resources. */
    bool BindlessResources = false;
    /** D3D12: D3D12_OPTIONS12.EnhancedBarriersSupported. */
    bool EnhancedBarriers = false;
    /** R11G11B10_FLOAT works as a typed UAV, which is what the compressed scene-colour format needs
        (D3D12: D3D12_OPTIONS.TypedUAVLoadAdditionalFormats; D3D11 has always relied on it). */
    bool TypedUAVLoadAdditionalFormats = false;
};
