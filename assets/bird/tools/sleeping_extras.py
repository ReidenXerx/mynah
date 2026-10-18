#!/usr/bin/env python3
"""The sleeping pose's new parts, ready for Creator, and a preview of the pose.

    python3 -I assets/bird/tools/sleeping_extras.py assets/bird/source/mynah-sleeping-parts.svg \\
        assets/bird/bird.json assets/bird/import [--check /tmp/sleeping.png]

The sleeping sheet (source/mynah-sleeping-parts.svg) redraws the whole bird
— puffed body, wing, tail, feet — but only two things in it are new to the
rig: the closed eye and the three Z's. The rest of the pose is the idle rig
moved (the owner's call: reuse the existing parts), so this writes just
those four as import files on the 1024 canvas: import/eye-closed.svg over
the open eye, at its size, and import/z-small|medium|big.svg where the
reference has them relative to the eye once the head has sunk.

--check draws the sleeping pose — the idle layers from bird.json with the
POSE transforms below, plus the new parts — the way Creator will, to tune
POSE before any keyframe exists. POSE is a plan, not the source of truth:
after Creator, bird.json is.
"""
import io
import json
import subprocess
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

import numpy as np
from PIL import Image

SVG = "http://www.w3.org/2000/svg"
ET.register_namespace("", SVG)
CANVAS = 1024

# The sheet's top-level elements, in its order.
SHEET = ["sleep-body", "sleep-wing", "sleep-tail", "eye-closed", "sleep-beak",
         "sleep-foot-a", "sleep-foot-b", "z-big", "z-medium", "z-small", "pupil-spare"]
NEW = ["eye-closed", "z-small", "z-medium", "z-big"]

# The Z's in the reference (source/mynah-sleeping.png): box centres relative
# to the closed eye patch's centre, in reference px. They are sized and
# spaced by the body, not the eye: the canvas eye is smaller for its head
# than the reference's, and by the eye the Z's came out small. The sheet's
# body part is drawn at the reference's scale (its Z's measure the same).
REF_Z = {"z-big": (436.5, -286.0), "z-medium": (320.5, -194.5), "z-small": (408.0, -116.0)}

# The sleeping pose, as transforms of the idle rig. The body settles from
# the feet (its bottom edge stays put) and puffs: wider, shorter, so the
# head sinks into it; parts on the body follow the same scale about the
# same point. The beak droops, the tail settles.
SETTLE = (524.0, 751.0)          # the body's bottom centre on the canvas
POSE = {
    "body_scale": (1.06, 0.92),
    "beak_rotation": 6.0,        # degrees, from the idle -3
    "tail_rotation": 26.0,       # from the idle 30: the tip a little lower
}
FOLLOWERS = ["eye", "eye-closed", "eye-far", "beak", "wing-left", "wing-right", "tail"]
# The far eye has no closed drawing: it squeezes shut and fades with the
# near one (the reference shows only a sliver of it, behind the beak).


def render(svg_text, width=None, background=None):
    args = ["rsvg-convert"] + (["-w", str(width)] if width else []) + \
        ([f"--background-color={background}"] if background else [])
    png = subprocess.run(args, input=svg_text.encode(), capture_output=True, check=True).stdout
    return Image.open(io.BytesIO(png)).convert("RGBA")


def box(img):
    ys, xs = np.nonzero(np.asarray(img)[..., 3] > 8)
    return float(xs.min()), float(ys.min()), float(xs.max() + 1), float(ys.max() + 1)


def follow(point):
    """Where a point on the body goes when the body takes the sleeping scale."""
    sx, sy = POSE["body_scale"]
    return (SETTLE[0] + sx * (point[0] - SETTLE[0]), SETTLE[1] + sy * (point[1] - SETTLE[1]))


def main(sheet_path, bird_json, import_dir, check_path=None):
    import_dir = Path(import_dir)
    root = ET.parse(sheet_path).getroot()
    vb = root.get("viewBox")
    elements = dict(zip(SHEET, list(root)))

    def alone(name):
        doc = ET.Element(f"{{{SVG}}}svg", {"viewBox": vb, "width": vb.split()[2], "height": vb.split()[3]})
        doc.append(elements[name])
        return ET.tostring(doc, encoding="unicode")

    sheet_box = {name: box(render(alone(name))) for name in NEW}
    # Scale: the closed eye takes the open eye's size on the canvas.
    open_eye = box(render((import_dir / "eye.svg").read_text()))
    ex0, ey0, ex1, ey1 = sheet_box["eye-closed"]
    k = ((open_eye[2] - open_eye[0]) / (ex1 - ex0) + (open_eye[3] - open_eye[1]) / (ey1 - ey0)) / 2
    eye_centre = ((open_eye[0] + open_eye[2]) / 2, (open_eye[1] + open_eye[3]) / 2)
    sleeping_eye = follow(eye_centre)
    body = box(render((import_dir / "body.svg").read_text()))
    sleep_body = box(render(alone("sleep-body")))
    ref_to_canvas = (body[2] - body[0]) * POSE["body_scale"][0] / (sleep_body[2] - sleep_body[0])

    centres = {"eye-closed": eye_centre}  # over the open eye; it moves with it
    for name, (dx, dy) in REF_Z.items():
        centres[name] = (sleeping_eye[0] + dx * ref_to_canvas, sleeping_eye[1] + dy * ref_to_canvas)

    sheet_box["sleep-body"] = sleep_body
    for name in NEW:
        x0, y0, x1, y1 = sheet_box[name]
        cx, cy = centres[name]
        scale = k if name == "eye-closed" else ref_to_canvas
        tx, ty = cx - scale * (x0 + x1) / 2, cy - scale * (y0 + y1) / 2
        doc = ET.Element(f"{{{SVG}}}svg", {"viewBox": f"0 0 {CANVAS} {CANVAS}", "width": str(CANVAS), "height": str(CANVAS)})
        g = ET.SubElement(doc, f"{{{SVG}}}g", {"id": name, "transform": f"translate({tx:.1f} {ty:.1f}) scale({scale:.4f})"})
        source = elements[name]
        for child in (list(source) if source.tag.endswith("g") else [source]):
            g.append(child)
        ET.ElementTree(doc).write(import_dir / f"{name}.svg", encoding="unicode")
        print(f"{name:10s} centre ({cx:6.1f},{cy:6.1f})  {scale * (x1 - x0):5.1f}×{scale * (y1 - y0):5.1f}")
    print(f"scale {k:.4f} (eye) / {ref_to_canvas:.4f} (Z's) sheet px → canvas px; eye centre idle ({eye_centre[0]:.1f},{eye_centre[1]:.1f}),"
          f" asleep ({sleeping_eye[0]:.1f},{sleeping_eye[1]:.1f})")
    for name in FOLLOWERS:
        print(f"  follows the body: {name}")

    if check_path:
        check(bird_json, import_dir, check_path)


