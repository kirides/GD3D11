#!/usr/bin/env python3
"""Offline AMD (RDNA) shader statistics for the D3D12/Vulkan HLSL shaders (see REPORT.md).

For every (file, entry, target, defines) the D3D12 backend compiles (extracted by tools/validate_spirv.py,
plus a few call sites it cannot see), this compiles the HLSL with DXC to SPIR-V using the engine's own
SPIR-V arguments, then feeds the SPIR-V to Radeon GPU Analyzer's vk-spv-offline mode for each target ASIC.

Usage:  python tools/shader_analysis/analyze_shaders.py [--asic gfx1100 --asic gfx1200] [--filter SUBSTR]
                                                          [--jobs N] [--isa-top N] [--out DIR]
Tools:  rga.exe (2.9.1 for gfx1035, 2.15 for gfx1100/gfx1200) via --rga, RGA_PATH, PATH, or %LOCALAPPDATA%\\rga_*\\bin; dxc.exe via --dxc, VULKAN_SDK, C:\\VulkanSDK.
Output: results.csv / results.json in --out (default tools/shader_analysis), ISA of the heaviest shaders in isa/.
Note:   vk-spv-offline uses RGA's bundled AMD compiler, not the installed driver; each stage is compiled alone
        with default pipeline state, so PS numbers exclude the VS->PS interpolant/export interplay.
"""
import argparse
import concurrent.futures
import csv
import glob
import importlib.util
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

spec = importlib.util.spec_from_file_location("validate_spirv", os.path.join(ROOT, "tools", "validate_spirv.py"))
vs = importlib.util.module_from_spec(spec)
spec.loader.exec_module(vs)

# Entry points compiled from call sites validate_spirv.py cannot parse (non-literal file/entry arguments).
EXTRA = []
for e in ("CSSpectrum", "CSEvolve", "CSResolve", "CSMip"):
    EXTRA.append(("Ocean.hlsl", e, "cs_6_6", ()))
for n in (128, 256, 512):
    EXTRA.append(("Ocean.hlsl", "CSFFT", "cs_6_6", (("OCEAN_N", str(n)), ("OCEAN_LOG2N", str(int(n).bit_length() - 1)))))
for e in ("CSClear", "CSInstances", "CSTail"):
    EXTRA.append(("RtSceneInstances.hlsl", e, "cs_6_6", ()))
for e in ("CSDownsampleDepth", "CSPrefilterDepths16x16", "CSGenerateNormals", "CSGTAOLow", "CSGTAOMedium",
          "CSGTAOHigh", "CSGTAOUltra", "CSDenoisePass", "CSDenoiseLastPass"):
    EXTRA.append(("XeGTAO.hlsl", e, "cs_6_6", ()))
# Forward lighting with USE_FP16_LIGHTING=0 (what DXIL gets without native 16-bit) for the A/B in the report.
for f, e in (("World.hlsl", "PSMain"), ("World.hlsl", "PSMainOpaque"), ("Vob.hlsl", "PSMain"),
             ("Vob.hlsl", "PSMainBindless"), ("Skeletal.hlsl", "PSMain"), ("Vegetation.hlsl", "PSMain")):
    EXTRA.append((f, e, "ps_6_6", (("USE_FP16_LIGHTING", "0"),)))

STAGE_FLAG = {"vs": "--vert", "ps": "--frag", "cs": "--comp", "hs": "--tesc", "ds": "--tese"}
# VGPR file per SIMD (wave32): RDNA3 1536/granule 24, RDNA2 1024/granule 8, 16 waves max. RGA 2.15 vk-spv-offline lacks gfx10xx;
# use RGA 2.9.1 (--rga, RGA_PATH) for gfx1035.
ARCH = {"gfx1100": (1536, 24, 16), "gfx1200": (1536, 24, 16), "gfx1035": (1024, 8, 16), "gfx1030": (1024, 8, 16)}


def find_rga(explicit):
    cands = [explicit, os.environ.get("RGA_PATH"), shutil.which("rga")]
    cands += sorted(glob.glob(os.path.join(os.environ.get("LOCALAPPDATA", ""), "rga_*", "bin", "rga.exe")), reverse=True)
    cands += sorted(glob.glob(r"C:\Program Files\RGA*\rga.exe"), reverse=True)
    for c in cands:
        if c and os.path.isfile(c):
            return c
    sys.exit("rga.exe not found; use --rga or RGA_PATH (https://github.com/GPUOpen-Tools/radeon_gpu_analyzer/releases)")


