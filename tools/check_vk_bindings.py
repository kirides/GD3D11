#!/usr/bin/env python3
"""Checks the Vulkan root-signature lowering against the shaders (VULKAN_IMPLEMENTATION_PLAN.md 5.3).

VulkanRhiPipeline.cpp lowers every root parameter to a set-0 push descriptor with a fixed type: root
constants/CBVs -> uniform buffer, root SRV/UAVs -> storage buffer, table SRVs -> sampled image, table UAVs
-> storage image, static samplers -> sampler. A shader that declares something else at such a register
builds an invalid pipeline. For each function in D3D12Engine/*.cpp that declares a D3D12RootLayout, this
compiles the entry points the same function compiles and checks every set-0 binding they use.

Usage:  python tools/check_vk_bindings.py [--dxc PATH]   (same DXC lookup as validate_spirv.py)
"""
import argparse
import os
import re
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import validate_spirv as vs  # noqa: E402

SHIFT = {"b": 0, "t": 16, "u": 32, "s": 48}


def layout_bindings(chunk):
    """Vulkan binding -> expected type for every root layout declared in `chunk` (all layouts merged)."""
    layouts = {}
    for m in re.finditer(r"D3D12RootLayout&?\s+(\w+)\s*(?:=|;)", chunk):
        layouts.setdefault(m.group(1), {})
    for m in re.finditer(r"\b(\w+)\.(AddConstants|AddCBV|AddSRV|AddUAV|AddTable|AddStaticSampler)\s*\(", chunk):
        var, fn = m.group(1), m.group(2)
        if var not in layouts:
            continue
        args, _ = vs.split_args(chunk, m.end() - 1)
        out = layouts[var]
        first = args[0] if args else ""
        if fn in ("AddConstants", "AddCBV") and first.isdigit():
            out[SHIFT["b"] + int(first)] = "uniform"
        elif fn == "AddSRV" and first.isdigit():
            out[SHIFT["t"] + int(first)] = "storage-buffer"
        elif fn == "AddUAV" and first.isdigit():
            out[SHIFT["u"] + int(first)] = "storage-buffer"
        elif fn == "AddTable":
            for r in re.finditer(r"(SRV|UAV|CBV)Range\(\s*(\w+)\s*(?:,\s*(\d+))?", ",".join(args)):
                cls, base, count = r.group(1), r.group(2), int(r.group(3) or 1)
                kind = {"SRV": ("t", "sampled-image"), "UAV": ("u", "storage-image"), "CBV": ("b", "uniform")}[cls]
                if base.isdigit():
                    regs = [int(base) + k for k in range(count)]
                else:   # `for ( UINT i = A; i < B; ++i ) rs.AddTable( SRVRange( i ) ... )`
                    loop = re.findall(r"for\s*\(\s*\w+\s+" + base + r"\s*=\s*(\d+)\s*;\s*" + base + r"\s*<\s*(\d+)",
                                      chunk[:m.start()])
                    regs = list(range(int(loop[-1][0]), int(loop[-1][1]))) if loop else []
                for reg in regs:
                    out[SHIFT[kind[0]] + reg] = kind[1]
        elif fn == "AddStaticSampler":
            r = re.search(r"Sampler\w*\(\s*(\d+)", first)
            reg = int(r.group(1)) if r else None
            if reg is None and re.fullmatch(r"\w+", first):   # a sampler desc built in a variable
                a1 = re.findall(first + r"\s*=\s*D3D12RootLayout::Sampler\w*\(\s*(\d+)", chunk[:m.start()])
                a2 = re.findall(first + r"\.ShaderRegister\s*=\s*(\d+)", chunk[:m.start()])
                reg = int((a2 or a1)[-1]) if ( a2 or a1 ) else None
            if reg is not None:
                out[SHIFT["s"] + reg] = "sampler"
    return layouts


