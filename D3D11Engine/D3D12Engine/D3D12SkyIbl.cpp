// D3D12GraphicsEngine — sky image-based lighting: the indirect-light source for the Forward+ PBR shaders.
//
// Motivation. Before this, PBRLighting.hlsl's ComputeSunLightingPBR applied one flat ambient term,
// `albedo * AmbientStrength * sunLum * shadowAO * ssao`. That is a greyscale constant with no directionality,
// no sky colour and — critically — NO indirect SPECULAR at all. At metallic = 1 the Cook-Torrance diffuse
// weight kD is exactly 0, so a metal surface received nothing but the direct sun/torch highlight and read as
// black everywhere else; roughness likewise had no effect whatsoever on the ambient response.
//
// Approach (Stage 1 of the environment-probe work: ONE global sky probe, no placement, no authoring).
// Three compute passes over two small cubemaps (Shaders/D3D12/SkyIbl.hlsl):
//   1. CSSkyRadiance  -> m_SkyEnvCube mip 0: the sky's radiance, evaluated analytically per texel.
//   2. CSPrefilter    -> m_SkyEnvCube mips 1..N: GGX importance-sampled; mip m == roughness m/(N-1).
//   3. CSIrradiance   -> m_SkyIrradCube: cosine-convolved diffuse irradiance.
// The lit shaders then do a standard split-sum lookup (see EvaluateSkyIBL in include/PBRLighting.hlsl).
//
// Why analytic rather than a scene capture. D3D12 renders Gothic's FIXED-FUNCTION skydome (DrawSky — the
// atmospheric-scattering path is D3D11-only and was never ported), so an atmospheric-scattering IBL would not
// match the sky actually on screen. Driving a gradient from Gothic's OWN zCSkyState master colours tracks time
// of day and weather exactly, costs one small dispatch instead of six scene re-renders, and needs no re-entrant
// render path. Localized, placed probes (BSP-sector-driven, parallax-corrected) are deliberately a later stage.
//
// Cost. ~1.05 MB of VA for the specular cube and ~12 KB for the irradiance cube — negligible against the 32-bit
// address-space budget, which is exactly why this stage comes before a probe grid.
#include "../pch.h"
#include "D3D12GraphicsEngine.h"
#include "../Engine.h"
#include "../GothicAPI.h"
#include "../GSky.h"
#include "../zCSkyController_Outdoor.h"
#include "../oCGame.h"

using Microsoft::WRL::ComPtr;
#include "D3D12EngineCommon.h"

namespace {
    constexpr DXGI_FORMAT kSkyCubeFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

    // Gothic's sky-state colours are gamma-encoded 0..255 bytes; the lighting math downstream is linear (the
    // shaders run SrgbToLinear on SunColor for the same reason). Convert once per frame on the CPU rather than
    // once per cube texel on the GPU.
    float SrgbToLinear( float c ) {
        c = std::clamp( c, 0.0f, 1.0f );
        return c <= 0.04045f ? c / 12.92f : std::pow( ( c + 0.055f ) / 1.055f, 2.4f );
    }
    XMFLOAT3 SrgbToLinear3( const XMFLOAT3& c ) {
        return XMFLOAT3( SrgbToLinear( c.x ), SrgbToLinear( c.y ), SrgbToLinear( c.z ) );
    }
    XMFLOAT3 Scale3( const XMFLOAT3& c, float s ) { return XMFLOAT3( c.x * s, c.y * s, c.z * s ); }

    bool NearlyEqual3( const XMFLOAT3& a, const XMFLOAT3& b, float eps ) {
        return std::fabs( a.x - b.x ) < eps && std::fabs( a.y - b.y ) < eps && std::fabs( a.z - b.z ) < eps;
    }

