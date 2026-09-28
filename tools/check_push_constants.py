#!/usr/bin/env python3
"""Checks the Vulkan RHI's uniform-to-push-constant SPIR-V rewrite (VulkanEngine/VulkanSpirvPatch.h) on the real shaders.

Compiles every entry point of the given HLSL files with the engine's SPIR-V arguments, applies the rewrite the RHI
applies for the World root signature (vertex stage: b4 at offset 16, 48 bytes; pixel stage: b6 at offset 0, 16 bytes)
through tools/spirv_push_test, and runs spirv-val on each rewritten module.

Usage:  python tools/check_push_constants.py [--dxc PATH] [--debug] [--rules vs:4:16:48,ps:6:0:16] [files...]   (default: World.hlsl Vob.hlsl DepthPrepass.hlsl)
Build tools/spirv_push_test/spirv_push_test.exe first (see its header).
"""
import argparse
import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import validate_spirv as vs  # noqa: E402

TOOL = os.path.join(os.path.dirname(os.path.abspath(__file__)), "spirv_push_test", "spirv_push_test.exe")
# stage prefix -> (binding, offset, range size)
RULES = {"vs": (4, 16, 48), "ps": (6, 0, 16)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dxc")
    ap.add_argument("--debug", action="store_true", help="-Zi -O1 like DEBUG_D3D11 builds instead of -O3")
    ap.add_argument("--rules", help="stage:binding:offset:size,... (default: the World root signature's)")
    ap.add_argument("files", nargs="*", default=["World.hlsl", "Vob.hlsl", "DepthPrepass.hlsl"])
    opts = ap.parse_args()
    if opts.debug:
        i = vs.SPIRV_ARGS.index("-O3")
        vs.SPIRV_ARGS[i:i + 1] = ["-Zi", "-O1"]
    rules = dict(RULES)
    if opts.rules:
        rules = {}
        for rule in opts.rules.split(","):
            stage, binding, offset, size = rule.split(":")
            rules[stage] = (int(binding), int(offset), int(size))
    dxc = vs.find_tool(opts.dxc, "dxc.exe")
    spirv_val = vs.find_tool(None, "spirv-val.exe")
    if not os.path.isfile(TOOL):
        sys.exit(f"{TOOL} missing; build it first")

    entries, _ = vs.collect_entries()
    entries = [e for e in entries if e[0] in opts.files and e[2][:2] in rules]
    counts = {"lowered": 0, "absent": 0, "unsupported": 0, "invalid": 0}
    with tempfile.TemporaryDirectory() as tmp:
        for file, ep, target, defines in entries:
            macro_args = []
            for n, v in list(defines) + vs.GLOBAL_MACRO_SETS[0]:
                macro_args += ["-D", f"{n}={v}"]
            src = os.path.join(tmp, "in.spv")
            r = subprocess.run([dxc, *vs.SPIRV_ARGS, "-I", ".", "-E", ep, "-T", target, *macro_args, file, "-Fo", src],
                               cwd=vs.SHADER_DIR, capture_output=True, text=True)
            if r.returncode != 0:
                print(f"COMPILE {file}:{ep}: {(r.stderr or r.stdout).strip().splitlines()[0]}")
                continue
            binding, offset, size = rules[target[:2]]
            dst = os.path.join(tmp, "out.spv")
            run = subprocess.run([TOOL, src, dst, str(binding), str(offset), str(size)], capture_output=True, text=True)
            code = run.returncode
            name = f"{file}:{ep} ({target}, b{binding})"
            if code == 1:
                counts["absent"] += 1
                continue
            if code != 0:
                counts["unsupported"] += 1
                print(f"UNSUPPORTED {name}: {run.stdout.strip()}")
                continue
            v = subprocess.run([spirv_val, "--target-env", "vulkan1.3", "--scalar-block-layout", dst], capture_output=True, text=True)
            if v.returncode != 0:
                counts["invalid"] += 1
                print(f"INVALID {name}\n  {(v.stderr or v.stdout).strip()}")
            else:
                counts["lowered"] += 1
    print(", ".join(f"{v} {k}" for k, v in counts.items()))
    return 1 if counts["invalid"] else 0


if __name__ == "__main__":
    sys.exit(main())
