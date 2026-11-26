# Shader analysis: GD3D11 D3D12/Vulkan HLSL on gfx1035 (RDNA2 iGPU, Radeon 680M) vs gfx1100

Produced 2026-11-26 by a background analysis agent; saved here by the main session. Numbers marked *RGA* are offline
compiler output (measured, no GPU timing). Everything else is inferred from those numbers and the source and needs a
Radeon GPU Profiler capture on a 680M to confirm. Spot-checked by the main session: result files, row counts, and the
`SKY_VISIBILITY_LITE` / `USE_FP16_LIGHTING` / `SampleCascadePCF` line references. Other file:line references are the
agent's and were not individually re-verified.

Reproduce: `python tools/shader_analysis/analyze_shaders.py --rga <path> --asic gfx1035,gfx1100 --tag rga212`, then
`make_tables.py`. Raw results: `results_rga212.csv/.json` (gfx1035 + gfx1100, primary), `results_rga215.csv/.json`
(gfx1100 + gfx1200), `results_d3d11_approx.csv`, ISA of the heaviest shaders in `isa_rga212/` and `isa_rga215/`.
The `n_loops` column is broken (label parser); ignore it.

## Tools and caveats
- Radeon GPU Analyzer `vk-spv-offline` (LLPC), HLSL to SPIR-V with the Vulkan SDK 1.4.363.0 `dxc.exe` and the engine's own
  DXC arguments. RGA 2.12 and 2.15 were downloaded from GPUOpen-Tools releases into `%LOCALAPPDATA%\rga_2.12` and
  `rga_2.15` (outside the repo). Nothing else was installed.
- RGA 2.15 builds nothing for gfx10xx; RGA 2.9.1 crashes on 34 of 221 entries; RGA 2.12 builds gfx1035 for everything
  except `RtShadows.CSMain` (ray-query SPIR-V). gfx1100 rows from 2.12 are comparable with the gfx1035 rows.
- Pixel/vertex/domain/hull stages run wave64, compute wave32. Occupancy model = VGPR only (1024 per SIMD on RDNA2, 1536 on
  RDNA3, 16-wave cap). RVK measured the real driver at about 15 % fewer VGPRs than RGA: treat these as upper bounds.
- RGA 2.15 allocates far more VGPRs to tessellation domain shaders than 2.12 (`World.DSWorld` 140 vs 66, gfx1100), so
  gfx1035 DS/HS numbers may be understated against a current driver. Lit pixel shaders agree within 1-8 VGPR.
