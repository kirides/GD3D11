# GD3D11 (Gothic Direct3D 11) Renderer - Stockholm Syndrome Edition

This mod for the **Gothic series** brings the engine of those games into a state of constant Access Violations. Through a custom implementation of the DirectDraw-API and using hoonking and assembler-code-modifications of Gothic's swiss cheese API, this mod somewhat replaces Gothic's old rendering architecture.

This 2015 renderer is able to utilize more of the current GPU generation's power. Since Gothic's engine in its original state tries to cull as much as possible, this takes a lot of work from the CPU, which was slowing down the game even on today's processors. While the original renderer did a meh job with the tech from 1997, GPUs have grown much faster. And now, nobody reads this shit so why bother, just pick up the release and go.

* Dynamic Shadows
* Increased draw distance
* ~~Increased Performance~~ Probably not, this one should be more stable than any other GD3D11 fork.
* HBAO+
* Water refractions
* Atmospheric Scattering
* Heightfog
* Normalmapping
* Full DynamicLighting
* Rewritten bink player for better compatibility with bink videos
* FPS-Limiter

## Bugs & Problems

There are only bugs and problems, deal with it. This exact series of patches was made strictly for Mordan so that this piece of shit would stop AC'ing internally in GD3D11. Oh, and also mainly because I play on Loonix, and this dumb D2D <-> D3D interop has abysmal performance, so I had to abort it with a clothes hanger. As of now, it's recommended to install DXVK + this GD3D11 fork, ~~as that wasn't really working due to DXVK not supporting the D2D interop on Windows~~ Saiyans added his solution to this problem on Windows, but when playing on Linux you're still out of luck.

## Building

### Latest version

Building the mod is currently only possible with windows, but should be easy to do for anyone. To build the mod, you need to do the following:

- Download & install **Git** (or any Git client) and clone this GitHub repository to get the GD3D11 code.
- Download & install **Microsoft Visual Studio 2022** 
- `powershell.exe -noprofile -executionpolicy bypass -file .\AssemblePackage.ps1`
- Copy output folder to Gothic/System, you're done. Compiled and Assembled.
- Or... Just go through humiliation ritual with the .sln project file manually, compile `Launcher - ddraw.dll` + 20 x `Game - Release_GAME_SIMD.dll`, and then put it respectively in Gothic/System and Gothic/System/GD3D11/Bin

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