def layer_transform(ks, extra=None):
    """A Lottie layer's static transform as an SVG transform, at rest (idle)."""
    def at_rest(prop):
        return prop["k"][-1]["s"] if prop.get("a") == 1 else prop["k"]
    a, p, s, r = at_rest(ks["a"]), at_rest(ks["p"]), at_rest(ks["s"]), at_rest(ks.get("r", {"k": 0}))
    r = r[0] if isinstance(r, list) else r
    extra = extra or {}
    p = extra.get("position", p)
    s = extra.get("scale", s)
    r = extra.get("rotation", r)
    return (f"translate({p[0]:.2f} {p[1]:.2f}) rotate({r:.2f}) scale({s[0] / 100:.4f} {s[1] / 100:.4f}) "
            f"translate({-a[0]:.2f} {-a[1]:.2f})")


def check(bird_json, import_dir, check_path):
    layers = json.load(open(bird_json))["layers"]
    hidden_when_idle = {"wing-right-raised", "sound-lines"}
    sx, sy = POSE["body_scale"]
    poses = {}
    for pose in ("idle", "asleep"):
        doc = ET.Element(f"{{{SVG}}}svg", {"viewBox": f"0 0 {CANVAS} {CANVAS}", "width": str(CANVAS), "height": str(CANVAS)})
        order = [L["nm"] for L in reversed(layers)]  # back to front
        if pose == "asleep":
            order += ["eye-closed", "z-small", "z-medium", "z-big"]
        by_name = {L["nm"]: L for L in layers}
        for name in order:
            if name in hidden_when_idle:
                continue
            if pose == "idle" and name in NEW:
                continue
            if pose == "asleep" and name in ("eye", "eye-far"):
                continue  # squeezed shut: the near one under the closed eye
            part = ET.parse(import_dir / f"{name}.svg").getroot()
            g = ET.SubElement(doc, f"{{{SVG}}}g")
            if name in by_name:
                ks = by_name[name]["ks"]
                extra = {}
                if pose == "asleep":
                    if name == "body":
                        # About the settle point: anchor there, position there.
                        g.set("transform", f"translate({SETTLE[0]} {SETTLE[1]}) scale({sx} {sy}) "
                                           f"translate({-SETTLE[0]} {-SETTLE[1]})")
                        ks = None
                    else:
                        p = ks["p"]["k"] if ks["p"].get("a", 0) == 0 else ks["p"]["k"][-1]["s"]
                        if name in FOLLOWERS:
                            extra["position"] = follow(p)
                        if name == "beak":
                            extra["rotation"] = POSE["beak_rotation"]
                        if name == "tail":
                            extra["rotation"] = POSE["tail_rotation"]
                if ks is not None:
                    g.set("transform", layer_transform(ks, extra))
            elif name == "eye-closed":
                ex = ET.parse(import_dir / "eye.svg").getroot()
                eb = box(render(ET.tostring(ex, encoding="unicode")))
                centre = ((eb[0] + eb[2]) / 2, (eb[1] + eb[3]) / 2)
                moved = follow(centre)
                g.set("transform", f"translate({moved[0] - centre[0]:.2f} {moved[1] - centre[1]:.2f})")
            for child in list(part):
                g.append(child)
        poses[pose] = render(ET.tostring(doc, encoding="unicode"), 512, "#3b4252")
    out = Image.new("RGBA", (1024, 512))
    out.paste(poses["idle"], (0, 0))
    out.paste(poses["asleep"], (512, 0))
    out.convert("RGB").save(check_path)
    print("check render (idle | asleep):", check_path)


if __name__ == "__main__":
    a = sys.argv[1:]
    check_path = None
    if "--check" in a:
        i = a.index("--check")
        check_path = a[i + 1]
        del a[i:i + 2]
    main(*a, check_path=check_path)
