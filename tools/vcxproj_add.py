#!/usr/bin/env python3
"""Adds source/header files to D3D11Engine.vcxproj + .filters.

pch.h is force-included project-wide, so .cpp entries need no per-file PCH metadata. Usage:
    python tools/vcxproj_add.py --filter "Engine\\Vulkan" VulkanEngine\\Foo.cpp VulkanEngine\\Foo.h
Files already listed are skipped.
"""
import argparse
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PROJ = os.path.join(ROOT, "D3D11Engine", "D3D11Engine.vcxproj")
FILTERS = PROJ + ".filters"
TEMPLATE_CPP = r"D3D12Engine\D3D12Device.cpp"


def read(p):
    with open(p, encoding="utf-8-sig") as f:
        return f.read()


def write(p, s):
    with open(p, "w", encoding="utf-8-sig", newline="\r\n") as f:
        f.write(s.replace("\r\n", "\n"))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--filter", required=True)
    ap.add_argument("files", nargs="+")
    opts = ap.parse_args()

    proj = read(PROJ).replace("\r\n", "\n")
    filters = read(FILTERS).replace("\r\n", "\n")

    if f'<Filter Include="{opts.filter}">' not in filters:
        guid = "{" + __import__("uuid").uuid4().__str__() + "}"
        entry = f'    <Filter Include="{opts.filter}">\n      <UniqueIdentifier>{guid}</UniqueIdentifier>\n    </Filter>\n'
        filters = filters.replace("  </ItemGroup>\n", entry + "  </ItemGroup>\n", 1)

    for f in opts.files:
        f = f.replace("/", "\\")
        is_cpp = f.endswith(".cpp") or f.endswith(".c")
        tag = "ClCompile" if is_cpp else "ClInclude"
        if f'Include="{f}"' in proj:
            print("skip (already listed)", f)
            continue
        if is_cpp:
            item = f'    <ClCompile Include="{f}" />\n'
            anchor = f'    <ClCompile Include="{TEMPLATE_CPP}" />'
        else:
            item = f'    <ClInclude Include="{f}" />\n'
            anchor = '    <ClInclude Include="D3D12Engine\\D3D12Device.h" />'
        if anchor not in proj:
            sys.exit("anchor not found: " + anchor)
        proj = proj.replace(anchor, item + anchor, 1)

        fitem = f'    <{tag} Include="{f}">\n      <Filter>{opts.filter}</Filter>\n    </{tag}>\n'
        fanchor = f'    <{tag} Include="D3D12Engine\\D3D12Device.{"cpp" if is_cpp else "h"}">'
        if fanchor not in filters:
            sys.exit("filter anchor not found: " + fanchor)
        filters = filters.replace(fanchor, fitem + fanchor, 1)
        print("added", f)

    write(PROJ, proj)
    write(FILTERS, filters)


if __name__ == "__main__":
    main()
