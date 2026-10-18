#!/usr/bin/env python3
"""Put the exploded parts together into one bird.

    python3 -I assets/bird/tools/assemble.py assets/bird/parts.svg assets/bird/assembled.svg \
        [--check /tmp/assembled.png]

The parts sheet records no positions. Matching the parts against the
reference PNGs by pixels was tried first and dropped: the exploded sheet
and the assembled reference are two different drawings with different
proportions, so nothing locks on. Instead each part is placed from its own
geometry — the eye a quarter of the way down the head, the beak against the
head's front edge, the feet overlapping the belly — with the proportions
read off the reference by eye. The numbers in LAYOUT are the thing to tune;
the final nudging happens on the Creator canvas.

Output: one SVG with every <g id="…"> given a translate(), back to front,
on a canvas cropped to the bird (the idle pose) plus room for the raised
wing and the sound lines above-right.
"""
import io
import re
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
from pathlib import Path

import numpy as np
from PIL import Image

SVG = "http://www.w3.org/2000/svg"
ET.register_namespace("", SVG)

# Where each part's box goes, in sheet units, relative to the body's box
# (x0, y0, w, h): fractions of the body box for a centre or a corner,
# or "edge" = against the head's painted right edge at that height.
LAYOUT = {
    "eye":              {"centre": (0.515, 0.26)},
    "eye-far":          {"centre_y": 0.23, "right_of_edge": -26},
    "beak":             {"centre_y": 0.33, "left_of_edge": -67},
    "wing-left":        {"centre": (0.00, 0.54)},
    "tail":             {"centre": (-0.29, 0.89)},
    "foot-left":        {"centre": (0.15, 1.03)},
    "foot-right":       {"centre": (0.64, 1.03)},
    "wing-right":       {"top_left": (0.47, 0.36)},   # folded, hidden behind the body
    "wing-right-raised": {"top_left": (0.82, 0.14)},
    "sound-lines":      {"top_left": (1.16, -0.02)},
}
MARGIN = 40
CANVAS = 1024  # Creator's scene is square


def bbox_of(svg_text, width):
    with tempfile.NamedTemporaryFile("w", suffix=".svg", delete=False) as f:
        f.write(svg_text)
    png = subprocess.run(["rsvg-convert", "-w", str(width), f.name], capture_output=True, check=True).stdout
    Path(f.name).unlink()
    a = np.asarray(Image.open(io.BytesIO(png)).convert("RGBA"))
    return a[..., 3] > 8


