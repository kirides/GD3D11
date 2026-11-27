#!/usr/bin/env python3
"""Prints markdown ranking tables from a results CSV (written by analyze_shaders.py) for the --asic rows, with --cmp ASIC in the last column.

Usage: python tools/shader_analysis/make_tables.py [--top N] [--csv PATH] [--asic gfx1035] [--cmp gfx1100]
Waves/SIMD: wave32 / wave64 columns (VGPR file model in analyze_shaders.py). Pixel/vertex stages run wave64 on AMD.
"""
import argparse
import csv
import os

HERE = os.path.dirname(os.path.abspath(__file__))


def row(x, y):
    d = x["defines"].replace("USE_FP16_LIGHTING=0", "FP16=0")
    w64 = x["waves_w64"]
    return (f"| {x['shader'][:-5]} | {x['entry']} | {d} | {x['stage']} | {x['vgprs']} | {x['sgprs']} | {x['lds']} | "
            f"{x['scratch']}/{x['vgpr_spills']} | {x['n_total']} | {x['n_image_sample']} | {x['n_trans']} | "
            f"{x['waves_per_simd']} / {w64} | {y['vgprs'] if y else '-'} |")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--top", type=int, default=30)
    ap.add_argument("--csv", default=os.path.join(HERE, "results_rga212.csv"))
    ap.add_argument("--asic", default="gfx1035")
    ap.add_argument("--cmp", default="gfx1100")
    o = ap.parse_args()
    rows = list(csv.DictReader(open(o.csv)))
    g = [x for x in rows if x["asic"] == o.asic]
    r4 = {(x["shader"], x["entry"], x["defines"]): x for x in rows if x["asic"] == o.cmp}
    hdr = ("| file | entry | defines | st | VGPR | SGPR | LDS B | scratch/spill | ISA instr | samples | transc | waves w32 / w64 | VGPR cmp |\n"
           "|---|---|---|---|---|---|---|---|---|---|---|---|---|")
    for title, key in (("By VGPR (occupancy pressure)", lambda x: (-int(x["vgprs"]), -int(x["n_total"]))),
                       ("By ISA instruction count", lambda x: -int(x["n_total"])),
                       ("By LDS", lambda x: -int(x["lds"]))):
        print(f"\n### {title}\n\n{hdr}")
        for x in sorted(g, key=key)[: o.top]:
            print(row(x, r4.get((x["shader"], x["entry"], x["defines"]))))
    spills = [x for x in g if int(x["scratch"]) or int(x["vgpr_spills"]) or int(x["sgpr_spills"])]
    print(f"\nShaders with scratch or spills: {len(spills)} of {len(g)}")


if __name__ == "__main__":
    main()