def spirv_bindings(path):
    """Set-0 binding -> type for the resources the entry point's interface lists."""
    data = open(path, "rb").read()
    words = struct.unpack(f"<{len(data) // 4}I", data)
    deco, types, ptrs, variables, interface = {}, {}, {}, {}, set()
    i = 5
    while i < len(words):
        op, wc = words[i] & 0xFFFF, words[i] >> 16
        ins = words[i:i + wc]
        if op == 15:                       # OpEntryPoint
            name_words = 0
            for j in range(3, wc):         # skip the literal name, then the interface ids
                name_words += 1
                if (words[i + j] >> 24) == 0:
                    break
            interface.update(ins[3 + name_words:])
        elif op == 71 and wc >= 3:         # OpDecorate
            deco.setdefault(ins[1], {})[ins[2]] = ins[3] if wc > 3 else True
        elif op == 25:                     # OpTypeImage: id, sampled type, dim, depth, arrayed, ms, sampled
            types[ins[1]] = ("image", ins[7])
        elif op == 26:
            types[ins[1]] = ("sampler",)
        elif op in (28, 29):               # arrays: element type
            types[ins[1]] = ("array", ins[2])
        elif op == 30:
            types[ins[1]] = ("struct",)
        elif op == 32:                     # OpTypePointer: id, storage class, pointee
            ptrs[ins[1]] = (ins[2], ins[3])
        elif op == 59:                     # OpVariable: result type, id, storage class
            variables[ins[2]] = (ins[1], ins[3])
        i += wc

    out = {}
    for var, (ptype, storage) in variables.items():
        d = deco.get(var, {})
        if d.get(34) != 0 or 33 not in d or (interface and var not in interface):
            continue
        pointee = ptrs.get(ptype, (None, None))[1]
        t = types.get(pointee)
        while t and t[0] == "array":
            pointee = t[1]
            t = types.get(pointee)
        if storage == 12 or (storage == 2 and 3 in deco.get(pointee, {})):
            kind = "storage-buffer"
        elif storage == 2:
            kind = "uniform"
        elif t and t[0] == "image":
            kind = "storage-image" if t[1] == 2 else "sampled-image"
        elif t and t[0] == "sampler":
            kind = "sampler"
        else:
            kind = f"storage{storage}"
        out[d[33]] = kind
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dxc")
    opts = ap.parse_args()
    dxc = vs.find_tool(opts.dxc, "dxc.exe")

    problems = checked = 0
    with tempfile.TemporaryDirectory() as tmp:
        for path in sorted(vs.glob.glob(os.path.join(vs.ENGINE_DIR, "*.cpp"))):
            text = open(path, encoding="utf-8", errors="replace").read()
            macros = vs.parse_macro_arrays(text)
            # Top-level function bodies: a definition line ending in '{' at column 0.
            starts = [m.start() for m in re.finditer(r"^\S[^\n;]*\)\s*(?:const\s*)?\{\s*$", text, re.M)] + [len(text)]
            for a, b in zip(starts, starts[1:]):
                chunk = text[a:b]
                layouts = layout_bindings(chunk)
                if not any(layouts.values()):
                    continue
                for call, di in vs.CALLS.items():
                    for m in re.finditer(r"\b" + call + r"\s*\(", chunk):
                        args, _ = vs.split_args(chunk, m.end() - 1)
                        if len(args) < 3 or not args[0].startswith('"') or not args[1].startswith('"'):
                            continue
                        file, ep = args[0].strip('"'), args[1].strip('"')
                        target = "cs_6_6" if call == "makeComputePSO" else vs.TARGET_ALIASES.get(args[2], args[2].strip('"'))
                        defines = []
                        if len(args) > di and args[di] in macros:
                            defines = macros[args[di]]
                        macro_args = []
                        for n, v in defines + vs.GLOBAL_MACRO_SETS[0]:
                            macro_args += ["-D", f"{n}={v}"]
                        out = os.path.join(tmp, f"{file}.{ep}.spv")
                        r = subprocess.run([dxc, *vs.SPIRV_ARGS, "-I", ".", "-E", ep, "-T", target, *macro_args, file, "-Fo", out],
                                           cwd=vs.SHADER_DIR, capture_output=True, text=True)
                        if r.returncode != 0:
                            print(f"COMPILE {file}:{ep}: {(r.stderr or r.stdout).strip().splitlines()[0]}")
                            continue
                        checked += 1
                        head = chunk.split("\n", 1)[0].strip()[:70]
                        for binding, kind in sorted(spirv_bindings(out).items()):
                            expected = {layouts[v][binding] for v in layouts if binding in layouts[v]}
                            if not expected:
                                print(f"UNBOUND  {file}:{ep} binding {binding} ({kind}) — no layout in `{head}`")
                                problems += 1
                            elif kind not in expected:
                                print(f"MISMATCH {file}:{ep} binding {binding}: shader {kind}, layout {sorted(expected)} in `{head}`")
                                problems += 1
    print(f"{checked} entry points checked, {problems} problems")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