    // Calibration constant: converts the sky's LINEAR RADIANCE magnitude back onto the scale the flat ambient
    // term used to produce, so that SkyIblIntensity == 1.0 is neutral and every stock knob value keeps working.
    //
    // The old term was `albedo * AmbientStrength * sunLum`, where sunLum is the mean of the linearized
    // SunLightColor — and the default SunLightColor is pure white, so sunLum was exactly 1.0 and the whole
    // expression collapsed to `albedo * AmbientStrength`, a flat constant that never tracked the sky at all.
    // The IBL's diffuse term is instead `skyRadiance * albedo * kD`, and a linearized daytime sky sits around
    // 0.33, so without this the same AmbientStrength produced a roughly 3x darker result. 1/0.33 ~= 3.
    // Purely a unit conversion — retune only if the sky-colour sources below change.
    constexpr float kSkyIblNormalize = 3.0f;

    // Fraction of Gothic's polyColor taken as the terrain's average ALBEDO for the ground-bounce term. polyColor
    // is near-white by day (it is an ambient TINT, not a reflectance), and real ground — grass, dirt, rock — sits
    // around 0.2-0.35. Applied in LINEAR space, unlike the pre-decode 0.35 this replaces.
    constexpr float kGroundAlbedo = 0.35f;

    // Moonlight floor, in LINEAR radiance, scaled by RendererSettings.SkyIblNightFloor (which IS the green
    // channel; R and B follow this fixed blue-weighted ratio so the floor reads as moonlight rather than as a
    // grey underexposure). Applied per-channel as a max, so it is inert during the day and only lifts the night
    // trough.
    //
    // Why a floor is needed at all — from ZenGin's own zCSkyState presets:
    //   PresetNight1/2: fogColor (5,5,20)      -> ~0.002 linear. The horizon band is effectively black.
    //                   domeColor1 (55,55,155) -> ~(0.038, 0.038, 0.328) linear. Strongly blue, but only BLUE.
    //                   polyColor (40,60,210)  -> ~(0.021, 0.045, 0.645) linear.
    // So a faithful IBL gives night a reasonable BLUE channel and almost no red/green: correct as radiance,
    // but it reads as near-black in luminance (0.2126R + 0.7152G carry most of the perceived brightness, and
    // both are ~0.03). Gothic's night was never physically lit — the old flat ambient was a deliberate
    // sun-coloured fill, and D3D11's atmospheric scattering hardcodes the same idea
    // (AtmosphericScattering.h:138, nightColor = (0.2,0.2,0.4) * NIGHT_BRIGHTNESS = (0.4,0.4,0.8)). This floor
    // is that fill, expressed in the IBL's units.
    constexpr XMFLOAT3 kNightFloorTint = { 0.86f, 1.0f, 1.43f };   // relative; x SkyIblNightFloor
    XMFLOAT3 MaxFloor3( const XMFLOAT3& c, const XMFLOAT3& floorCol ) {
        return XMFLOAT3( ( std::max )( c.x, floorCol.x ), ( std::max )( c.y, floorCol.y ), ( std::max )( c.z, floorCol.z ) );
    }
}

