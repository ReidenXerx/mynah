#!/usr/bin/env python3
"""Turn the exploded mascot drawings into one named parts sheet.

    python3 -I assets/bird/tools/group_parts.py \
        assets/bird/source/mynah-dismantled.svg assets/bird/source/mynah-listening-parts.svg \
        assets/bird/parts.svg [--debug assets/bird/parts-debug.svg]

The base pose comes from the dismantled drawing (every part spread out, no
groups, no names). Parts are found by clustering shapes whose bounding boxes
touch, then named by where they sit on that sheet; the near wing and the tail
share a cluster and are split by height. From the listening drawing — a
separate trace, with its own colours — only the raised wing and the three
sound lines are taken, moved to the right of the sheet, and recoloured to the
base palette. Tracing specks under 12 px go. Output: one <g id="…"> per part,
back to front, on a wider canvas. Positions stay exploded: assembling the
bird is the next step, not this script's.
"""
import re
import sys
import xml.etree.ElementTree as ET

SVG = "http://www.w3.org/2000/svg"
ET.register_namespace("", SVG)

# The listening trace's colours, mapped onto the base drawing's.
PALETTE = {
    "#220e0a": "#21120e", "#392c2b": "#392d2d", "#a36b4e": "#966147",
    "#935d41": "#966147", "#fcf0e4": "#f0efee", "#73432e": "#704531",
    "#fcb02f": "#fab032", "#a44c01": "#8e4109", "#fefcfd": "#f0efee",
}
SPECK = 12          # px: anything smaller is a tracing artefact
EXTRAS_DX = 368     # where the listening extras land on the wider sheet
CANVAS = (1900, 1024)

# Where each base part sits on the exploded sheet (x, y of the cluster's
# centre, roughly) → its name. Matched by nearest centre.
BASE_NAMES = {
    (775, 400): "body",
    (1285, 175): "beak",
    (240, 590): "wing-left",     # near wing; shares a cluster with the tail
    (240, 840): "tail",
    (1290, 615): "wing-right",   # far wing, folded
    (1255, 380): "eye",
    (1436, 355): "eye-far",
    (645, 880): "foot-left",
    (980, 880): "foot-right",
}
# Back to front.
Z_ORDER = ["wing-right", "wing-right-raised", "tail", "body", "foot-left", "foot-right",
           "wing-left", "eye-far", "eye", "beak", "sound-lines"]


def tag(e):
    return e.tag.split("}")[-1]


def bbox(e):
    t = tag(e)
    if t == "path":
        xs, ys = [], []
        for m in re.finditer(r"([MLQCAZ])([^MLQCAZ]*)", e.get("d", ""), re.I):
            cmd, args = m.group(1), [float(x) for x in re.findall(r"-?\d+\.?\d*", m.group(2))]
            if cmd.upper() == "A":
                for i in range(5, len(args), 7):
                    xs.append(args[i]); ys.append(args[i + 1])
            else:
                for i in range(0, len(args) - 1, 2):
                    xs.append(args[i]); ys.append(args[i + 1])
        return (min(xs), min(ys), max(xs), max(ys)) if xs else None
    if t == "circle":
        cx, cy, r = (float(e.get(k)) for k in ("cx", "cy", "r"))
        return (cx - r, cy - r, cx + r, cy + r)
    if t == "ellipse":
        cx, cy = float(e.get("cx")), float(e.get("cy"))
        r = max(float(e.get("rx")), float(e.get("ry")))
        return (cx - r, cy - r, cx + r, cy + r)
    return None


def is_speck(b):
    return b is not None and (b[2] - b[0] < SPECK and b[3] - b[1] < SPECK)


def union(a, b):
    return (min(a[0], b[0]), min(a[1], b[1]), max(a[2], b[2]), max(a[3], b[3]))


def touches(a, b, pad=4):
    return a[0] <= b[2] + pad and b[0] <= a[2] + pad and a[1] <= b[3] + pad and b[1] <= a[3] + pad


def cluster(shapes):
    """[(bbox, element)] → list of [bbox, [elements]] by touching boxes."""
    groups = [[b, [e]] for b, e in shapes]
    merged = True
    while merged:
        merged = False
        for i in range(len(groups)):
            for j in range(i + 1, len(groups)):
                if touches(groups[i][0], groups[j][0]):
                    groups[i] = [union(groups[i][0], groups[j][0]), groups[i][1] + groups[j][1]]
                    del groups[j]
                    merged = True
                    break
            if merged:
                break
    return groups


