"""Tool icons in the style of the shape icons: 32 x 32, orange outline (1.6) with a 35 % orange fill,
cut-outs truly transparent. Writes appdata/icons/<tool>.svg (the toolbar prefers these over PNGs).

    python tools/make_tool_icons.py            # writes the icons
    python tools/make_tool_icons.py --sheet    # also writes tools/tool_icons_preview.html
"""
import math
import os
import sys

INK = "#e87400"
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "appdata", "icons")


def body(d, opacity=0.35):
    """A solid: outline + translucent fill. Sub-paths inside it are holes (evenodd)."""
    return (f'<path d="{d}" fill="{INK}" fill-opacity="{opacity}" fill-rule="evenodd" stroke="{INK}" '
            f'stroke-width="1.6" stroke-linejoin="round"/>')


def line(d, width=1.6, dash=None):
    dash_attr = f' stroke-dasharray="{dash}"' if dash else ""
    return (f'<path d="{d}" fill="none" stroke="{INK}" stroke-width="{width}" stroke-linecap="round" '
            f'stroke-linejoin="round"{dash_attr}/>')


def solid(d):
    """Fully orange (arrow heads, small marks)."""
    return f'<path d="{d}" fill="{INK}" stroke="{INK}" stroke-width="0.8" stroke-linejoin="round"/>'


def rect(x, y, w, h, r=0):
    if r <= 0:
        return f"M{x} {y}h{w}v{h}h{-w}Z"
    return (f"M{x + r} {y}h{w - 2 * r}a{r} {r} 0 0 1 {r} {r}v{h - 2 * r}a{r} {r} 0 0 1 {-r} {r}"
            f"h{-(w - 2 * r)}a{r} {r} 0 0 1 {-r} {-r}v{-(h - 2 * r)}a{r} {r} 0 0 1 {r} {-r}Z")


def circle(cx, cy, r):
    return f"M{cx - r} {cy}a{r} {r} 0 1 0 {2 * r} 0a{r} {r} 0 1 0 {-2 * r} 0Z"


def poly(points):
    return "M" + "L".join(f"{x:.2f} {y:.2f}" for x, y in points) + "Z"


def head(tip, direction_deg, size=4.2):
    """A solid arrow head with its tip at `tip`, pointing along direction_deg (0 = right, 90 = down)."""
    a = math.radians(direction_deg)
    dx, dy = math.cos(a), math.sin(a)
    bx, by = tip[0] - dx * size, tip[1] - dy * size
    px, py = -dy * size * 0.62, dx * size * 0.62
    return solid(poly([tip, (bx + px, by + py), (bx - px, by - py)]))


def arc(cx, cy, r, a0, a1):
    """Arc path from angle a0 to a1 (degrees, y down, increasing clockwise on screen)."""
    p0 = (cx + r * math.cos(math.radians(a0)), cy + r * math.sin(math.radians(a0)))
    p1 = (cx + r * math.cos(math.radians(a1)), cy + r * math.sin(math.radians(a1)))
    large = 1 if (a1 - a0) % 360 > 180 else 0
    return f"M{p0[0]:.2f} {p0[1]:.2f}A{r} {r} 0 {large} 1 {p1[0]:.2f} {p1[1]:.2f}", p1


def svg(*parts):
    return ('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 32 32" width="32" height="32">\n  '
            + "\n  ".join(parts) + "\n</svg>\n")


