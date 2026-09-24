#!/usr/bin/env python3
"""Adds source/header files that live in a D3D11Engine subfolder to D3D11Engine.vcxproj + .filters.

Subfolder .cpp files need the per-config `../pch.h` PrecompiledHeaderFile override (CLAUDE.md), which is
copied from D3D12Engine\\D3D12Device.cpp's entry. Usage:
    python tools/vcxproj_add.py --filter "Engine\\Vulkan" VulkanEngine\\Foo.cpp VulkanEngine\\Foo.h
Files already listed are skipped.
"""
import argparse
import os
import re
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
    ap.add_argument("--no-pch-override", action="store_true", help="for .cpp files outside a subfolder")
    ap.add_argument("files", nargs="+")
    opts = ap.parse_args()

    proj = read(PROJ).replace("\r\n", "\n")
    filters = read(FILTERS).replace("\r\n", "\n")

    m = re.search(r'    <ClCompile Include="' + re.escape(TEMPLATE_CPP) + r'">\n(.*?)    </ClCompile>\n', proj, re.S)
    if not m:
        sys.exit("template entry not found")
    pch_block = m.group(1)

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
            body = "" if opts.no_pch_override else pch_block
            item = f'    <ClCompile Include="{f}">\n{body}    </ClCompile>\n' if body else f'    <ClCompile Include="{f}" />\n'
            anchor = f'    <ClCompile Include="{TEMPLATE_CPP}">'
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
