#!/usr/bin/env python3
"""Offline SPIR-V validation of the D3D12 HLSL shaders (VULKAN_IMPLEMENTATION_PLAN.md, Phase 0).

MSBuild never compiles HLSL: both backends compile at runtime. This script finds every
(file, entry, target, defines) the D3D12 backend passes to CompileFromFile, compiles each with the
same DXC SPIR-V arguments the engine uses (D3D12ShaderBackend.cpp: AppendSpirvArguments) and runs
spirv-val on the result.

Usage:  python tools/validate_spirv.py [--dxc PATH] [--spirv-val PATH] [--filter SUBSTR] [--keep DIR]
Needs the Vulkan SDK (dxc.exe with SPIR-V codegen + spirv-val.exe); found via VULKAN_SDK or C:\\VulkanSDK.
"""
import argparse
import concurrent.futures
import glob
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ENGINE_DIR = os.path.join(ROOT, "D3D11Engine", "D3D12Engine")
SHADER_DIR = os.path.join(ROOT, "D3D11Engine", "Shaders", "D3D12")

# Keep in sync with AppendSpirvArguments + the release flags in D3D12ShaderBackend.cpp.
SPIRV_ARGS = [
    "-spirv", "-fspv-target-env=vulkan1.3", "-fvk-use-dx-layout",
    "-fspv-use-unknown-image-format", "-fvk-support-nonzero-base-instance",
    "-fvk-b-shift", "0", "0", "-fvk-t-shift", "16", "0",
    "-fvk-u-shift", "32", "0", "-fvk-s-shift", "48", "0",
    "-fvk-bind-resource-heap", "0", "1",
    "-O3", "-enable-16bit-types",
]

# D3D12ShaderBackend::AppendGlobalMacros adds these to every compile; cover each value once.
GLOBAL_MACRO_SETS = [
    [("NORMAL_MAP_RESTORE_Z", "0"), ("NORMAL_MAP_MODE", "1")],
    [("NORMAL_MAP_RESTORE_Z", "1"), ("NORMAL_MAP_MODE", "2")],
]

TARGET_ALIASES = {"Shadermodel_VS": "vs_6_6", "Shadermodel_PS": "ps_6_6", "Shadermodel_CS": "cs_6_6"}

# Call name -> index of the defines argument.
CALLS = {"CompileFromFile": 4, "makeComputePSO": 5}


def find_tool(explicit, name):
    if explicit:
        return explicit
    candidates = []
    if os.environ.get("VULKAN_SDK"):
        candidates.append(os.path.join(os.environ["VULKAN_SDK"], "Bin", name))
    candidates += sorted(glob.glob(os.path.join("C:\\", "VulkanSDK", "*", "Bin", name)), reverse=True)
    for c in candidates:
        if os.path.isfile(c):
            return c
    sys.exit(f"{name} not found; pass it explicitly or set VULKAN_SDK")


def split_args(text, start):
    """Splits the argument list of the call whose '(' is at text[start]. Returns (args, end)."""
    depth, args, cur, i, in_str = 0, [], [], start, False
    while i < len(text):
        ch = text[i]
        if in_str:
            cur.append(ch)
            if ch == "\\":
                cur.append(text[i + 1])
                i += 1
            elif ch == '"':
                in_str = False
        elif ch == '"':
            in_str = True
            cur.append(ch)
        elif ch in "([{":
            depth += 1
            if depth > 1:
                cur.append(ch)
        elif ch in ")]}":
            depth -= 1
            if depth == 0:
                args.append("".join(cur).strip())
                return args, i
            cur.append(ch)
        elif ch == "," and depth == 1:
            args.append("".join(cur).strip())
            cur = []
        else:
            cur.append(ch)
        i += 1
    return args, i


def parse_macro_arrays(text):
    arrays = {}
    for m in re.finditer(r"D3D_SHADER_MACRO\s+(\w+)\[\]\s*=\s*\{(.*?)\};", text, re.S):
        pairs = re.findall(r'\{\s*"(\w+)"\s*,\s*"([^"]*)"\s*\}', m.group(2))
        arrays[m.group(1)] = pairs
    return arrays


