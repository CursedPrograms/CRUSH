#!/usr/bin/env python3
"""Generate docs/benchmark.svg from benchmark numbers.

Re-run after re-benchmarking to refresh the README chart:
    python make_chart.py
Colours are chosen to read on both light and dark GitHub themes.
"""
import os

# (label, compressed ratio % of original, time seconds, is_ours)
DATA = [
    ("CRUSH v3  (ours)", 15.1, 0.69, True),
    ("xz -9  (LZMA / 7-zip)", 17.7, 0.26, False),
    ("bzip2 -9  (BWT)", 18.3, 0.13, False),
    ("gzip -9  (DEFLATE)", 22.4, 0.10, False),
]
CORPUS = "863 KB of C++ source + English prose"

W, ROW_H, TOP, LEFT, BARMAX = 760, 46, 92, 250, 420
H = TOP + ROW_H * len(DATA) + 44
scale_max = max(d[1] for d in DATA) * 1.08

OURS = "#F2A33A"       # amber — our bar
OTHER = "#5B8DEF"      # blue — the others
AXIS = "#8b949e"       # grey that reads on white and on dark
TITLE = "#768390"

def bar_w(r):
    return BARMAX * r / scale_max

rows = []
y = TOP
for label, ratio, t, ours in DATA:
    bw = bar_w(ratio)
    fill = OURS if ours else OTHER
    weight = "700" if ours else "500"
    rows.append(f'''  <text x="{LEFT-14}" y="{y+22}" text-anchor="end" font-size="14" font-weight="{weight}" fill="{AXIS}">{label}</text>
  <rect x="{LEFT}" y="{y+6}" width="{bw:.1f}" height="24" rx="5" fill="{fill}"/>
  <text x="{LEFT+bw+10:.1f}" y="{y+22}" font-size="13" font-weight="{weight}" fill="{AXIS}">{ratio:.1f}%  ·  {t:.2f}s</text>''')
    y += ROW_H

svg = f'''<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}" font-family="-apple-system,Segoe UI,Helvetica,Arial,sans-serif">
  <text x="{LEFT-14}" y="34" text-anchor="end" font-size="17" font-weight="700" fill="{AXIS}">CRUSH vs popular compressors</text>
  <text x="{LEFT-14}" y="56" text-anchor="end" font-size="12.5" fill="{TITLE}">{CORPUS} — compressed size as % of original (shorter = better)</text>
  <line x1="{LEFT}" y1="{TOP-8}" x2="{LEFT}" y2="{TOP + ROW_H*len(DATA)}" stroke="{AXIS}" stroke-width="1" opacity="0.4"/>
{os.linesep.join(rows)}
  <text x="{LEFT}" y="{H-14}" font-size="11.5" fill="{TITLE}">CRUSH is a from-scratch context-mixing coder — no LZMA, no zlib. It already beats gzip and bzip2 and trails LZMA by &lt;1%.</text>
</svg>
'''

os.makedirs("docs", exist_ok=True)
with open("docs/benchmark.svg", "w", encoding="utf-8") as f:
    f.write(svg)
print("wrote docs/benchmark.svg")