bool D3D12GraphicsEngine::CreateSkyIblResources() {
    m_SkyIblResourcesReady = false;
    m_SkyIblValid = false;
    ID3D12Device* device = m_Device.GetDevice();
    if ( !device ) return false;

    for ( UINT m = 0; m < kSkyEnvMips; ++m ) m_SkyEnvUavSlot[m] = UINT_MAX;

    D3D12MA::ALLOCATION_DESC heapDefault = {};
    heapDefault.HeapType = D3D12_HEAP_TYPE_DEFAULT;

    // Both cubes are TEXTURE2D arrays of 6 slices — that is what a cube resource IS in D3D12; only the VIEWS
    // decide whether it reads back as a TextureCube (SRV) or a Texture2DArray (UAV; cube UAVs don't exist).
    auto makeCube = [&]( ComPtr<ID3D12Resource>& out, ComPtr<D3D12MA::Allocation>& outAlloc,
                         UINT size, UINT mips, const wchar_t* name ) -> bool {
        D3D12_RESOURCE_DESC dd = {};
        dd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        dd.Width = size;
        dd.Height = size;
        dd.DepthOrArraySize = 6;
        dd.MipLevels = static_cast<UINT16>( mips );
        dd.Format = kSkyCubeFormat;
        dd.SampleDesc.Count = 1;
        dd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        dd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if ( FAILED( m_Allocator->CreateResource( &heapDefault, &dd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            nullptr, outAlloc.ReleaseAndGetAddressOf(), IID_PPV_ARGS( out.ReleaseAndGetAddressOf() ) ) ) ) {
            LogWarn() << "D3D12: failed to create a sky-IBL cubemap (" << size << "^2, " << mips << " mips).";
            return false;
        }
        out->SetName( name );
        return true;
        };

    if ( !makeCube( m_SkyEnvCube, m_SkyEnvCubeAlloc, kSkyEnvSize, kSkyEnvMips, L"SkyEnvCube" ) ) return false;
    if ( !makeCube( m_SkyIrradCube, m_SkyIrradCubeAlloc, kSkyIrradSize, 1, L"SkyIrradianceCube" ) ) return false;

    auto ensureSlot = [&]( UINT& slot ) -> bool {
        if ( slot == UINT_MAX ) slot = AllocateSrvSlot();
        return slot != UINT_MAX;
        };
    if ( !ensureSlot( m_SkyEnvSrvSlot ) || !ensureSlot( m_SkyEnvMip0SrvSlot )
        || !ensureSlot( m_SkyIrradSrvSlot ) || !ensureSlot( m_SkyIrradUavSlot ) )
        return false;
    for ( UINT m = 0; m < kSkyEnvMips; ++m )
        if ( !ensureSlot( m_SkyEnvUavSlot[m] ) ) return false;

    // Full-chain cube SRV: what the lit pixel shaders sample bindlessly (roughness -> mip).
    D3D12_SHADER_RESOURCE_VIEW_DESC cubeSrv = {};
    cubeSrv.Format = kSkyCubeFormat;
    cubeSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
    cubeSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    cubeSrv.TextureCube.MipLevels = kSkyEnvMips;
    device->CreateShaderResourceView( m_SkyEnvCube.Get(), &cubeSrv, GetSrvCpuHandle( m_SkyEnvSrvSlot ) );

    // Mip-0-ONLY cube SRV: the prefilter/irradiance source. It must exclude mips 1..N because those are
    // simultaneously bound as UAVs in the very dispatch that reads this — see the per-subresource barriers in
    // RenderSkyIBL. A full-chain view here would be a read/write state conflict on one resource.
    D3D12_SHADER_RESOURCE_VIEW_DESC mip0Srv = cubeSrv;
    mip0Srv.TextureCube.MostDetailedMip = 0;
    mip0Srv.TextureCube.MipLevels = 1;
    device->CreateShaderResourceView( m_SkyEnvCube.Get(), &mip0Srv, GetSrvCpuHandle( m_SkyEnvMip0SrvSlot ) );

    cubeSrv.TextureCube.MipLevels = 1;
    device->CreateShaderResourceView( m_SkyIrradCube.Get(), &cubeSrv, GetSrvCpuHandle( m_SkyIrradSrvSlot ) );

    // One UAV per env mip, each covering all 6 faces as a Texture2DArray slice range.
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
    uav.Format = kSkyCubeFormat;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
    uav.Texture2DArray.ArraySize = 6;
    for ( UINT m = 0; m < kSkyEnvMips; ++m ) {
        uav.Texture2DArray.MipSlice = m;
        device->CreateUnorderedAccessView( m_SkyEnvCube.Get(), nullptr, &uav, GetSrvCpuHandle( m_SkyEnvUavSlot[m] ) );
    }
    uav.Texture2DArray.MipSlice = 0;
    device->CreateUnorderedAccessView( m_SkyIrradCube.Get(), nullptr, &uav, GetSrvCpuHandle( m_SkyIrradUavSlot ) );

    m_SkyEnvInReadState = false;   // freshly created above, born in UNORDERED_ACCESS
    m_SkyIblResourcesReady = true;
    return true;
}

