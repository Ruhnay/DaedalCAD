# Writes tool icons in the format DaedalCAD's 2D editor saves (editable there):
# one orange path (evenodd) + data-daedalcad-contours records + sampled polygons.
import math
from matplotlib.textpath import TextPath
from matplotlib.font_manager import FontProperties

INK = "#e87400"

def node(x, y, ix=0.0, iy=0.0, ox=0.0, oy=0.0):
    return [(x, y), (ix, iy), (ox, oy)]

def poly(points):
    return [node(x, y) for x, y in points]

def rounded_square(lo, hi, r):
    """Square lo..hi with the top-right corner rounded by r (editor coords, y up)."""
    k = 0.5523 * r
    return [node(lo, lo), node(hi, lo), node(hi, hi - r, 0, 0, 0, k), node(hi - r, hi, k, 0, 0, 0), node(lo, hi)]

def rotate(contour, deg):
    a = math.radians(deg)
    c, s = math.cos(a), math.sin(a)
    rot = lambda x, y: (x * c - y * s, x * s + y * c)
    return [[rot(*p), rot(*i), rot(*o)] for p, i, o in contour]

def glyph(char, height, centre, family="DejaVu Sans", weight="bold"):
    """A font character as editor contours (cubic handles), scaled to `height`, centred at `centre`."""
    tp = TextPath((0, 0), char, size=100, prop=FontProperties(family=family, weight=weight))
    verts, codes = tp.vertices, tp.codes
    subpaths, cur, i = [], None, 0
    while i < len(codes):
        c = codes[i]
        if c == 1:  # MOVETO
            cur = [[tuple(verts[i]), (0, 0), (0, 0)]]
            subpaths.append(cur)
            i += 1
        elif c == 2:  # LINETO
            cur.append([tuple(verts[i]), (0, 0), (0, 0)])
            i += 1
        elif c == 3:  # quadratic: control, end
            p0 = cur[-1][0]
            q, p1 = verts[i], verts[i + 1]
            c1 = (p0[0] + 2 / 3 * (q[0] - p0[0]), p0[1] + 2 / 3 * (q[1] - p0[1]))
            c2 = (p1[0] + 2 / 3 * (q[0] - p1[0]), p1[1] + 2 / 3 * (q[1] - p1[1]))
            cur[-1][2] = (c1[0] - p0[0], c1[1] - p0[1])
            cur.append([tuple(p1), (c2[0] - p1[0], c2[1] - p1[1]), (0, 0)])
            i += 2
        elif c == 4:  # cubic: c1, c2, end
            p0 = cur[-1][0]
            c1, c2, p1 = verts[i], verts[i + 1], verts[i + 2]
            cur[-1][2] = (c1[0] - p0[0], c1[1] - p0[1])
            cur.append([tuple(p1), (c2[0] - p1[0], c2[1] - p1[1]), (0, 0)])
            i += 3
        else:  # CLOSEPOLY / STOP
            i += 1
    out = []
    for sp in subpaths:
        if len(sp) > 1 and math.dist(sp[0][0], sp[-1][0]) < 1e-6:
            sp[0][1] = sp[-1][1]
            sp.pop()
        if len(sp) >= 3:
            out.append(sp)
    xs = [p[0][0] for sp in out for p in sp]
    ys = [p[0][1] for sp in out for p in sp]
    scale = height / (max(ys) - min(ys))
    cx, cy = (max(xs) + min(xs)) / 2, (max(ys) + min(ys)) / 2
    return [[[((p[0] - cx) * scale + centre[0], (p[1] - cy) * scale + centre[1]),
              (h[0] * scale, h[1] * scale), (o[0] * scale, o[1] * scale)] for p, h, o in sp] for sp in out]

def sample(contour, per=24):
    pts = []
    n = len(contour)
    for i in range(n):
        p0, _, out = contour[i]
        p3, inn, _ = contour[(i + 1) % n]
        if out == (0, 0) and inn == (0, 0):
            pts.append(p0)
            continue
        c1 = (p0[0] + out[0], p0[1] + out[1])
        c2 = (p3[0] + inn[0], p3[1] + inn[1])
        for k in range(per):
            t = k / per
            mt = 1 - t
            pts.append((mt**3 * p0[0] + 3 * mt * mt * t * c1[0] + 3 * mt * t * t * c2[0] + t**3 * p3[0],
                        mt**3 * p0[1] + 3 * mt * mt * t * c1[1] + 3 * mt * t * t * c2[1] + t**3 * p3[1]))
    return pts