- Not covered: the DXIL path (AMD's D3D12 compiler differs from LLPC), FSR, and D3D11 FXC shaders on gfx1035
  (`results_d3d11_approx.csv` is DXC SM6 on gfx1100 only).

## Headline numbers (gfx1035, RGA 2.12)
- **No scratch and no spills in any of the 220 gfx1035 shaders.** The RVK scratch pattern (dynamically indexed struct arrays,
  struct copies) does not occur in our HLSL.
- **Pixel shaders are the occupancy problem:** 12 sit at 5 waves/SIMD or fewer (wave64), 6 more at 6-8. Post-process and UI
  pixel shaders all get 16.
- **Lit forward pixel shaders** (World PSMain/PSMainOpaque/PSQuadMark, Vob, Skeletal): 84-92 VGPR = **5 waves**,
  3.4k-4.5k instructions, 119-132 sample instructions (about 102 are shadow `image_sample_c`).
- **Water PSMain:** 116 VGPR = **4 waves**, 7299 instructions, 369 transcendentals, SGPR 106 of 106.
- **World HSWorldTess:** **61,440 B LDS**, 122 VGPR.
- **`USE_FP16_LIGHTING` does nothing measurable:** FP16=1 vs 0 changes VGPR by at most 1 and instruction count by about 10
  of 3.4k-4.5k, on both ASICs (16-18 `v_pk_*` per shader).
- gfx1100 gets 7-8 waves for the same shaders (1.5x register file).
- Not occupancy-limited: XeGTAO (31-46 VGPR), SMAA (21-29), TAA (48), DoF PS (23), sky PS (38), VobCull, skinning.

### Ranking by VGPR / occupancy (gfx1035; waves = w32 / w64)

| file | entry | stage | VGPR | SGPR | LDS B | instr | samples | transc | waves | VGPR gfx1100 |
|---|---|---|---|---|---|---|---|---|---|---|
| World | HSWorldTess (VERTEX_PULL) | hs | 122 | 34 | 61440 | 889 | 0 | 22 | 8 / 4 | 122 |
| Water | PSMain | ps | 116 | 106 | 0 | 7299 | 56 | 369 | 8 / **4** | 116 |
| WaterRT | CSMain | cs | 109 | 107 | 4096 | 7105 | 12 | 153 | 9 / 4 | 106 |
| RtShadows | CSFilter | cs | 97 | 98 | 0 | 2725 | 0 | 66 | 9 / 4 | 97 |
| Vob | PSMainBindless | ps | 92 | 94 | 0 | 3618 | 121 | 163 | 10 / **5** | 92 |
| Skeletal | PSMain | ps | 88 | 90 | 0 | 3414 | 120 | 154 | 11 / **5** | 85 |
| LowClouds | CSGenerate | cs | 88 | 90 | 0 | 2184 | 16 | 141 | 11 / 5 | 88 |
| Vob | PSMain | ps | 87 | 86 | 0 | 3426 | 119 | 155 | 11 / **5** | 85 |
| World | PSQuadMark | ps | 85 | 92 | 0 | 4478 | 132 | 183 | 11 / **5** | 84 |
| World | PSMain / PSMainOpaque | ps | 84 | 90-92 | 0 | 4478 / 3969 | 132 / 126 | 183 / 172 | 11 / **5** | 84 |
| LowClouds | PSComposite | ps | 77 | 71 | 0 | 2045 | 12 | 132 | 12 / **6** | 77 |
| Vob | PSAlphaBlendBindless | ps | 77 | 82 | 0 | 2031 | 85 | 113 | 12 / **6** | 71 |
| VobCull | CSCull | cs | 72 | 71 | 512 | 722 | 0 | 11 | 14 / 7 | 72 |
| Decal | PSMainLit | ps | 71 | 78 | 0 | 1897 | 131 | 67 | 14 / **7** | 71 |
| Vegetation | PSMain | ps | 69 | 74 | 0 | 1564 | 66 | 75 | 14 / **7** | 71 |
| World | DSWorld (VERTEX_PULL) | ds | 66 | 50 | 0 | 1636 | 7 | 54 | 14 / 7 | 60 (2.15: 140) |
| Water | HSWater / HSOcean | hs | 44 / 42 | 34 | 29696 | 411 / 335 | 0 | 12 / 9 | 16 / 10 | 44 / 42 |

LDS: World HS 61,440 B, Water HS 29,696 B, Ocean FFT 4-16 KB, LightCull 6.6 KB, DSWater/DSOcean 5 KB, WaterRT 4 KB.
D3D11 deferred approximation (gfx1100 only): PS_Diffuse 22 VGPR, PS_DS_AtmosphericScattering 34, PS_DS_PointLight 55,
PS_DS_PointLightDynShadow 75, PS_Water 117, CS_TiledShading 93, PS_PFX_TAA 62.

## What a 680M would be limited on (inferred, confirm with RGP)
1. Lit world/VOB/skeletal pixel shaders (5 waves, 3.4-4.5k instr, ~100 shadow fetches): fill rate x overdraw.
2. Water pixel shader (4 waves, 7.3k instr, SGPR at the RDNA2 cap).
3. Cascade shadow rendering (D32 2048^2 x 3, `D3D12ShadowMap.cpp:128-156`).
4. Tessellation HS/DS (61 KB LDS hull).
5. Low clouds. 6. RT shadows / RT water (keep off by default on iGPU). 7. RGBA16F scene-colour traffic.

## Recommendations (estimated wins are inferred unless an ISA number is given)

- **R1. Cheaper sun-shadow filter tier on iGPU:** `PBRLighting.hlsl:132` (`SampleCascadePCF`), `:190` (`ComputeSunShadow`).
  A 5-tap uniformity pre-test is followed by a 25-tap `SampleCmp` kernel in penumbra pixels, and the cascade cross-fade
  runs it again. Add a `SHADOW_PCF_LITE` tier (3x3 or 8-tap rotated disk with interleaved-gradient noise; TAA hides it).
  25 to 9 fetches in penumbra. Risk medium: softer/noisier penumbra, `kPcfWorldRadius` (line ~126) needs retuning.
- **R2. Use the existing `SKY_VISIBILITY_LITE` on iGPU:** `PBRLighting.hlsl:430`. Only grass uses it today
  (`Vegetation.hlsl:53`). `SampleSkyVisibility` does 25 `SampleCmp` per lit pixel; LITE does 9. Add the define in
  `D3D12ShaderBackend::AppendGlobalMacros` (`D3D12ShaderBackend.cpp:~513`) for integrated GPUs. Cheapest change, low risk.
- **R3. Lit-pixel-shader register pressure (84-92 VGPR): split rare features by compile-time permutation** (WET, BACKLIT,
  RT_MASK, POM, SKYIBL), selected per material class or when the setting is off. `ComputeSunLightingPBR`
  (`PBRLighting.hlsl:~625-680`) keeps all of them behind runtime branches, so every shader pays the union's registers.
  `PSMainOpaque` is the precedent (3969 vs 4478 instr). RVK measured the same (GI hooks removed: -43 % instr, -14 VGPR,
  gfx1035 3 to 4 waves). Possible gain 10-20 VGPR (5 to 6-7 waves). Risk: more PSOs / compile time; do the top 3 only.
- **R4. Water pixel shader** (`Water.hlsl:413`, SSR `:203-260`): lower SSR steps on iGPU, half-res or Hi-Z marching;
  variants for ocean-sim off / SSR off / SSR on. Estimated 116 to ~70-80 VGPR (4 to 6 waves). Reflection look changes.
- **R5. World tessellation hull:** the hull is a pass-through but carries ~39 dwords per control point, which is the
  61 KB LDS / 122 VGPR. Carry only what the patch-constant function needs plus vertex ids (vertex pulling is on, so the
  DS can re-fetch uv/colour/tangent); default tessellation off on iGPU. Positions must stay bit-identical for the
  depth-EQUAL pass. Risk medium.
- **R6. Low clouds:** (a) hoist the three Henyey-Greenstein phase `pow(x,1.5)` terms out of the march loop (same
  `cosTheta` per pixel; ~12 transcendentals per step, bit-equivalent, no risk); (b) 16 steps instead of 32 or a
  quarter-res layer on iGPU; (c) R8G8B8A8_UNORM instead of RGBA16F for the gamma-space layers; (d) early-out
  `PSComposite` where layer alpha is 0.
- **R7. `USE_FP16_LIGHTING` is a no-op** (`PBRLighting.hlsl:284`): remove it, or write the light loop in fp16 end to end
  (precision-sensitive, at most 5-10 % of the lit shader's ALU). Low priority. See also the memory note on the fp16 kernel.
- **R8. Bandwidth / render-target bytes:** scene colour RGBA16F is copied/read by SSR, TAA, fog, DoF, bloom (R11G11B10
  `CompressBackBuffer` exists but is ignored while TAA is on because TAA needs alpha: move the TAA history weight out of
  alpha to allow it); CSM D32 to D16 like the point-light cubes (retune the 0.000003 bias); GTAO half-depth R32F to R16F
  (RVK did this); SSR and TAA each keep an extra R32F depth copy. **Render scale (FSR / `m_Resolution`, ~0.67x) is the
  largest single iGPU lever**, bigger than any shader change here.
- **R9. RT paths off by default on iGPU** (WaterRT 109 VGPR / 7.1k instr, RtShadows CSMain 120 VGPR on RDNA3, CSFilter
  97). If kept: precompute the per-tap `cos/sin/round` ring offsets in `RtShadows.hlsl:~528-535`.
- **R10. TAA debug toggles ship in production** (`TAAResolve.hlsl:186-188` `ENABLE_DEBUG`): build with
  `-DENABLE_DEBUG=0` for release; a few percent of a 48-VGPR shader. Costs live toggling.
- **R11. Micro:** `SrgbToLinear` on per-frame constants (`SunColor`, `NightFill`) per pixel (`PBRLighting.hlsl:~631, 667`),
  ~12 of ~180 transcendentals per lit shader; do it on the CPU.

### RVK lessons checked against our shaders
- Scratch from dynamic indexing: absent (0 scratch, 0 spills).
- Debug code in production: only TAA (R10).
- Per-light loops: `AccumTiledPointLights` (`PBRLighting.hlsl:~751-803`) loads `Lights[i]` per lane (cluster index from
  `SV_Position` is not provably wave-uniform); ~100 vector `buffer_load` vs ~85 `s_buffer_load`. RVK found LLPC does not
  scalarise these even with a waterfall, so there is no cheap fix. `SamplePointShadow` (`:68-112`) re-normalises per tap.
- Register-heavy prologues: see R3. `LightCull.hlsl` already does the right thing.

## Suggested order
1. R2, then R1. 2. R3 (largest occupancy lever). 3. R4, R5, R6a (free). 4. R8 + render-scale defaults (needs a GPU profile
to size). 5. R10, R11, R9 table hoist (trivial, low gain).
Test any of these by editing a scratch copy of `D3D11Engine/Shaders/`, pointing the script at it, and comparing VGPR and
instruction counts; real timings need Radeon GPU Profiler captures on the 680M.

## Follow-up: what was implemented, measured, or rejected (2026-11-26)
All numbers RGA 2.12, gfx1035, `PSMain`/`PSMainOpaque` (lit passes), `IGPU_TUNING=1` unless noted. Reproduce with
`analyze_shaders.py --define NAME=VALUE` (extra global macro) and `--shader-dir` (analyse a scratch copy of `Shaders/D3D12`
with its `../include` sibling).

- **R2, cheap sky visibility (implemented, `19affb47`):** `SKY_VISIBILITY_LITE` follows the new `IGPU_TUNING` macro
  (integrated-GPU detection + Featureset override). World PSMain image samples 132 -> 116, instructions 4478 -> 4421.
- **R1, penumbra kernel (implemented, `a0e358e4`):** `SHADOW_PCF_LITE`, 3x3 at 1.5 steps after the pre-test.
  Image samples 116 -> 92 (132 -> 92 together with R2), instructions 4350, VGPR 83.
- **R3, partial (implemented, `683b57e9`):** features that are *off by setting* are compiled out on iGPUs
  (`FEAT_RT_SHADOWS`, `FEAT_OPAQUE_SSR`), with a settings key that reloads shaders when they change.
  RT shadows off (the default): World PSMain 83 -> 80 VGPR (5 -> 6 waves), Vob PSMain 87 -> 79 (5 -> 6),
  Vegetation 69 -> 62 (7 -> 8). RT and opaque SSR both off: Skeletal 88 -> 79 (5 -> 6), instructions -10..-15 %.
  Measured what each lit-shader feature costs in `World.hlsl` PSMainOpaque (VGPR / instructions when compiled out):
  wetness -12 / -1216, height+POM -4 / -400, far-field -4 / -18, RT mask -3 / -97, opaque SSR 0 / -333,
  foliage bend 0 / -129; all six: 83 -> 69 VGPR (7 waves). Wetness and height are material/weather driven, so they
  stay compiled in; they would need dry/wet pipeline variants (not done).
- **R6a, low-cloud phase hoist (implemented):** the compiler had already hoisted the Henyey-Greenstein terms (instruction
  and transcendental counts unchanged); making it explicit only moves registers, CSGenerate 88 -> 85, PSComposite 77 -> 74.
- **R4, water pixel shader variants (rejected, measured):** compiling out the FFT ocean evaluation and the SSR march
  changes `Water.hlsl` PSMain by 116 -> 115 VGPR (still 4 waves) and instructions 7299 -> 6699 (-8 %). The peak is in the
  shared shading (atmosphere, clouds, glint, absorption), not in the optional features, so these permutations would not buy
  occupancy. A real reduction needs restructuring `WaterShading.hlsl`, not a macro.
- **R5, tessellation hull LDS (not done):** the 38-dword control point is copied through the hull. Shrinking it means the domain
  shader re-fetches vertex attributes per tessellated vertex (the DS runs far more often than the HS), trading LDS for memory
  traffic on a bandwidth-limited GPU; packing only the vertex colour saves ~8 %. World tessellation is off by default.
  Needs a GPU capture before touching it.