def beta_path(x, y, height):
    """A small β from DejaVu Sans Bold (outline + counters), as one evenodd path."""
    from matplotlib.textpath import TextPath
    from matplotlib.font_manager import FontProperties
    tp = TextPath((0, 0), "β", size=100, prop=FontProperties(family="DejaVu Sans", weight="bold"))
    v = tp.vertices
    x0, x1, y0, y1 = v[:, 0].min(), v[:, 0].max(), v[:, 1].min(), v[:, 1].max()
    s = height / (y1 - y0)
    tx = lambda p: (x + (p[0] - x0) * s, y + (y1 - p[1]) * s)
    d, i, codes = [], 0, tp.codes
    while i < len(codes):
        c = codes[i]
        if c == 1:
            d.append("M%.2f %.2f" % tx(v[i])); i += 1
        elif c == 2:
            d.append("L%.2f %.2f" % tx(v[i])); i += 1
        elif c == 3:
            d.append("Q%.2f %.2f %.2f %.2f" % (*tx(v[i]), *tx(v[i + 1]))); i += 2
        elif c == 4:
            d.append("C%.2f %.2f %.2f %.2f %.2f %.2f" % (*tx(v[i]), *tx(v[i + 1]), *tx(v[i + 2]))); i += 3
        else:
            d.append("Z"); i += 1
    return f'<path d="{"".join(d)}" fill="{INK}" fill-rule="nonzero"/>'


ICONS = {}

# --- transform ------------------------------------------------------------------------------------
ICONS["move"] = svg(body(poly([
    (16, 2.5), (21.5, 8), (18, 8), (18, 14), (24, 14), (24, 10.5), (29.5, 16), (24, 21.5), (24, 18), (18, 18),
    (18, 24), (21.5, 24), (16, 29.5), (10.5, 24), (14, 24), (14, 18), (8, 18), (8, 21.5), (2.5, 16), (8, 10.5),
    (8, 14), (14, 14), (14, 8), (10.5, 8)])))

def _rotate():
    # Clockwise arc around a block; the head continues the arc at its end (tangent = angle + 90).
    start, end = -60, 200
    d, p1 = arc(16, 16, 11.5, start, end)
    t = math.radians(end + 90)
    tip = (p1[0] + 3.6 * math.cos(t), p1[1] + 3.6 * math.sin(t))
    return svg(body(rect(11, 11, 10, 10, 2)), line(d), head(tip, end + 90 + 8, 5))


ICONS["rotate"] = _rotate()

ICONS["mirror"] = svg(
    line("M16 3V29", 1.4, "2.4 2.2"),
    body(poly([(13, 7), (13, 25), (3.5, 25)])),
    body(poly([(19, 7), (19, 25), (28.5, 25)]), 0.12))

ICONS["align"] = svg(
    line("M5 3.5V28.5"),
    body(rect(7, 6, 19, 5, 1.2)),
    body(rect(7, 13.5, 12, 5, 1.2)),
    body(rect(7, 21, 16, 5, 1.2)))


# A block turned down onto the plate, along the curved arrow.
ICONS["lay_flat"] = svg(
    line("M3 28.5H29", 1.6),
    body(rect(7, 18, 18, 9, 1.2)),
    solid(rect(7, 24.5, 18, 2.5)),
    line(arc(16, 18, 10, 200, 330)[0], 1.4),
    head((24.7, 13), 60, 3.6))

# A slanted plane with a part standing upright on it.
ICONS["workplane"] = svg(
    body(poly([(3, 22), (17, 29), (29, 19), (15, 12)]), 0.25),
    line("M9 19.5L21 25.5M12 16.5L24 22.5", 1.0, "1.6 1.4"),
    body(poly([(14, 19), (19, 21.5), (19, 11.5), (14, 9)])),
    head((16.5, 3.5), -90, 3.6), line("M16.5 9.5V6.5", 1.4))

# A block cut open: the near half gone (dashed), the cut face hatched.
ICONS["section"] = svg(
    line(rect(17, 6, 11, 20, 1.2), 1.2, "2 1.6"),
    body(rect(4, 6, 13, 20, 1.2)),
    line("M7 26L17 16M7 19L17 9M10 26L17 19M4.5 14L12.5 6", 1.0),
    line("M17 3V29", 1.8))