void D3D12GraphicsEngine::UploadSkyIblConstants() {
    // Publishes the two bindless cube indices into the shadow CB the lit shaders already bind (the fourth
    // disjoint block — see the static_asserts). Written UNCONDITIONALLY every frame: a frame that skipped the
    // write would leave the previous frame's indices standing, and on the frame the IBL first becomes
    // unavailable that would be a stale-but-valid descriptor rather than the intended flat-ambient fallback.
    static_assert( kAoReprojCbOffset + kAoReprojCbReservedBytes == kSkyIblCbOffset,
        "sky-IBL CB block must start right after the (now unused) AO-reprojection hole" );
    static_assert( kSkyIblCbOffset + sizeof( SkyIblCBData ) <= 512, "shadow CB overflow" );
    if ( !m_ShadowCBMapped[m_FrameIndex] ) return;

    const auto& set = Engine::GAPI->GetRendererState().RendererSettings;
    const bool valid = m_SkyIblResourcesReady && m_SkyIblValid
        && m_SkyEnvSrvSlot != UINT_MAX && m_SkyIrradSrvSlot != UINT_MAX
        && set.SkyIblIntensity > 0.0f;

    SkyIblCBData cb = {};
    // 0xFFFFFFFF is the shaders' "no IBL, use the flat ambient" sentinel; it is never dereferenced.
    cb.IrradianceIndex = valid ? m_SkyIrradSrvSlot : 0xFFFFFFFFu;
    cb.SpecularIndex = valid ? m_SkyEnvSrvSlot : 0xFFFFFFFFu;
    cb.SpecularMips = static_cast<float>( kSkyEnvMips );

    // Intensity carries the COMPLETE ambient scale for the IBL path — the shader multiplies by this alone and
    // does NOT also apply AmbientStrength (that stays exclusive to the flat fallback branch). Two reasons:
    //
    //  * ShadowStrength is used UNHALVED here. D3D12ShadowMap::Prepare publishes AmbientStrength as
    //    `sunUp ? ShadowStrength : ShadowStrength * 0.5` — that halving exists to darken a flat, time-of-day-
    //    independent constant at night. The sky radiance driving the IBL already collapses on its own after
    //    dusk, so applying the halving too double-counts the darkness and is what drove nights to black.
    //  * It folds in kSkyIblNormalize, so SkyIblIntensity is a taste control at a neutral 1.0 rather than a
    //    unit conversion the user has to discover.
    cb.Intensity = set.SkyIblIntensity * kSkyIblNormalize * set.ShadowStrength;

    memcpy( m_ShadowCBMapped[m_FrameIndex] + kSkyIblCbOffset, &cb, sizeof( cb ) );
}

