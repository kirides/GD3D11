# GD3D11 (Gothic Direct3D 11) Renderer [![GitHub Actions](https://img.shields.io/github/actions/workflow/status/kirides/GD3D11/build-and-conditional-release.yml?logo=github&labelColor=24292e&color=28A745)](https://github.com/kirides/GD3D11/actions) [![GitHub latest release](https://img.shields.io/github/v/release/kirides/GD3D11?logo=github&labelColor=24292e&color=0969da)](https://github.com/kirides/GD3D11/releases/latest) [![GitHub nightly release](https://img.shields.io/github/v/release/kirides/GD3D11?include_prereleases&filter=nightly&logo=github&label=dev-release&labelColor=24292e&color=bf8700)](https://github.com/kirides/GD3D11/releases/tag/nightly)

This mod for the games **Gothic** and **Gothic II** brings the engine of those games into a more modern state. Through a custom implementation of the DirectDraw-API and using hooking and assembler-code-modifications of Gothic's internal engine calls, this mod completely replaces Gothic's old rendering architecture.

The new renderer is able to utilize more of the current GPU generation's power. Since Gothic's engine in its original state tries to cull as much as possible, this takes a lot of work from the CPU, which was slowing down the game even on today's processors. While the original renderer did a really great job with the tech from 2002, GPUs have grown much faster. And now, that they can actually use their power to render, we not only get a big performance boost on most systems, but also more features:

* Shadows
  * Cascaded Shadow Maps for the sun, and for the moon at night
  * Point light shadows for torches, campfires and spells
* Lighting
  * Full dynamic lighting, optionally with clustered (compute shader) light culling
  * Atmospheric Scattering, including a night sky with the moon
  * Normalmapping (OpenGL and DirectX style normal maps, BC5 compressed normal maps)
  * Ambient Occlusion: HBAO+, SAO or ASSAO
* Water & Weather
  * Volumetric water with refractions, screen space reflections, waves and a configurable ocean color
  * Underwater effect
  * Rain with wet surfaces, puddles, ripples and splashes
  * Low clouds drifting above the valleys and along the horizon
  * Heightfog, also applied to transparent surfaces and particle effects
* Image Quality
  * HDR rendering with several tone mapping curves
  * Bloom, God Rays and Depth of Field
  * Anti-Aliasing: SMAA, TAA or FSR 3
  * Resolution scaling with FSR 1 or FSR 3 upscaling, and sharpening (Simple or CAS)
* World
  * Increased draw distance, set separately for the world, objects, NPCs and effects
  * Wind animation for trees, grass and wheat, which also react to the player walking through them (Gothic 2, and Gothic 1 when a patch provides wind animations)
  * Highlighting of the focused object in Gothic 1
  * Vegetationgeneration
* Performance
  * Increased Performance
  * Meshes are loaded on background threads
  * On-disk shader cache for faster startup
  * Batched rendering of Gothic's 2D UI, and an optional faster inventory renderer
* Interface & Tools
  * Settings menu (F11) with graphics presets, preview images and per-world settings, plus advanced settings (CTRL+F11)
  * Editor-Panel (F1, requires the `-XEnableEditorPanel` command line parameter) to insert some of the renderers features into the world
  * Support for the Spacer.NET world editor
  * Rewritten bink player for better compatibility with bink videos
* Display
  * FPS-Limiter, plus a separate limit while the game is paused
  * Low-Latency borderless fullscreen
    Frame latency on a 144Hz refresh rate with v-sync
    * Borderless Fullscreen: ~28ms
    * Borderless LowLatency: ~10ms