def _ruler():
    a = math.radians(-35)
    c, s = math.cos(a), math.sin(a)
    rot = lambda x, y: (16 + x * c - y * s, 16 + x * s + y * c)
    outline = poly([rot(-13, -4.5), rot(13, -4.5), rot(13, 4.5), rot(-13, 4.5)])
    ticks = "".join("M%.2f %.2fL%.2f %.2f" % (*rot(x, -4.5), *rot(x, -4.5 + depth))
                    for x, depth in [(-8, 4.5), (-4, 2.6), (0, 4.5), (4, 2.6), (8, 4.5)])
    return svg(body(outline), line(ticks, 1.3))


ICONS["measure"] = _ruler()

# --- shape the solid ------------------------------------------------------------------------------
ICONS["bend"] = svg(body(
    "M3.5 22A13 13 0 0 1 28.5 22L23 22A7.6 7.6 0 0 0 9 22Z"))

ICONS["bend_face"] = svg(
    body(rect(3.5, 17, 13, 11.5, 1.5)),
    body("M3.5 17L3.5 13A10 10 0 0 1 13.5 3L28.5 3L28.5 8.5L13.5 8.5A4.6 4.6 0 0 0 8.9 13L8.9 17Z", 0.18))

ICONS["extrude_face"] = svg(
    body(rect(4, 16, 16, 12.5, 1.5)),
    line("M4 16V6.5H20V16", 1.4, "2.2 2"),
    line("M26 26.5V9"), head((26, 4.5), -90))

ICONS["fillet"] = svg(body("M4 28.5V4H14A14.5 14.5 0 0 1 28.5 18.5V28.5Z"))

ICONS["fillet_surface"] = svg(
    body("M3.5 28.5V13A9 9 0 0 1 12.5 4H19.5A9 9 0 0 1 28.5 13V28.5Z"),
    body(circle(16, 17, 3.2), 0.8))

ICONS["edge_fillet_beta"] = svg(
    body("M4 28.5V4H14A14.5 14.5 0 0 1 28.5 18.5V28.5Z"),
    beta_path(8.5, 11, 14))

# --- combine --------------------------------------------------------------------------------------
ICONS["hole"] = svg(body(rect(4, 4, 24, 24, 3) + circle(16, 16, 6.5)))

ICONS["group"] = svg(
    line(rect(2.5, 4.5, 27, 23, 2), 1.2, "2.2 2"),
    body(rect(6, 12, 12, 12, 1.5)),
    body(circle(19.5, 13.5, 6.2)))

def _hull():
    """Two circles shrink-wrapped: the hull as a solid, the circles inside as thin outlines."""
    pts = []
    for cx, cy, r in ((9, 22.5, 5), (21.5, 10.5, 7.5)):
        pts += [(cx + r * math.cos(2 * math.pi * k / 96), cy + r * math.sin(2 * math.pi * k / 96)) for k in range(96)]
    pts.sort()
    def cross(o, a, b):
        return (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0])
    lower, upper = [], []
    for p in pts:
        while len(lower) >= 2 and cross(lower[-2], lower[-1], p) <= 0:
            lower.pop()
        lower.append(p)
    for p in reversed(pts):
        while len(upper) >= 2 and cross(upper[-2], upper[-1], p) <= 0:
            upper.pop()
        upper.append(p)
    return svg(body(poly(lower[:-1] + upper[:-1]), 0.3),
               line(circle(9, 22.5, 5), 1.0, "1.6 1.4"), line(circle(21.5, 10.5, 7.5), 1.0, "1.6 1.4"))


ICONS["hull"] = _hull()

ICONS["intersect"] = svg(
    line(circle(12, 16, 9), 1.0, "1.6 1.4"), line(circle(20, 16, 9), 1.0, "1.6 1.4"),
    body("M16 7.94A9 9 0 0 1 16 24.06A9 9 0 0 1 16 7.94Z", 0.45))

ICONS["hollow"] = svg(body("M4 6H9.5V22.5H22.5V6H28V28H4Z"))

ICONS["split"] = svg(
    body(rect(3, 8, 10.5, 17, 1.5)), body(rect(18.5, 8, 10.5, 17, 1.5)),
    line("M16 3.5V28.5", 1.2, "2.2 1.8"))