void D3D12GraphicsEngine::RenderSkyIBL() {
    // Called from OnStartWorldRendering after D3D12ShadowMap::Prepare (which computes the sun dir) and before the lit
    // geometry passes read the cubes. Rebuilds only when the sky state actually moved — the chain is ~100k
    // texels of analytic evaluation plus a ~1963-sample hemisphere march per irradiance texel, cheap in
    // absolute terms but pointless to repeat while the player stands still and the clock barely ticks.
    UploadSkyIblConstants();

    if ( !m_FrameOpen || !m_SkyIblResourcesReady
        || !m_Pipelines.SkyIbl.RadiancePSO || !m_Pipelines.SkyIbl.RadianceRootSig
        || !m_Pipelines.SkyIbl.PrefilterPSO || !m_Pipelines.SkyIbl.IrradiancePSO
        || !m_Pipelines.SkyIbl.FilterRootSig )
        return;

    const auto& set = Engine::GAPI->GetRendererState().RendererSettings;
    if ( set.SkyIblIntensity <= 0.0f ) return;   // knob at 0 = flat ambient only; don't pay for the dispatches

    // --- Resolve this frame's sky parameters from Gothic's own state --------------------------------------
    SkyIblParams p;

    // Indoor worlds see no sky. Suppressing the IBL there keeps the existing indoor lighting balance intact
    // (the CSM sampling CB already gives interiors a neutral sun + non-zero flat ambient) and
    // avoids reflecting an outdoor gradient onto cave walls.
    if ( auto* wi = Engine::GAPI->GetLoadedWorldInfo() )
        if ( wi->BspTree )
            p.Indoor = ( wi->BspTree->GetBspTreeMode() == zBSP_MODE_INDOOR );

    const auto& gs = Engine::GAPI->GetRendererState().GraphicsState;
    // FF_FogColor is refreshed every frame by GSky from zCSkyController_Outdoor's master state, so it already
    // tracks time of day and weather; it is the fallback for every colour below.
    XMFLOAT3 fogColor = gs.FF_FogColor;

    zCSkyController_Outdoor* sc = nullptr;
    if ( oCGame::GetGame() && oCGame::GetGame()->_zCSession_world )
        sc = oCGame::GetGame()->_zCSession_world->GetSkyControllerOutdoor();

    // Sun: the same values the CSM sampling CB feeds the direct term, so the IBL's sun lobe and the direct
    // highlight agree about where the sun is and how bright it is. Resolved BEFORE the sky colours because the
    // ground bounce below is a function of the sun.
    const float3 lp = Engine::GAPI->GetSky()->GetAtmosphereCB().AC_LightPos;
    const bool sunUp = ( lp.y > 0.0f );
    const float rain = Engine::GAPI->GetRainFXWeight();
    const float sunStrength = set.SunLightStrength
        + ( set.RainSunLightStrength - set.SunLightStrength ) * std::min( 1.0f, rain * 2.0f );

    p.SunDir = m_ShadowMap.GetSunDirWS();
    p.SunColor = SrgbToLinear3( XMFLOAT3( set.SunLightColor.x, set.SunLightColor.y, set.SunLightColor.z ) );
    p.SunIntensity = sunUp ? sunStrength : 0.0f;

    // Gamma-encoded 0..1 sources. NOTHING is scaled here: every tint/fraction below is applied AFTER the sRGB
    // decode, because a factor applied to a gamma-encoded value lands as roughly its 2.2nd power in linear light
    // (a "35% bounce" written pre-decode is really a 10% one — that was the original cause of the near-black
    // downward-facing surfaces this path produced in bright daylight).
    XMFLOAT3 horizonSrgb = fogColor;
    XMFLOAT3 groundSrgb = fogColor;
    XMFLOAT3 zenithSrgb = fogColor;
    bool haveZenith = false;
    if ( zCSkyState* ms = sc ? sc->GetMasterState() : nullptr ) {
        // zCSkyState colours are 0..255. Field roles are confirmed against ZenGin's own zCSkyState presets:
        //   domeColor1 = UPPER dome (zenith)   — e.g. (55,55,155) at night, (255,255,255) at dawn.
        //   domeColor0 = LOWER dome (horizon)  — every preset ends with `domeColor0 = fogColor`, so fogColor
        //                                        and domeColor0 are the same value and either may be read.
        //   polyColor  = the tint Gothic applies to WORLD POLYGONS — its own ambient-light colour, and the
        //                closest thing the game data has to an average terrain ALBEDO (which is how it is used
        //                below: as a reflectance, not as a radiance).
        horizonSrgb = Scale3( ms->FogColor, 1.0f / 255.0f );
        zenithSrgb = Scale3( ms->DomeColor1, 1.0f / 255.0f );
        groundSrgb = Scale3( ms->PolyColor, 1.0f / 255.0f );
        // Guard, not an expected path: no stock preset leaves domeColor1 black (the darkest is the night
        // (55,55,155)), but a mod's sky definition could, and a zero upper hemisphere would darken every
        // up-facing surface in the world.
        haveZenith = ( zenithSrgb.x + zenithSrgb.y + zenithSrgb.z >= 0.01f );
    }

    const XMFLOAT3 horizonLin = SrgbToLinear3( horizonSrgb );
    const XMFLOAT3 zenithLin = haveZenith
        ? SrgbToLinear3( zenithSrgb )
        : XMFLOAT3( horizonLin.x * 0.55f, horizonLin.y * 0.75f, horizonLin.z );   // blue-shifted fallback

    // --- Ground bounce ------------------------------------------------------------------------------------
    // The below-horizon hemisphere is what every downward- and sideways-facing normal integrates: cave
    // ceilings, the underside of a rock overhang, and — the visible one — the drooping leaf cards of a tree
    // canopy, whose geometric normals point down and sideways far more often than up.
    //
    // Deriving it from polyColor ALONE (as this used to) is wrong by construction: polyColor is a reflectance,
    // so treating it as radiance means the ground bounce never brightens when the sun comes up. At noon the
    // terrain is the second-brightest thing in the scene, yet the probe's lower hemisphere stayed at a dim
    // fraction of an ambient tint, and any surface facing it went nearly black under a blazing sky.
    //
    // So bounce it properly: albedo x the irradiance actually landing on the terrain. Sky irradiance is
    // pi * (the mean sky radiance over the ground's upward hemisphere), and the sun contributes
    // sunColor * intensity * N.L for a horizontal N (i.e. the sun's height). Dividing that sum by pi converts
    // the Lambertian exitance back to the RADIANCE units this cube stores.
    const XMFLOAT3 skyMeanLin = Scale3( XMFLOAT3( horizonLin.x + zenithLin.x,
                                                  horizonLin.y + zenithLin.y,
                                                  horizonLin.z + zenithLin.z ), 0.5f );
    const float sunOnGround = p.SunIntensity * std::clamp( p.SunDir.y, 0.0f, 1.0f ) * ( 1.0f / 3.14159265f );
    const XMFLOAT3 groundAlbedo = Scale3( SrgbToLinear3( groundSrgb ), kGroundAlbedo );
    const XMFLOAT3 groundLin = XMFLOAT3(
        groundAlbedo.x * ( skyMeanLin.x + p.SunColor.x * sunOnGround ),
        groundAlbedo.y * ( skyMeanLin.y + p.SunColor.y * sunOnGround ),
        groundAlbedo.z * ( skyMeanLin.z + p.SunColor.z * sunOnGround ) );

    // Floor at moonlight. The floor is specified in linear radiance (see kNightFloorTint), which is why it is
    // applied here rather than to the gamma-encoded sources — flooring those would lift daytime colours too.
    const float nightFloor = ( std::max )( set.SkyIblNightFloor, 0.0f );
    const XMFLOAT3 floorCol = Scale3( kNightFloorTint, nightFloor );
    p.Horizon = MaxFloor3( horizonLin, floorCol );
    p.Zenith = MaxFloor3( zenithLin, floorCol );
    // The ground bounce stays below the sky it is bouncing: a floor as bright as the horizon's would make
    // downward-facing normals read as lit from underneath on a dark night. It carries the whole night-time
    // lower hemisphere on its own, because the product above collapses to ~0 once the sun sets (a dim sky
    // bouncing off a dim albedo is doubly dim — correct physically, useless for a game Gothic never lit that way).
    p.Ground = MaxFloor3( groundLin, Scale3( floorCol, 0.5f ) );

    // --- Dirty check --------------------------------------------------------------------------------------
    // Colours move continuously through Gothic's day cycle, so an exact compare would rebuild every frame. The
    // epsilon is deliberately loose: this is very low-frequency lighting and a visible step would need a much
    // larger delta than these thresholds allow through.
    const bool dirty = !m_SkyIblValid
        || p.Indoor != m_SkyLastParams.Indoor
        || !NearlyEqual3( p.Zenith, m_SkyLastParams.Zenith, 0.004f )
        || !NearlyEqual3( p.Horizon, m_SkyLastParams.Horizon, 0.004f )
        || !NearlyEqual3( p.Ground, m_SkyLastParams.Ground, 0.004f )
        || !NearlyEqual3( p.SunDir, m_SkyLastParams.SunDir, 0.01f )
        || !NearlyEqual3( p.SunColor, m_SkyLastParams.SunColor, 0.004f )
        || std::fabs( p.SunIntensity - m_SkyLastParams.SunIntensity ) > 0.01f;
    if ( !dirty ) return;

    DX_ZONE( m_CmdList.Get(), "Sky IBL" );

    // --- Pass 1: analytic radiance -> env cube mip 0 -------------------------------------------------------
    // BOTH cubes go back to UNORDERED_ACCESS: the flag covers the pair because the tail of this function
    // flips them to PIXEL_SHADER_RESOURCE together. Transitioning only the env cube left the irradiance
    // cube in PIXEL_SHADER_RESOURCE while pass 3 wrote it through a UAV, and made the tail barrier below
    // claim a "before" state it was not in (RESOURCE_BARRIER_BEFORE_AFTER_MISMATCH on every rebuild — i.e.
    // continuously, as Gothic's day cycle keeps the sky colours dirty).
    if ( m_SkyEnvInReadState ) {
        D3D12_RESOURCE_BARRIER toUav[2] = {
            TransitionBarrier( m_SkyEnvCube.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS ),
            TransitionBarrier( m_SkyIrradCube.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS ),
        };
        m_CmdList->ResourceBarrier( 2, toUav );
        m_SkyEnvInReadState = false;
    }

    struct SkyRadianceCB {
        float Zenith[3];   float SunSharpness;
        float Horizon[3];  float SkyIntensity;
        float Ground[3];   float GroundBlend;
        float SunDir[3];   float SunLobeIntensity;
        float SunColor[3]; float FaceSize;
    } rcb = {};
    rcb.Zenith[0] = p.Zenith.x; rcb.Zenith[1] = p.Zenith.y; rcb.Zenith[2] = p.Zenith.z;
    rcb.Horizon[0] = p.Horizon.x; rcb.Horizon[1] = p.Horizon.y; rcb.Horizon[2] = p.Horizon.z;
    rcb.Ground[0] = p.Ground.x; rcb.Ground[1] = p.Ground.y; rcb.Ground[2] = p.Ground.z;
    rcb.SunDir[0] = p.SunDir.x; rcb.SunDir[1] = p.SunDir.y; rcb.SunDir[2] = p.SunDir.z;
    rcb.SunColor[0] = p.SunColor.x; rcb.SunColor[1] = p.SunColor.y; rcb.SunColor[2] = p.SunColor.z;
    // A tight lobe (high exponent) keeps the sun a small bright disc rather than a broad wash — the disc is
    // what a low-roughness surface reflects, while the broad wash would double-count the direct sun term.
    rcb.SunSharpness = 256.0f;
    rcb.SunLobeIntensity = p.SunIntensity;
    rcb.GroundBlend = 0.6f;
    // Indoors the sky contributes nothing; build a black cube rather than skipping the passes, so the shaders
    // do not have to distinguish "no IBL" from "IBL that happens to be dark" and the indoor flat ambient in
    // the CSM sampling CB stays the only interior ambient (unchanged behaviour).
    rcb.SkyIntensity = p.Indoor ? 0.0f : 1.0f;
    rcb.FaceSize = static_cast<float>( kSkyEnvSize );
    static_assert( sizeof( SkyRadianceCB ) == 20 * sizeof( float ), "SkyRadianceCB must match the 20 root constants" );

    const UINT g0 = ( kSkyEnvSize + 7 ) / 8;
    m_CmdList->SetPipelineState( m_Pipelines.SkyIbl.RadiancePSO.Get() );
    m_CmdList->SetComputeRootSignature( m_Pipelines.SkyIbl.RadianceRootSig.Get() );
    m_CmdList->SetComputeRoot32BitConstants( 0, 20, &rcb, 0 );
    m_CmdList->SetComputeRootDescriptorTable( 1, GetSrvGpuHandle( m_SkyEnvUavSlot[0] ) );
    m_CmdList->Dispatch( g0, g0, 6 );   // .z = cube face

    // Mip 0 alone flips to a shader-read state; mips 1..N stay UNORDERED_ACCESS because the very next dispatch
    // writes them while sampling mip 0. That is why m_SkyEnvMip0SrvSlot is a mip-0-only view — a full-chain SRV
    // would require the whole resource in one state.
    {
        D3D12_RESOURCE_BARRIER b = TransitionBarrier( m_SkyEnvCube.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE );
        // Subresource index for a mip-mapped array is mip + face * mipCount — mip 0 of all 6 faces.
        for ( UINT face = 0; face < 6; ++face ) {
            b.Transition.Subresource = face * kSkyEnvMips + 0;
            m_CmdList->ResourceBarrier( 1, &b );
        }
    }

    // --- Pass 2: GGX prefilter -> env cube mips 1..N -------------------------------------------------------
    struct PrefilterCB { float FaceSize; float Roughness; float SourceSize; UINT NumSamples; } pcb = {};
    pcb.SourceSize = static_cast<float>( kSkyEnvSize );

    m_CmdList->SetPipelineState( m_Pipelines.SkyIbl.PrefilterPSO.Get() );
    m_CmdList->SetComputeRootSignature( m_Pipelines.SkyIbl.FilterRootSig.Get() );
    m_CmdList->SetComputeRootDescriptorTable( 1, GetSrvGpuHandle( m_SkyEnvMip0SrvSlot ) );

    for ( UINT m = 1; m < kSkyEnvMips; ++m ) {
        const UINT mipSize = kSkyEnvSize >> m;
        pcb.FaceSize = static_cast<float>( mipSize );
        pcb.Roughness = static_cast<float>( m ) / static_cast<float>( kSkyEnvMips - 1 );
        // Rougher mips integrate a wider lobe and so need more samples, but they are also much smaller — 32 at
        // mip 1 up to 128 at the last one keeps total work roughly flat across the chain.
        pcb.NumSamples = 32u << std::min( m - 1, 2u );
        m_CmdList->SetComputeRoot32BitConstants( 0, 4, &pcb, 0 );
        m_CmdList->SetComputeRootDescriptorTable( 2, GetSrvGpuHandle( m_SkyEnvUavSlot[m] ) );
        const UINT g = ( mipSize + 7 ) / 8;
        m_CmdList->Dispatch( g, g, 6 );
    }

    // --- Pass 3: cosine convolution -> irradiance cube -----------------------------------------------------
    // Reads the same mip-0 SRV (still bound at root param 1) and writes a separate resource, so no barrier is
    // needed between this and the prefilter dispatches above.
    pcb.FaceSize = static_cast<float>( kSkyIrradSize );
    pcb.Roughness = 1.0f;
    pcb.NumSamples = 0;   // unused by CSIrradiance (its march is a fixed angular step)
    m_CmdList->SetPipelineState( m_Pipelines.SkyIbl.IrradiancePSO.Get() );
    m_CmdList->SetComputeRoot32BitConstants( 0, 4, &pcb, 0 );
    m_CmdList->SetComputeRootDescriptorTable( 2, GetSrvGpuHandle( m_SkyIrradUavSlot ) );
    const UINT gi = ( kSkyIrradSize + 7 ) / 8;
    m_CmdList->Dispatch( gi, gi, 6 );

    // --- Hand both cubes to the lit pixel shaders ----------------------------------------------------------
    // Mip 0 is currently NON_PIXEL_SHADER_RESOURCE and mips 1..N UNORDERED_ACCESS; bring the whole resource to
    // PIXEL_SHADER_RESOURCE with per-subresource transitions that state the correct "before" for each.
    {
        std::vector<D3D12_RESOURCE_BARRIER> barriers;
        barriers.reserve( 6 * kSkyEnvMips + 1 );
        for ( UINT face = 0; face < 6; ++face ) {
            for ( UINT m = 0; m < kSkyEnvMips; ++m ) {
                auto b = TransitionBarrier( m_SkyEnvCube.Get(),
                    m == 0 ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE );
                b.Transition.Subresource = face * kSkyEnvMips + m;
                barriers.push_back( b );
            }
        }
        barriers.push_back( TransitionBarrier( m_SkyIrradCube.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE ) );
        m_CmdList->ResourceBarrier( static_cast<UINT>( barriers.size() ), barriers.data() );
    }

    m_SkyEnvInReadState = true;
    m_SkyLastParams = p;
    m_SkyIblValid = true;

    // The indices published at the top of this call were computed from the PREVIOUS m_SkyIblValid, so on the
    // very first build of a world they said "no IBL". Re-publish now that the cubes hold real data — the lit
    // passes have not been recorded yet this frame, so they will see the corrected block.
    UploadSkyIblConstants();
}
