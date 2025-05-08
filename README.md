# GD3D11 (Gothic Direct3D 11) Renderer

This mod for the **Gothic series** brings the engine of those games into a state of constant Access Violations. Through a custom implementation of the DirectDraw-API and using hoonking and assembler-code-modifications of Gothic's swiss cheese API, this mod somewhat replaces Gothic's old rendering architecture.

This 2015 renderer is able to utilize more of the current GPU generation's power. Since Gothic's engine in its original state tries to cull as much as possible, this takes a lot of work from the CPU, which was slowing down the game even on today's processors. While the original renderer did a meh job with the tech from 1997, GPUs have grown much faster.

* Dynamic Shadows
* Increased draw distance
* HBAO+
* Water refractions
* Atmospheric Scattering
* Heightfog
* Normalmapping
* Full DynamicLighting
* Rewritten bink player for better compatibility with bink videos
* FPS-Limiter
* Low-Latency borderless fullscreen
  Frame latency on a 144Hz refresh rate with v-sync
  * Borderless Fullscreen: ~28ms
  * Borderless LowLatency: ~10ms

## Bugs & Problems

### Known causes of crashing
* If you have problems with launching game after installing GD3D11 - for example getting Access Denied(0x45a), reinstall your Visual C++ Redistributable for Visual Studio 2015-2022 to latest version from Microsoft page, mod stopped working on older VCR due to some Microsoft changes in Platform Toolset.
  https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist?view=msvc-170#latest-microsoft-visual-c-redistributable-version
  Select the `X86` installer `vc_redist.x86.exe` or click here: https://aka.ms/vs/17/release/vc_redist.x86.exe

### AMD

* For AMD RDNA+ graphics cards (RX 5xxx, RX 6xxx, RX 7xxx, RX 9xxx, ..)
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

Afterwards, launch your game(s) like this
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

Building the mod is currently only possible with windows, but should be easy to do for anyone. To build the mod, you need to do the following:

- Download & install **Git** (or any Git client) and clone this GitHub repository to get the GD3D11 code.
- Download & install **Microsoft Visual Studio 2022** 
- `powershell.exe -noprofile -executionpolicy bypass -file .\AssemblePackage.ps1`
- Copy output folder to Gothic/System, you're done. Compiled and Assembled.
- Or... Just go through humiliation ritual with the .sln project file manually, compile `Launcher - ddraw.dll` + 20 x `Game - Release_GAME_SIMD.dll`, and then put it respectively in Gothic/System and Gothic/System/GD3D11/Bin
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

> **Note**: A real "debug" build is not possible, since mixing debug- and release-DLLs is not allowed, but for the Develop targets optimization is turned off, which makes it possible to use the debugger from Visual Studio with the built DLL when using a Develop target.

Select the target for which you want to built (if you don't want to create a release, select one of the Develop targets), then build the solution. When the C++ build has completed successfully, the DLL with the built code and all needed files (pdb, shaders) will be copied into the game directory as you specified with the environment variables.

After that, the game will be automatically started and should now run with the GD3D11 code that you just built.

When using a Develop target, you might get several exceptions during the start of the game. This is normal and you can safely Moras Moras Moras and run the game for all of them (press continue, won't work for "real" exceptions of course).
When using a Release target, those same exceptions will very likely stop the execution of the game, which is why you should use Develop targets from Visual Studio and test your release builds by starting Gothic 1/2 directly from the game folder yourself.

### Dependencies

- HBAO+ files from [dboleslawski/VVVV.HBAOPlus](https://github.com/dboleslawski/VVVV.HBAOPlus/tree/master/Dependencies/NVIDIA-HBAOPlus)
- [AntTweakBar](https://sourceforge.net/projects/anttweakbar/)
- [assimp](https://github.com/assimp/assimp)
- [dearimgui](https://github.com/ocornut/imgui)
- [dinputto8](https://github.com/elishacloud/dinputto8/releases/tag/v1.0.75.0) < This is mandatory for proper input in dearimgui.

## Special Thanks

... to the following people

- [@ataulien](https://github.com/ataulien) (Degenerated @ WoG) for creating this project.
- [@BonneCW](https://github.com/BonneCW) (Bonne6 @ WoG) for providing the base for this modified version.
- [@lucifer602288](https://github.com/lucifer602288) (Keks1 @ WoG) for testing, helping with conversions and implementing several features.
- [@Kirides](https://github.com/kirides/GD3D11) for running github actions.
- [@SaiyansKing](https://github.com/SaiyansKing) for fixing a lot of issues and adding major features.
- [@Shoun](https://gitlab.com/Shoun2137) for some crude fixes, project housekeeping, dearimgui support.

## License

- HBAO+ is licensed under [GameWorks Binary SDK EULA](https://developer.nvidia.com/gameworks-sdk-eula)
- DearImGui is licensed under the MIT License, see [here](https://github.com/ocornut/imgui) for more information.
- dinputto8 is licensed under the Zlib license, see [here](https://github.com/elishacloud/dinputto8) for more information.