def f(v):
    return f"{v:.4f}"

def write(path, contours, pad=2.0):
    """contours: list of (is_hole, contour). Coordinates are editor coords (y up)."""
    sampled = [sample(item[1]) for item in contours]
    xs = [p[0] for s in sampled for p in s]
    ys = [-p[1] for s in sampled for p in s]
    x0, x1, y0, y1 = min(xs) - pad, max(xs) + pad, min(ys) - pad, max(ys) + pad
    d, records = [], []
    for item in contours:
        hole, c = item[0], item[1]
        layer = item[2] if len(item) > 2 else 1
        n = len(c)
        seg = [f"M {f(c[0][0][0])} {f(-c[0][0][1])}"]
        for i in range(n):
            p0, _, out = c[i]
            p3, inn, _ = c[(i + 1) % n]
            if out == (0, 0) and inn == (0, 0):
                seg.append(f"L {f(p3[0])} {f(-p3[1])}")
            else:
                seg.append(f"C {f(p0[0] + out[0])} {f(-(p0[1] + out[1]))} {f(p3[0] + inn[0])} {f(-(p3[1] + inn[1]))} {f(p3[0])} {f(-p3[1])}")
        d.append(" ".join(seg) + " Z")
        records.append(("h" if hole else "s") + (f"{layer}c{layer}" if layer > 1 else "") + ":" + ";".join(
            ",".join(f(v) for v in (p[0], p[1], i[0], i[1], o[0], o[1])) for p, i, o in c))
    lines = ['<?xml version="1.0" encoding="UTF-8"?>',
             f'<svg xmlns="http://www.w3.org/2000/svg" version="1.1" viewBox="{f(x0)} {f(y0)} {f(x1 - x0)} {f(y1 - y0)}">',
             f'  <path d="{"".join(d)}" data-daedalcad-contours="{"|".join(records)}" data-daedalcad-color="{INK}" '
             f'fill="{INK}" fill-rule="evenodd" stroke="{INK}" stroke-width="1"/>']
    for item, pts in zip(contours, sampled):
        hole = item[0]
        p = " ".join(f"{f(x)},{f(-y)}" for x, y in pts + [pts[0]])
        lines.append(f'  <polygon points="{p}"{" data-hole=\"1\"" if hole else ""} fill="none" stroke="none"/>')
    lines.append("</svg>")
    open(path, "w").write("\n".join(lines) + "\n")

import os
out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "appdata", "icons") + os.sep

# Edge fillet (Beta): the fillet block as a frame, with a beta inside.
frame = rounded_square(-20, 20, 13)
inner = rounded_square(-15, 15, 8)
beta = glyph("β", 23, (-0.5, -1.0))
def area(c):
    pts = sample(c)
    return abs(sum(pts[i][0] * pts[(i + 1) % len(pts)][1] - pts[(i + 1) % len(pts)][0] * pts[i][1] for i in range(len(pts)))) / 2
beta.sort(key=area, reverse=True)  # the outline first, then its counters
# The beta on layer 2, so the frame's hole does not cut it away in the 2D editor (or in 3D).
write(out + "edge_fillet_beta.svg", [(False, frame), (True, inner)] + [(False, beta[0], 2)] + [(True, c, 2) for c in beta[1:]])

# Drop onto part: block, arrow, landing on a part that stands on the plate.
write(out + "drop_onto_part.svg", [
    (False, poly([(-30, -30), (30, -30), (30, -24), (-30, -24)])),
    (False, poly([(-16, -20), (16, -20), (16, -2), (-16, -2)])),
    (False, poly([(-4, 26), (4, 26), (4, 11), (10, 11), (0, 2), (-10, 11), (-4, 11)])),
    (False, poly([(-10, 31), (10, 31), (10, 49), (-10, 49)])),
])

# Measure: a ruler with tick notches, tilted.
ruler = [(-25, -7), (25, -7), (25, 7)]
for x, depth in [(16, 7), (8, 4), (0, 7), (-8, 4), (-16, 7)]:
    ruler += [(x + 1.3, 7), (x + 1.3, 7 - depth), (x - 1.3, 7 - depth), (x - 1.3, 7)]
ruler += [(-25, 7)]
write(out + "measure.svg", [(False, rotate(poly(ruler), 35))])
print("written")