def occupancy(asic, vgprs):
    total, gran, cap = ARCH.get(asic, (1536, 24, 16))
    alloc = max(gran, -(-vgprs // gran) * gran)
    return min(cap, total // alloc)


def toint(v):
    try:
        return int(v)
    except ValueError:
        return -1   # RGA prints N/A


def occupancy64(asic, vgprs):
    total, gran, cap = ARCH.get(asic, (1536, 24, 16))
    alloc = max(gran, -(-vgprs // gran) * gran)
    return max(1, min(cap, total // (2 * alloc)))


def parse_isa(path):
    """Instruction mix + simple hot-spot hints from an RGA ISA listing."""
    mix = dict(total=0, salu=0, valu=0, vmem=0, smem=0, lds=0, branch=0, trans=0, loops=0, scratch=0, image=0,
               image_sample=0, bvh=0, packed16=0, wmma=0, wait=0)
    labels = {}
    n = 0
    for line in open(path, encoding="utf-8", errors="replace"):
        s = line.strip()
        if not s or s.startswith(("//", ";", "Disassembly")):
            continue
        if re.match(r"^\w+:\s*(;.*)?$", s) or s.endswith(":") and " " not in s:
            labels[s.rstrip(":")] = n
            continue
        op = s.split(None, 1)[0]
        if not re.match(r"^[a-z][a-z0-9_]*$", op):
            continue
        n += 1
        mix["total"] += 1
        if op.startswith("s_waitcnt") or op.startswith("s_delay_alu"):
            mix["wait"] += 1
        if op.startswith("s_cbranch") or op == "s_branch":
            mix["branch"] += 1
            m = re.search(r"(L\d+)", s.split("//")[0])
            if m and m.group(1) in labels:
                mix["loops"] += 1   # backward branch to an already-seen label
        elif op.startswith("s_load") or op.startswith("s_buffer_load"):
            mix["smem"] += 1
        elif op.startswith("s_"):
            mix["salu"] += 1
        elif op.startswith("v_") or op.startswith("v"):
            mix["valu"] += 1
            if re.match(r"v_(exp|log|rcp|rsq|sqrt|sin|cos)_f(16|32)", op):
                mix["trans"] += 1
            if re.match(r"v_pk_|v_dot2", op):
                mix["packed16"] += 1
            if "wmma" in op:
                mix["wmma"] += 1
        elif op.startswith(("image_", "buffer_", "global_", "flat_", "scratch_", "tbuffer_")):
            mix["vmem"] += 1
            if op.startswith("scratch_") or "scratch" in s:
                mix["scratch"] += 1
            if op.startswith("image_bvh"):
                mix["bvh"] += 1
            if op.startswith("image_sample") or op.startswith("image_gather"):
                mix["image_sample"] += 1
            if op.startswith("image_"):
                mix["image"] += 1
        elif op.startswith("ds_"):
            mix["lds"] += 1
    return mix


def dxc_args(defines, globals_):
    a = list(vs.SPIRV_ARGS)
    for name, value in list(defines) + globals_:
        a += ["-D", f"{name}={value}"]
    return a


def run_one(rga, dxc, entry, asics, work, isa_dir):
    file, ep, target, defines = entry
    stage = target.split("_")[0]
    tag = f"{os.path.splitext(file)[0]}.{ep}" + ("." + "_".join(f"{n}{v}" for n, v in defines) if defines else "")
    glob_set = vs.GLOBAL_MACRO_SETS[1] + EXTRA_GLOBALS
    rows, errs = [], []
    with tempfile.TemporaryDirectory(dir=work) as tmp:
        spv = os.path.join(tmp, tag + ".spv")
        cmd = [dxc, *dxc_args(defines, glob_set), "-I", ".", "-E", ep, "-T", target, file, "-Fo", spv]
        r = subprocess.run(cmd, cwd=vs.SHADER_DIR, capture_output=True, text=True)
        if r.returncode != 0:
            return tag, [], [("dxc", "", (r.stderr or r.stdout).strip()[-600:])]
        for asic in asics:
            isa, stats = os.path.join(tmp, "isa.txt"), os.path.join(tmp, "stats.csv")
            try:
                p = subprocess.run([rga, "-s", "vk-spv-offline", "-c", asic, "--isa", isa, "-a", stats,
                                    STAGE_FLAG[stage], spv], capture_output=True, text=True, timeout=600)
            except subprocess.TimeoutExpired:
                errs.append(("rga-timeout", asic, ""))
                continue
            stat_files = glob.glob(os.path.join(tmp, f"{asic}_stats_*.csv"))
            isa_files = glob.glob(os.path.join(tmp, f"{asic}_isa_*.txt"))
            if not stat_files:
                errs.append(("rga", asic, (p.stdout + p.stderr).strip()[-600:]))
                continue
            row = next(csv.DictReader(open(stat_files[0])))
            mix = parse_isa(isa_files[0]) if isa_files else {}
            vg, sg = toint(row.get("USED_VGPRs", row.get("USED_VGPRS"))), toint(row["USED_SGPRs"])
            rec = dict(shader=file, entry=ep, stage=stage, defines=" ".join(f"{n}={v}" for n, v in defines), asic=asic,
                       vgprs=vg, sgprs=sg, lds=toint(row["USED_LDS_BYTES"]), scratch=toint(row["SCRATCH_MEM"]),
                       vgpr_spills=toint(row["VGPR_SPILLS"]), sgpr_spills=toint(row["SGPR_SPILLS"]),
                       isa_bytes=toint(row["ISA_SIZE"]), waves_per_simd=occupancy(asic, vg), waves_w64=occupancy64(asic, vg),
                       threads=toint(row["THREADS_PER_WORKGROUP"]), **{f"n_{k}": v for k, v in mix.items()})
            rows.append(rec)
            if isa_files:
                keep = os.path.join(isa_dir, f"{tag}.{asic}.isa.txt")
                shutil.copyfile(isa_files[0], keep)
    return tag, rows, errs


EXTRA_GLOBALS = []   # --define NAME=VALUE (later -D wins), e.g. IGPU_TUNING=0 to analyse the desktop permutation


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rga")
    ap.add_argument("--tag", default="", help="suffix for output names, e.g. _rga291")
    ap.add_argument("--dxc")
    ap.add_argument("--asic", action="append")
    ap.add_argument("--filter")
    ap.add_argument("--shader-dir", help="analyse a scratch copy of Shaders/D3D12 (for experiments) instead of the repo one")
    ap.add_argument("--define", action="append", default=[], help="extra global macro NAME=VALUE for every compile")
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 4) // 2))
    ap.add_argument("--isa-top", type=int, default=12, help="keep ISA for the N highest-VGPR and N longest shaders (first ASIC)")
    ap.add_argument("--out", default=HERE)
    o = ap.parse_args()
    if o.shader_dir:
        vs.SHADER_DIR = os.path.abspath(o.shader_dir)
    EXTRA_GLOBALS.extend(tuple(d.split('=', 1)) for d in o.define)
    asics = o.asic or ["gfx1100", "gfx1200"]
    rga, dxc = find_rga(o.rga), vs.find_tool(o.dxc, "dxc.exe")
    suffix = o.tag
    entries, _ = vs.collect_entries()
    entries = sorted(set(entries) | set(EXTRA))
    if o.filter:
        entries = [e for e in entries if o.filter in e[0] or o.filter in e[1]]
    work = os.path.join(o.out, "work" + suffix)
    isa_all = os.path.join(work, "isa")
    os.makedirs(isa_all, exist_ok=True)
    print(f"{len(entries)} entries x {asics} with {rga}")

    rows, fails = [], []
    with concurrent.futures.ThreadPoolExecutor(max_workers=o.jobs) as pool:
        futs = {pool.submit(run_one, rga, dxc, e, asics, work, isa_all): e for e in entries}
        for i, f in enumerate(concurrent.futures.as_completed(futs), 1):
            tag, r, errs = f.result()
            rows += r
            for kind, asic, msg in errs:
                fails.append(dict(shader=tag, stage=kind, asic=asic, message=msg))
                print(f"FAIL [{kind}] {tag} {asic}: {msg[:200]}")
            if i % 20 == 0:
                print(f"  {i}/{len(entries)}")
    rows.sort(key=lambda r: (r["asic"], -r["vgprs"]))
    os.makedirs(o.out, exist_ok=True)
    if rows:
        with open(os.path.join(o.out, f"results{suffix}.csv"), "w", newline="") as fh:
            w = csv.DictWriter(fh, fieldnames=list(rows[0].keys()))
            w.writeheader()
            w.writerows(rows)
    json.dump(dict(results=rows, failures=fails), open(os.path.join(o.out, f"results{suffix}.json"), "w"), indent=1)

    # Keep ISA only for the heaviest shaders.
    keep_dir = os.path.join(o.out, "isa" + suffix)
    os.makedirs(keep_dir, exist_ok=True)
    mine = [r for r in rows if r["asic"] == asics[0]]
    top = mine[: o.isa_top] + [r for r in sorted(mine, key=lambda r: -r["n_total"])[: o.isa_top] if r not in mine[: o.isa_top]]
    for r in top:
        base = f"{os.path.splitext(r['shader'])[0]}.{r['entry']}" + ("." + "_".join(p.replace("=", "") for p in r["defines"].split()) if r["defines"] else "")
        src = os.path.join(isa_all, f"{base}.{asics[0]}.isa.txt")
        if os.path.isfile(src):
            shutil.copyfile(src, os.path.join(keep_dir, os.path.basename(src)))
    shutil.rmtree(isa_all, ignore_errors=True)
    print(f"{len(rows)} result rows, {len(fails)} failures -> {o.out}")


if __name__ == "__main__":
    main()