def main(sheet_path, out_path, check_path=None):
    root = ET.parse(sheet_path).getroot()
    vb = [float(v) for v in root.get("viewBox").split()]
    groups = {g.get("id"): g for g in root.findall(f"{{{SVG}}}g")}

    # Pixel-exact boxes, and the body's painted right edge per row.
    masks = {}
    for name, g in groups.items():
        doc = ET.Element(f"{{{SVG}}}svg", {"viewBox": root.get("viewBox")})
        doc.append(g)
        masks[name] = bbox_of(ET.tostring(doc, encoding="unicode"), int(vb[2]))
    boxes = {}
    for name, m in masks.items():
        ys, xs = np.nonzero(m)
        boxes[name] = (int(xs.min()), int(ys.min()), int(xs.max() + 1), int(ys.max() + 1))
    # The listening extras were shifted onto the sheet; undo that for them.
    bx0, by0, bx1, by1 = boxes["body"]
    bw, bh = bx1 - bx0, by1 - by0

    def head_right_edge(y):
        row = np.nonzero(masks["body"][int(y)])[0]
        return int(row.max()) if row.size else bx1

    placed = {"body": (0, 0)}
    for name, rule in LAYOUT.items():
        x0, y0, x1, y1 = boxes[name]
        w, h = x1 - x0, y1 - y0
        if "centre" in rule:
            cx, cy = bx0 + rule["centre"][0] * bw, by0 + rule["centre"][1] * bh
            tx, ty = cx - w / 2, cy - h / 2
        elif "top_left" in rule:
            tx, ty = bx0 + rule["top_left"][0] * bw, by0 + rule["top_left"][1] * bh
        else:
            cy = by0 + rule["centre_y"] * bh
            edge = head_right_edge(cy)
            ty = cy - h / 2
            tx = edge + rule["right_of_edge"] - w if "right_of_edge" in rule else edge + rule["left_of_edge"]
        placed[name] = (tx - x0, ty - y0)

    # Canvas: the assembled extent plus a margin.
    ext = []
    for name, (dx, dy) in placed.items():
        x0, y0, x1, y1 = boxes[name]
        ext.append((x0 + dx, y0 + dy, x1 + dx, y1 + dy))
    X0 = min(e[0] for e in ext) - MARGIN; Y0 = min(e[1] for e in ext) - MARGIN
    X1 = max(e[2] for e in ext) + MARGIN; Y1 = max(e[3] for e in ext) + MARGIN
    W, H = X1 - X0, Y1 - Y0

    out = ET.Element(f"{{{SVG}}}svg", {"viewBox": f"0 0 {W:.0f} {H:.0f}", "width": f"{W:.0f}", "height": f"{H:.0f}"})
    final = {}
    for g in root.findall(f"{{{SVG}}}g"):  # the sheet's order is back to front
        name = g.get("id")
        dx, dy = placed[name]
        # The boxes were measured with the sheet's own transform applied
        # (the listening extras are shifted onto the sheet), so compose.
        sheet = g.get("transform", "")
        m = re.match(r"translate\(\s*(-?[\d.]+)[ ,]+(-?[\d.]+)\s*\)", sheet)
        sx, sy = (float(m.group(1)), float(m.group(2))) if m else (0.0, 0.0)
        final[name] = (dx - X0 + sx, dy - Y0 + sy)
        g.set("transform", f"translate({final[name][0]:.1f} {final[name][1]:.1f})")
        out.append(g)
    ET.ElementTree(out).write(out_path, encoding="unicode")

    # One SVG per part on the Creator canvas (CANVAS square), the bird
    # scaled to fit its width and centred vertically, for import_asset:
    # every layer then shares the same frame and needs no positioning.
    import_dir = Path(out_path).with_name("import")
    import_dir.mkdir(exist_ok=True)
    f = CANVAS / W
    oy = (CANVAS - H * f) / 2
    for g in out.findall(f"{{{SVG}}}g"):
        name = g.get("id")
        doc = ET.Element(f"{{{SVG}}}svg", {"viewBox": f"0 0 {CANVAS} {CANVAS}", "width": str(CANVAS), "height": str(CANVAS)})
        wrap = ET.SubElement(doc, f"{{{SVG}}}g", {"id": name, "transform": f"translate(0 {oy:.1f}) scale({f:.4f}) translate({final[name][0]:.1f} {final[name][1]:.1f})"})
        for child in list(g):
            wrap.append(child)
        ET.ElementTree(doc).write(import_dir / f"{name}.svg", encoding="unicode")
    print(f"import files in {import_dir}/ (canvas {CANVAS}, scale {f:.4f}, y offset {oy:.1f})")
    print(f"assembled canvas {W:.0f}×{H:.0f}; body box on it: "
          f"({bx0 - X0:.0f},{by0 - Y0:.0f}) {bw}×{bh}")
    for name, (dx, dy) in placed.items():
        x0, y0, x1, y1 = boxes[name]
        print(f"  {name:18s} box ({x0 + dx - X0:5.0f},{y0 + dy - Y0:5.0f})–({x1 + dx - X0:5.0f},{y1 + dy - Y0:5.0f})")
    if check_path:
        # Two renders: the idle pose (no listening parts) and the listening
        # pose (raised wing instead of the folded one, sound lines on).
        for pose, hidden in (("idle", {"wing-right-raised", "sound-lines"}), ("listening", {"wing-right"})):
            doc = ET.parse(out_path).getroot()
            for g in doc.findall(f"{{{SVG}}}g"):
                if g.get("id") in hidden:
                    g.set("display", "none")
            tmp = Path(check_path).with_name(f"{Path(check_path).stem}-{pose}.svg")
            ET.ElementTree(doc).write(tmp, encoding="unicode")
            png = tmp.with_suffix(".png")
            subprocess.run(["rsvg-convert", "-w", "900", "--background-color=#3b4252", "-o", str(png), str(tmp)], check=True)
            print("check render:", png)


if __name__ == "__main__":
    a = sys.argv[1:]
    check = None
    if "--check" in a:
        i = a.index("--check"); check = a[i + 1]; del a[i:i + 2]
    main(*a, check_path=check)