## Installation & Usage
> [!NOTE]
> In the past there used to be separate files for Gothic 1 and Gothic 2, this has now changed since the mod will automatically detect the game.
> Only Gothic 1 1.08k (1.30.0.0) and Gothic 2 Night of the Raven 2.6 (2.6.0.0-rev2) are supported. https://www.worldofgothic.de/dl/download_278.htm
1. Download the **GD3D11-*VERSION*.zip** file from the **Assets** section in the latest release of this repository (e.g. [kirides/releases](https://github.com/kirides/GD3D11/releases/latest)).
2. Unpack the zip file and copy the content into the `Gothic\system\` or `Gothic2\system\` game folder.
3. When starting the game you should see the version number of GD3D11 in the top-left corner.
4. As soon as you start the game for the first time after the installation you should press F11 to open the settings menu, choose a `Graphics Preset` and press `Save Settings`. This saves all the options to `Gothic(2)\system\GD3D11\UserSettings.ini`.

### Settings

* **F11** opens the settings menu. Options marked with `[*]` may need a restart of the game to take effect.
* **CTRL+F11** opens the advanced settings.
* **CTRL+Click** on `Save Settings` saves the settings for the current world only (`system\GD3D11\ZENResources\`). Once a world has its own settings, `Save Settings` keeps saving to them.
* Tick `Classic Settings Window` in the `System` tab to switch back to the previous settings window.

## Bugs & Problems

See also [known_issues.md](known_issues.md).

### Known causes of crashing
* If you have problems with launching game after installing GD3D11 - for example getting Access Denied(0x45a), reinstall your *Visual C++ Redistributable v14 (X86)* to latest version from [Microsoft website](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist?view=msvc-170), mod stopped working on older VCR due to some Microsoft changes in Platform Toolset.  
  https://aka.ms/vc14/vc_redist.x86.exe

### AMD

* For AMD RDNA+ graphics cards (RX 5xxx, RX 6xxx, RX 7xxx, RX 9xxx, …)
  installing DXVK (32-Bit, dxgi.dll & d3d11.dll) may help with Out-Of-Memory crashes.

## Running on Linux

To run the renderer on a bare linux desktop, you can use the following wine prefix setup, if you do not use Proton for example.

_tested on Fedora 42 with an AMD 7900 XTX graphics card_

_**requires WINE to be installed**_

```sh
# Setup a new wine-prefix specifically for Gothic games
WINEPREFIX=~/.wine-gothic WINEARCH=win32 winecfg
# Install dependencies
#  directmusic - fixes audio
#  dxvk - improves compatibility
#  vcrun2022 - is required for new builds
WINEPREFIX=~/.wine-gothic WINEARCH=win32 winetricks -q directmusic dxvk vcrun2022
```

using `winecfg` above, add `dinput`, `ddraw` and (optionally, see [#390](https://github.com/kirides/GD3D11/issues/390)) `d3dcompiler_47` as dll overrides (native, then built-in)
and remove `dsound` from overrides as that breaks Gothic 2.

Afterward launch your game(s) like this
```sh
cd /mnt/games/gothic1/system/
WINEPREFIX=~/.wine-gothic WINEARCH=win32 wine ./GothicMod.exe
```
```sh
cd /mnt/games/gothic2/system/
WINEPREFIX=~/.wine-gothic WINEARCH=win32 wine ./GothicStarter.exe
``` 

## Building

### Latest version

Building the mod is currently only possible with Windows, but should be easy to do for anyone. To build the mod, you need to do the following:

- Download & install **Git** (or any Git client) and clone this GitHub repository to get the GD3D11 code.
- Download & install **Microsoft Visual Studio 2026** (Community Edition is fine, make sure to enable the "Desktop development with C++" workload during installation!). The projects use the v145 platform toolset and C++23, so older Visual Studio versions won't work.
- ~~Download ... DirectX SDK ...~~ Not dependent on DirectX SDK anymore.
- Download & install/clone **[vcpkg](https://github.com/microsoft/vcpkg)**, then set the `VCPKG_ROOT` environment variable to point at it. All dependencies listed in vcpkg's manifest (`vcpkg.json`) are fetched and built automatically the first time you build - there's nothing to restore manually.
- Optional: Set environment variables "G2_SYSTEM_PATH" and/or "G1_SYSTEM_PATH", which should point to the "system"-folders of the games.

To build GD3D11, open its solution file (`Direct3D7Wrapper.sln`) with Visual Studio. It will then load all the required projects. There are multiple build targets, for releases and for developing / testing:

* Gothic 2 Release using AVX2: "Release_AVX2"
* Gothic 1 Release using AVX2: "Release_G1_AVX2"
* Gothic 2 Release using AVX: "Release_AVX"
* Gothic 1 Release using AVX: "Release_G1_AVX"
* Gothic 2 Release using old SSE2: "Release"
* Gothic 1 Release using old SSE2: "Release_G1"
* Gothic 1 1.12f Release: "Release_G1_12f"
* Gothic 2 Spacer.NET: "Spacer_NET"
* Gothic 1 Spacer.NET: "Spacer_NET_G1"
* Launcher (the `ddraw.dll` that loads the matching renderer DLL from `GD3D11\Bin`): "Launcher"
* Gothic 2 Develop: "Release_NoOpt"
* Gothic 1 Develop: "Release_NoOpt_G1"

> [!IMPORTANT]
> A real "debug" build is not possible, since mixing debug- and release-DLLs is not allowed, but for the Develop targets optimization is turned off, which makes it possible to use the debugger from Visual Studio with the built DLL when using a Develop target.

> [!TIP]
> The Release targets run the MSVC code analysis (`/analyze`), which takes about half of the build time. Set `GD3D11_DISABLE_ANALYZE=true` (as an environment variable, or `/p:GD3D11_DISABLE_ANALYZE=true` for MSBuild) to skip it.

Select the target for which you want to build (if you don't want to create a release, select one of the Develop targets), then build the solution. When the C++ build has completed successfully, the DLL with the built code and all needed files (pdb, shaders) will be copied into the game directory as you specified with the environment variables.

After that, the game will be automatically started and should now run with the GD3D11 code that you just built.

When using a Develop target, you might get several exceptions during the start of the game. This is normal, and you can safely continue to run the game for all of them (press continue, won't work for "real" exceptions of course).
When using a Release target, those same exceptions will very likely stop the execution of the game, which is why you should use Develop targets from Visual Studio and test your release builds by starting Gothic 1/2 directly from the game folder yourself.

### Building with CMake

The same targets are available as CMake presets (see `CMakePresets.json`). The plain presets use the Visual Studio 2026 generator, the `*_Clang` presets (`Release_Clang`, `Release_NoOpt_Clang`, `Release_AVX_Clang`, `Release_AVX2_Clang`, `Release_G1_Clang`, `Launcher_Clang`) build with Clang and Ninja.

```shell
cmake --preset Release_AVX2
cmake --build --preset Release_AVX2
```

Add `-DGD3D11_DEPLOY_AFTER_BUILD=ON` to the first command to copy the DLL and the shaders into the folder from "G2_SYSTEM_PATH" or "G1_SYSTEM_PATH" after every build.

### Producing the Redistributables
- Build all required configurations including the Launcher
- Copy required assets (shaders, textures, fonts, libraries) and the generated binaries into the release structure
> [!TIP]
> Check out the GitHub Actions workflow to see how the releases are build.

### [EXPERIMENTAL] Building on Linux

> [!WARNING]
> These steps were written before the build switched to vcpkg and have not been verified since. Passing `-DCMAKE_TOOLCHAIN_FILE` replaces the vcpkg toolchain of the presets, so the vcpkg dependencies are not installed automatically.

1. install clang & llvm
1. grab/install xwin
   ```shell
   xwin --accept-license --arch x86 splat --use-winsysroot-style --preserve-ms-arch-notation --output ~/.xwin
   ```
1. export correct vcpkg triplets in current terminal session
   ```shell
   export VCPKG_DEFAULT_TRIPLET="x86-windows-static-md"
   ```
1. prepare preset
   ```shell
   cmake --preset Release_NoOpt_Clang -DCMAKE_TOOLCHAIN_FILE=./cmake/msvc-clang-linux.cmake
   ```
1. build the project
   ```shell
   cmake --build --preset Release_NoOpt_Clang
   ```
1. copy `out/build/Release_NoOpt_Clang/D3D11Engine/ddraw.dll` (& `ddraw.pdb` for debugging) into `Gothic2\system\`
  
#### building a "release" version of G2 for example would mean

```shell
> cmake --preset Release_AVX2_Clang -DCMAKE_TOOLCHAIN_FILE=./cmake/msvc-clang-linux.cmake
> cmake --build --preset Release_AVX2_Clang
```

### Dependencies

- HBAO+ files from [dboleslawski/VVVV.HBAOPlus](https://github.com/dboleslawski/VVVV.HBAOPlus/tree/master/Dependencies/NVIDIA-HBAOPlus)
- [AMD FidelityFX SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK)
- [Intel ASSAO](https://github.com/GameTechDev/ASSAO)
- [SMAA](https://github.com/iryoku/smaa)
- [Dear ImGui](https://github.com/ocornut/imgui)
- [ImGuizmo](https://github.com/CedricGuillemet/ImGuizmo)
- [assimp](https://github.com/assimp/assimp)
- [meshoptimizer](https://github.com/zeux/meshoptimizer)
- [DirectXMath](https://github.com/microsoft/DirectXMath) and [DirectXTK](https://github.com/microsoft/DirectXTK)
- [Microsoft Detours](https://github.com/microsoft/Detours)
- [SQLite](https://www.sqlite.org)
- [MikkTSpace](https://github.com/mmikk/MikkTSpace)
- [stb](https://github.com/nothings/stb)
- [gtl](https://github.com/greg7mdp/gtl)
- [magic_enum](https://github.com/Neargye/magic_enum)
- [Tracy](https://github.com/wolfpld/tracy)

## Special Thanks

... to the following people

- [@ataulien](https://github.com/ataulien) (Degenerated @ WoG) for creating this project.
- [@BonneCW](https://github.com/BonneCW) (Bonne6 @ WoG) for providing the base for this modified version.
- [@lucifer602288](https://github.com/lucifer602288) (Keks1 @ WoG) for testing, helping with conversions and implementing several features.
- [@SaiyansKing](https://github.com/SaiyansKing) for fixing a lot of issues and adding major features.

<a href="https://github.com/kirides/GD3D11/graphs/contributors">
  <img src="https://contrib.rocks/image?repo=kirides/GD3D11" />
</a>

## License

- GD3D11 is licensed under the [GNU General Public License v3](LICENSE)
- HBAO+ is licensed under [GameWorks Binary SDK EULA](https://developer.nvidia.com/gameworks-sdk-eula)