def centre(b):
    return ((b[0] + b[2]) / 2, (b[1] + b[3]) / 2)


def nearest_name(b):
    cx, cy = centre(b)
    return min(BASE_NAMES, key=lambda p: (p[0] - cx) ** 2 + (p[1] - cy) ** 2)


def base_parts(root):
    shapes = []
    for e in root:
        b = bbox(e)
        if b is None or is_speck(b):
            continue
        shapes.append((b, e))
    parts = {}
    for b, elems in cluster(shapes):
        name = BASE_NAMES[nearest_name(b)]
        if name in ("wing-left", "tail"):
            # One cluster: the wing above, the tail below, split by centre height.
            for e in elems:
                eb = bbox(e)
                parts.setdefault("wing-left" if centre(eb)[1] < 740 else "tail", []).append(e)
        else:
            parts.setdefault(name, []).extend(elems)
    return parts


def listening_extras(root):
    """The raised wing (the one <g> on the far right) and the sound lines
    (the three yellow strokes top right)."""
    raised, lines = None, []
    for e in root:
        b = bbox(e) if tag(e) != "g" else None
        if tag(e) == "g":
            gb = None
            for c in e:
                cb = bbox(c)
                gb = cb if gb is None else union(gb, cb)
            if gb and gb[0] > 1150 and gb[3] - gb[1] > 400:
                raised = list(e)
        elif tag(e) == "path" and e.get("fill") == "#fcb02f" and b and b[0] > 1300 and b[1] < 240:
            lines.append(e)
    assert raised and len(lines) == 3, (raised is not None, len(lines))
    return {"wing-right-raised": raised, "sound-lines": lines}


def recolour(e):
    fill = e.get("fill")
    if fill in PALETTE:
        e.set("fill", PALETTE[fill])
    return e


def main(base_path, listening_path, out_path, debug_path=None):
    base = ET.parse(base_path).getroot()
    listening = ET.parse(listening_path).getroot()
    parts = base_parts(base)
    extras = listening_extras(listening)
    for name, elems in extras.items():
        parts[name] = [recolour(e) for e in elems]

    out = ET.Element(f"{{{SVG}}}svg", {"viewBox": f"0 0 {CANVAS[0]} {CANVAS[1]}",
                                        "width": str(CANVAS[0]), "height": str(CANVAS[1])})
    for name in Z_ORDER:
        g = ET.SubElement(out, f"{{{SVG}}}g", {"id": name})
        if name in extras:
            g.set("transform", f"translate({EXTRAS_DX} 0)")
        for e in parts[name]:
            e.tag = f"{{{SVG}}}{tag(e)}"
            g.append(e)
    ET.ElementTree(out).write(out_path, xml_declaration=False, encoding="unicode")

    if debug_path:
        for name in Z_ORDER:
            g = out.find(f"{{{SVG}}}g[@id='{name}']")
            b = None
            for e in g:
                b = bbox(e) if b is None else union(b, bbox(e))
            dx = EXTRAS_DX if name in extras else 0
            ET.SubElement(out, f"{{{SVG}}}rect", {"x": str(b[0] + dx), "y": str(b[1]), "width": str(b[2] - b[0]),
                                                  "height": str(b[3] - b[1]), "fill": "none",
                                                  "stroke": "#4ad", "stroke-width": "3"})
            t = ET.SubElement(out, f"{{{SVG}}}text", {"x": str(b[0] + dx + 6), "y": str(b[1] - 8),
                                                      "fill": "#4ad", "font-size": "34",
                                                      "font-family": "sans-serif", "font-weight": "bold"})
            t.text = f"{name} ({len(list(g))})"
        ET.ElementTree(out).write(debug_path, xml_declaration=False, encoding="unicode")

    for name in Z_ORDER:
        print(f"{name:20s} {len(parts[name]):2d} shapes")


if __name__ == "__main__":
    args = sys.argv[1:]
    debug = None
    if "--debug" in args:
        i = args.index("--debug"); debug = args[i + 1]; del args[i:i + 2]
    main(*args, debug_path=debug)
