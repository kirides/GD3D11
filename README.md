# GD3D11 (Gothic Direct3D 11) Renderer [![GitHub Actions](https://img.shields.io/github/actions/workflow/status/kirides/GD3D11/build-and-conditional-release.yml?logo=github&labelColor=24292e&color=28A745)](https://github.com/kirides/GD3D11/actions) [![GitHub latest release](https://img.shields.io/github/v/release/kirides/GD3D11?logo=github&labelColor=24292e&color=0969da)](https://github.com/kirides/GD3D11/releases/latest) [![GitHub nightly release](https://img.shields.io/github/v/release/kirides/GD3D11?include_prereleases&filter=nightly&logo=github&label=dev-release&labelColor=24292e&color=bf8700)](https://github.com/kirides/GD3D11/releases/tag/nightly)

This mod for the games **Gothic** and **Gothic II** brings the engine of those games into a more modern state. Through a custom implementation of the DirectDraw-API and using hooking and assembler-code-modifications of Gothic's internal engine calls, this mod completely replaces Gothic's old rendering architecture.

The new renderer is able to utilize more of the current GPU generation's power. Since Gothic's engine in its original state tries to cull as much as possible, this takes a lot of work from the CPU, which was slowing down the game even on today's processors. While the original renderer did a really great job with the tech from 2002, GPUs have grown much faster. And now, that they can actually use their power to render, we not only get a big performance boost on most systems, but also more features:

* Dynamic Shadows
* Cascaded Shadow Maps, more than 1 Cascade produces much better shadows.
* Increased draw distance
* Increased Performance
* HBAO+
* Water refractions
* Atmospheric Scattering
* Heightfog
* Normalmapping
* Full DynamicLighting
* Vegetationgeneration
* Editor-Panel to insert some of the renderers features into the world
* Custom-Built UI-Framework based on Direct2D
* Rewritten bink player for better compatibility with bink videos
* FPS-Limiter
* Low-Latency borderless fullscreen
  Frame latency on a 144Hz refresh rate with v-sync
  * Borderless Fullscreen: ~28ms
  * Borderless LowLatency: ~10ms

## Installation & Usage
> [!NOTE]
> In the past there used to be separate files for Gothic 1 and Gothic 2, this has now changed since the mod will automatically detect the game.
1. Download the **GD3D11-*VERSION*.zip** file from the **Assets** section in the latest release of this repository (e.g. [kirides/releases](https://github.com/kirides/GD3D11/releases/latest)).
3. Unpack the zip file and copy the content into the `Gothic\system\` or `Gothic2\system\` game folder.
4. When starting the game you should see the version number of GD3D11 in the top-left corner.
5. As soon as you start the game for the first time after the installation you should press F11 to open the renderer menu and press `Apply(*)`. This saves all the options to `Gothic(2)\system\GD3D11\UserSettings.ini`.

## Bugs & Problems

### Known causes of crashing
* If you have problems with launching game after installing GD3D11 - for example getting Access Denied(0x45a), reinstall your *Visual C++ Redistributable v14 (X86)* to latest version from [Microsoft website](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist?view=msvc-170), mod stopped working on older VCR due to some Microsoft changes in Platform Toolset.  
  https://aka.ms/vc14/vc_redist.x86.exe

### AMD

* For AMD RDNA+ graphics cards (RX 5xxx, RX 6xxx, RX 7xxx, RX 9xxx, …)
  installing DXVK (32-Bit, dxgi.dll & d3d11.dll) may help with Out-Of-Memory crashes.

> @Shoun2137:
> There are only bugs and problems, deal with it. This exact series of patches was made strictly for Mordan so that this _version_ would stop AC'ing internally in GD3D11. Oh, and also mainly because I play on Loonix, and this dumb D2D <-> D3D interop has abysmal performance, so I had to abort it with a clothes hanger. As of now, it's recommended to install DXVK + this GD3D11 fork, ~~as that wasn't really working due to DXVK not supporting the D2D interop on Windows~~ Saiyans added his solution to this problem on Windows, but when playing on Linux you're still out of luck.

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

using `winecfg` above, add `dinput` and `ddraw` as dll overrides (native, then built-in)
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
- Download & install **Microsoft Visual Studio 2019** (Community Edition is fine, make sure to enable C++ Tools during installation!). Might work on 2015 or 2017 but untested.
- ~~Download ... DirectX SDK ...~~ Not dependent on DirectX SDK anymore.
- Optional: Set environment variables "G2_SYSTEM_PATH" and/or "G1_SYSTEM_PATH", which should point to the "system"-folders of the games.

To build GD3D11, open its solution file (.sln) with Visual Studio. It will the load all the required projects. There are multiple build targets, one for release and one for developing / testing, for both games each:

* Gothic 2 Release using AVX2: "Release_AVX2"
* Gothic 1 Release using AVX2: "Release_G1_AVX2"
* Gothic 2 Release using AVX: "Release_AVX"
* Gothic 1 Release using AVX: "Release_G1_AVX"
* Gothic 2 Release using old SSE2: "Release"
* Gothic 1 Release using old SSE2: "Release_G1"
* Gothic 2 Develop: "Release_NoOpt"
* Gothic 1 Develop: "Release_NoOpt_G1"

> [!IMPORTANT]
> A real "debug" build is not possible, since mixing debug- and release-DLLs is not allowed, but for the Develop targets optimization is turned off, which makes it possible to use the debugger from Visual Studio with the built DLL when using a Develop target.

Select the target for which you want to build (if you don't want to create a release, select one of the Develop targets), then build the solution. When the C++ build has completed successfully, the DLL with the built code and all needed files (pdb, shaders) will be copied into the game directory as you specified with the environment variables.

After that, the game will be automatically started and should now run with the GD3D11 code that you just built.

When using a Develop target, you might get several exceptions during the start of the game. This is normal, and you can safely continue to run the game for all of them (press continue, won't work for "real" exceptions of course).
When using a Release target, those same exceptions will very likely stop the execution of the game, which is why you should use Develop targets from Visual Studio and test your release builds by starting Gothic 1/2 directly from the game folder yourself.

### Producing the Redistributables
- Build all required configurations including the Launcher
- Copy required assets (shaders, textures, fonts, libraries) and the generated binaries into the release structure
> [!TIP]
> Check out the GitHub Actions workflow to see how the releases are build.

### Dependencies

- HBAO+ files from [dboleslawski/VVVV.HBAOPlus](https://github.com/dboleslawski/VVVV.HBAOPlus/tree/master/Dependencies/NVIDIA-HBAOPlus)
- [AMD FidelityFX SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK)
- [AntTweakBar](https://sourceforge.net/projects/anttweakbar/)
- [Dear ImGui](https://github.com/ocornut/imgui)
- [assimp](https://github.com/assimp/assimp)

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

- HBAO+ is licensed under [GameWorks Binary SDK EULA](https://developer.nvidia.com/gameworks-sdk-eula)