ICONS["overhangs"] = svg(
    body("M4 28V6H28V13H13V28Z"),
    line("M15 20.5L20.5 15M19.5 20.5L25 15M24 20.5L27.5 17", 1.1))

ICONS["ungroup"] = svg(
    body(rect(3.5, 15, 12, 12, 1.5)),
    body(circle(22.5, 9.5, 6.2)),
    line("M17 16.5L20 13.5", 1.4), line("M13 11.5L10 8.5", 1.4, None), line("M22 20L25 23", 1.4))

ICONS["repeat"] = svg(
    body(rect(2.5, 11, 8, 10, 1.4)),
    body(rect(12, 11, 8, 10, 1.4), 0.25),
    body(rect(21.5, 11, 8, 10, 1.4), 0.15))

# --- edit -----------------------------------------------------------------------------------------
ICONS["copy"] = svg(
    body(rect(11, 3.5, 17.5, 19, 2), 0.15),
    body(rect(3.5, 9.5, 17.5, 19, 2)))

ICONS["delete"] = svg(
    line("M5 7.5H27M12.5 7.5V4.5H19.5V7.5"),
    body("M7.5 9.5H24.5L23 28.5H9Z"),
    line("M13 13V25M19 13V25", 1.3))

ICONS["drop"] = svg(
    body(rect(10.5, 2.5, 11, 8, 1.4)),
    line("M16 12.5V20.5"), head((16, 24.2), 90),
    body(rect(3, 25.5, 26, 4, 1.2)))

ICONS["drop_onto_part"] = svg(
    body(rect(11.5, 1.5, 9, 6.5, 1.2)),
    line("M16 9.5V14"), head((16, 17.5), 90),
    body(rect(8, 19, 16, 7, 1.3)),
    body(rect(3, 27, 26, 3, 1)))

ICONS["2d_shape"] = svg(
    body(poly([(6, 25), (11, 6), (26, 15), (20, 26)])),
    body(circle(6, 25, 2.4), 1), body(circle(11, 6, 2.4), 1),
    body(circle(26, 15, 2.4), 1), body(circle(20, 26, 2.4), 1))

# --- bottom bar -----------------------------------------------------------------------------------
ICONS["snap"] = svg(
    body("M6 4H12V17A4 4 0 0 0 20 17V4H26V17A10 10 0 0 1 6 17Z"),
    solid(rect(6, 4, 6, 4)), solid(rect(20, 4, 6, 4)),
    line("M16 27.5V29.5M11 26L10 28M21 26L22 28", 1.3))

ICONS["grid"] = svg(
    body(rect(4, 4, 24, 24, 2), 0.15),
    line("M12 4.8V27.2M20 4.8V27.2M4.8 12H27.2M4.8 20H27.2", 1.2))


def main():
    os.makedirs(OUT, exist_ok=True)
    for name, text in ICONS.items():
        with open(os.path.join(OUT, name + ".svg"), "w") as f:
            f.write(text)
    print(f"wrote {len(ICONS)} icons to {os.path.normpath(OUT)}")
    if "--sheet" in sys.argv:
        cells = "".join(
            f'<div class="c"><div class="big">{ICONS[n]}</div><div class="slot">{ICONS[n]}</div><span>{n}</span></div>'
            for n in ICONS)
        page = ("<!doctype html><html><body style='margin:0;background:#12151a;color:#99a5b3;font:11px sans-serif'>"
                "<style>.g{display:flex;flex-wrap:wrap;gap:14px;padding:16px;width:980px}.c{width:96px;text-align:center}"
                ".big svg{width:72px;height:72px}.slot{width:32px;height:32px;margin:6px auto;background:#1b2028}"
                ".slot svg{width:28px;height:28px;margin:2px}span{display:block}</style>"
                f"<div class='g'>{cells}</div></body></html>")
        with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "tool_icons_preview.html"), "w") as f:
            f.write(page)


if __name__ == "__main__":
    main()