def collect_entries():
    entries, unresolved = set(), []
    for path in sorted(glob.glob(os.path.join(ENGINE_DIR, "*.cpp"))):
        text = open(path, encoding="utf-8", errors="replace").read()
        macros = parse_macro_arrays(text)
        for call, defines_index in CALLS.items():
            for m in re.finditer(r"\b" + call + r"\s*\(", text):
                args, _ = split_args(text, m.end() - 1)
                if len(args) < 3 or not args[0].startswith('"') or not args[1].startswith('"'):
                    continue   # the helper's own definition / forwarding body
                target = TARGET_ALIASES.get(args[2], args[2].strip('"'))
                if call == "makeComputePSO":
                    target = "cs_6_6"
                defines = ()
                if len(args) > defines_index and args[defines_index] not in ("nullptr", ""):
                    name = args[defines_index]
                    if name not in macros:
                        unresolved.append(f"{os.path.basename(path)}: defines '{name}' for {args[0]} {args[1]}")
                        continue
                    defines = tuple(macros[name])
                entries.add((args[0].strip('"'), args[1].strip('"'), target, defines))
    return sorted(entries), unresolved


def run_one(dxc, spirv_val, entry, globals_, keep_dir):
    file, ep, target, defines = entry
    macro_args = []
    for name, value in list(defines) + globals_:
        macro_args += ["-D", f"{name}={value}"]
    with tempfile.TemporaryDirectory() as tmp:
        out = os.path.join(keep_dir or tmp, f"{file}.{ep}.{abs(hash((defines, tuple(globals_))))}.spv")
        cmd = [dxc, *SPIRV_ARGS, "-I", ".", "-E", ep, "-T", target, *macro_args, file, "-Fo", out]
        r = subprocess.run(cmd, cwd=SHADER_DIR, capture_output=True, text=True)
        if r.returncode != 0:
            return False, "compile", (r.stderr or r.stdout).strip()
        v = subprocess.run([spirv_val, "--target-env", "vulkan1.3", "--scalar-block-layout", out],
                           capture_output=True, text=True)
        if v.returncode != 0:
            return False, "validate", (v.stderr or v.stdout).strip()
        return True, "", ""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dxc")
    ap.add_argument("--spirv-val")
    ap.add_argument("--filter", help="only files/entries containing this substring")
    ap.add_argument("--keep", help="keep the .spv files in this directory")
    ap.add_argument("--list", action="store_true", help="only list the extracted entries")
    opts = ap.parse_args()

    entries, unresolved = collect_entries()
    if opts.filter:
        entries = [e for e in entries if opts.filter in e[0] or opts.filter in e[1]]
    for u in unresolved:
        print("UNRESOLVED", u)
    if opts.list:
        for e in entries:
            print(*e[:3], " ".join(f"{n}={v}" for n, v in e[3]))
        print(f"{len(entries)} entries")
        return 0

    dxc = find_tool(opts.dxc, "dxc.exe")
    spirv_val = find_tool(opts.spirv_val, "spirv-val.exe")
    if opts.keep:
        os.makedirs(opts.keep, exist_ok=True)

    jobs = [(e, g) for e in entries for g in GLOBAL_MACRO_SETS]
    failures = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
        futures = {pool.submit(run_one, dxc, spirv_val, e, g, opts.keep): (e, g) for e, g in jobs}
        for f in concurrent.futures.as_completed(futures):
            ok, stage, msg = f.result()
            if not ok:
                failures += 1
                (file, ep, target, defines), g = futures[f]
                print(f"FAIL [{stage}] {file}:{ep} ({target}) {dict(defines + tuple(g))}\n  {msg}\n")
    print(f"{len(entries)} entries x {len(GLOBAL_MACRO_SETS)} global macro sets: "
          f"{len(jobs) - failures} passed, {failures} failed, {len(unresolved)} unresolved")
    return 1 if failures or unresolved else 0


if __name__ == "__main__":
    sys.exit(main())
