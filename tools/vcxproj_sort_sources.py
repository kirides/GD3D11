#!/usr/bin/env python3
"""Orders D3D11Engine.vcxproj's <ClCompile> items largest file first.

MSBuild's /MP hands sources to the compiler processes in list order, so a big file listed late starts
last and stretches the build's tail. Re-run after adding or growing large sources:
    python tools/vcxproj_sort_sources.py
"""
import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PROJ_DIR = os.path.join(ROOT, "D3D11Engine")
PROJ = os.path.join(PROJ_DIR, "D3D11Engine.vcxproj")
ITEM = re.compile(r'[ \t]*<ClCompile Include="([^"]+)"\s*(?:/>|>.*?</ClCompile>)[ \t]*\r?\n', re.S)


def size_of(item):
    path = os.path.join(PROJ_DIR, ITEM.match(item).group(1).replace("\\", os.sep))
    return os.path.getsize(path) if os.path.exists(path) else 0


def sort_group(match):
    body = match.group(1)
    items = [m.group(0) for m in ITEM.finditer(body)]
    if len(items) < 2:
        return match.group(0)
    rest = ITEM.sub("", body)
    indent = rest[len(rest.rstrip(" \t")):]  # the closing tag's indentation
    rest = rest[:len(rest) - len(indent)]
    return "<ItemGroup>" + rest + "".join(sorted(items, key=size_of, reverse=True)) + indent + "</ItemGroup>"


def main():
    with open(PROJ, encoding="utf-8-sig", newline="") as f:
        text = f.read()
    result = re.sub(r"<ItemGroup>(.*?)</ItemGroup>", sort_group, text, flags=re.S)
    assert len(ITEM.findall(result)) == len(ITEM.findall(text))
    if result != text:
        with open(PROJ, "w", encoding="utf-8-sig", newline="") as f:
            f.write(result)
        print("sorted", PROJ)
    else:
        print("already sorted")


if __name__ == "__main__":
    main()
