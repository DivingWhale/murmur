"""Rasterize the prototype's stroke icons into anti-aliased LVGL alpha bitmaps.

The paths are copied from prototype/index.html (24-unit view boxes unless
noted). Each icon is drawn at the pixel sizes the firmware uses, as an
LV_IMG_CF_ALPHA_8BIT image that LVGL tints with `img_recolor`.

    python3 firmware/tools/gen_icons.py      # rewrites firmware/src/icons_data.cpp
"""
import math
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# name: (view box, stroke width in view-box units, [shapes]), shapes are SVG path
# strings or ("circle", cx, cy, r) / ("ellipse", cx, cy, rx, ry, degrees) / ("rect", x, y, w, h, rx).
ICONS = {
    "check": (24, 2.6, ["M5 12.5l4.5 4.5L19 7.5"]),
    "tick": (24, 3.5, ["M5 12.5l4.5 4.5L19 7.5"]),           # inside a filled task checkbox
    "sync": (24, 2.4, ["M20 11a8 8 0 0 0-14.3-4.9L4 8", "M4 3v5h5", "M4 13a8 8 0 0 0 14.3 4.9L20 16", "M20 21v-5h-5"]),
    "wifi": (24, 2.2, ["M5 12.6a10 10 0 0 1 14 0", "M1.5 9a15 15 0 0 1 21 0", "M8.5 16.4a5 5 0 0 1 7 0", "M12 20h.01"]),
    "wifi_off": (24, 2.2, ["M2 2l20 20", "M8.5 16.5a5 5 0 0 1 7 0", "M5 12.9a10 10 0 0 1 5.2-2.8", "M19 12.9a10 10 0 0 0-2-1.4",
                           "M2 8.8a15 15 0 0 1 4.2-2.7", "M22 8.8A15 15 0 0 0 11 5", "M12 20h.01"]),
    "sun": (24, 2.2, [("circle", 12, 12, 4), "M12 2v2M12 20v2M4.9 4.9l1.4 1.4M17.7 17.7l1.4 1.4M2 12h2M20 12h2M4.9 19.1l1.4-1.4M17.7 6.3l1.4-1.4"]),
    "moon": (24, 2, ["M20 14.5A8 8 0 1 1 9.5 4a6.5 6.5 0 0 0 10.5 10.5z"]),
    "back": (24, 2, ["M9 14 4 9l5-5", "M4 9h10.5a5.5 5.5 0 0 1 0 11H11"]),
    "lock": (24, 2, [("rect", 4, 11, 16, 10, 2), "M8 11V7a4 4 0 0 1 8 0v4"]),
    "atom": (24, 1.6, [("circle", 12, 12, 2), ("ellipse", 12, 12, 10, 4, 0), ("ellipse", 12, 12, 10, 4, 60), ("ellipse", 12, 12, 10, 4, 120)]),
    "warn": (24, 1.8, ["M10.3 3.9L1.8 18a2 2 0 0 0 1.7 3h17a2 2 0 0 0 1.7-3L13.7 3.9a2 2 0 0 0-3.4 0z", "M12 9v4M12 17h.01"]),
    "saved": (56, 2.5, [("circle", 28, 28, 25), "M17 29l7.5 7.5L39 21"]),
}
# Pixel sizes used by the UI (see icons.h).
SIZES = {
    "check": [11], "tick": [12], "sync": [11, 19], "wifi": [12, 15], "wifi_off": [12, 15], "sun": [20],
    "moon": [32], "back": [32], "lock": [32], "atom": [40], "warn": [44], "saved": [56],
}


def arc_points(x1, y1, rx, ry, phi, large, sweep, x2, y2):
    """SVG elliptical arc to points (endpoint parameterization, W3C F.6.5)."""
    if rx == 0 or ry == 0:
        return [(x2, y2)]
    cp, sp = math.cos(math.radians(phi)), math.sin(math.radians(phi))
    dx, dy = (x1 - x2) / 2, (y1 - y2) / 2
    x1p, y1p = cp * dx + sp * dy, -sp * dx + cp * dy
    lam = x1p ** 2 / rx ** 2 + y1p ** 2 / ry ** 2
    if lam > 1:
        rx, ry = rx * math.sqrt(lam), ry * math.sqrt(lam)
    num = rx ** 2 * ry ** 2 - rx ** 2 * y1p ** 2 - ry ** 2 * x1p ** 2
    den = rx ** 2 * y1p ** 2 + ry ** 2 * x1p ** 2
    co = math.sqrt(max(0, num / den)) * (-1 if large == sweep else 1)
    cxp, cyp = co * rx * y1p / ry, -co * ry * x1p / rx
    cx, cy = cp * cxp - sp * cyp + (x1 + x2) / 2, sp * cxp + cp * cyp + (y1 + y2) / 2

    def ang(ux, uy, vx, vy):
        return math.atan2(ux * vy - uy * vx, ux * vx + uy * vy)
    t1 = ang(1, 0, (x1p - cxp) / rx, (y1p - cyp) / ry)
    dt = ang((x1p - cxp) / rx, (y1p - cyp) / ry, (-x1p - cxp) / rx, (-y1p - cyp) / ry)
    if not sweep and dt > 0:
        dt -= 2 * math.pi
    elif sweep and dt < 0:
        dt += 2 * math.pi
    n = max(4, int(abs(dt) * max(rx, ry)))
    out = []
    for i in range(1, n + 1):
        t = t1 + dt * i / n
        x, y = rx * math.cos(t), ry * math.sin(t)
        out.append((cp * x - sp * y + cx, sp * x + cp * y + cy))
    return out


def path_polylines(d):
    tokens = re.findall(r"[MmLlHhVvAaZz]|-?(?:\d+\.?\d*|\.\d+)", d)
    lines, cur, x, y, sx, sy, cmd, i = [], [], 0.0, 0.0, 0.0, 0.0, None, 0

    def num():
        nonlocal i
        i += 1
        return float(tokens[i - 1])
    while i < len(tokens):
        if re.match(r"[A-Za-z]", tokens[i]):
            cmd = tokens[i]
            i += 1
            if cmd in "Zz":
                cur.append((sx, sy))
                x, y = sx, sy
                continue
        rel = cmd.islower()
        c = cmd.upper()
        if c == "M":
            nx, ny = num(), num()
            x, y = (x + nx, y + ny) if rel else (nx, ny)
            if cur:
                lines.append(cur)
            cur, sx, sy = [(x, y)], x, y
            cmd = "l" if rel else "L"
        elif c == "L":
            nx, ny = num(), num()
            x, y = (x + nx, y + ny) if rel else (nx, ny)
            cur.append((x, y))
        elif c == "H":
            nx = num()
            x = x + nx if rel else nx
            cur.append((x, y))
        elif c == "V":
            ny = num()
            y = y + ny if rel else ny
            cur.append((x, y))
        elif c == "A":
            rx, ry, phi, large, sweep, nx, ny = (num() for _ in range(7))
            ex, ey = (x + nx, y + ny) if rel else (nx, ny)
            cur += arc_points(x, y, rx, ry, phi, int(large), int(sweep), ex, ey)
            x, y = ex, ey
    if cur:
        lines.append(cur)
    return lines


def shape_polylines(shape):
    if isinstance(shape, str):
        return path_polylines(shape)
    kind = shape[0]
    if kind == "circle":
        _, cx, cy, r = shape
        shape = ("ellipse", cx, cy, r, r, 0)
        kind = "ellipse"
    if kind == "ellipse":
        _, cx, cy, rx, ry, deg = shape
        a = math.radians(deg)
        n = int(8 * max(rx, ry)) + 8
        pts = []
        for k in range(n + 1):
            t = 2 * math.pi * k / n
            x, y = rx * math.cos(t), ry * math.sin(t)
            pts.append((cx + x * math.cos(a) - y * math.sin(a), cy + x * math.sin(a) + y * math.cos(a)))
        return [pts]
    if kind == "rect":
        _, x, y, w, h, r = shape
        d = (f"M{x + r} {y}H{x + w - r}A{r} {r} 0 0 1 {x + w} {y + r}V{y + h - r}A{r} {r} 0 0 1 {x + w - r} {y + h}"
             f"H{x + r}A{r} {r} 0 0 1 {x} {y + h - r}V{y + r}A{r} {r} 0 0 1 {x + r} {y}z")
        return path_polylines(d)
    raise ValueError(kind)


def seg_dist(px, py, a, b):
    (ax, ay), (bx, by) = a, b
    vx, vy = bx - ax, by - ay
    l2 = vx * vx + vy * vy
    t = 0 if l2 == 0 else max(0, min(1, ((px - ax) * vx + (py - ay) * vy) / l2))
    return math.hypot(px - ax - t * vx, py - ay - t * vy)


def rasterize(name, size):
    box, stroke, shapes = ICONS[name]
    scale = size / box
    segs = []
    for shape in shapes:
        for line in shape_polylines(shape):
            pts = [(x * scale, y * scale) for x, y in line]
            if len(pts) == 1:
                pts.append(pts[0])
            segs += list(zip(pts, pts[1:]))
    half = stroke * scale / 2
    out = bytearray()
    for y in range(size):
        for x in range(size):
            # 4x4 supersampling of the round-capped stroke coverage.
            hits = 0
            for sy in range(4):
                for sx in range(4):
                    px, py = x + (sx + .5) / 4, y + (sy + .5) / 4
                    if any(seg_dist(px, py, a, b) <= half for a, b in segs):
                        hits += 1
            out.append(round(255 * hits / 16))
    return bytes(out)


def main():
    lines = ["// Generated by firmware/tools/gen_icons.py from the prototype's SVG icons. Do not edit.",
             '#include "icons.h"', "", "namespace {"]
    table = []
    for name, sizes in SIZES.items():
        for size in sizes:
            data = rasterize(name, size)
            ident = f"{name}_{size}"
            body = ",".join(str(b) for b in data)
            lines.append(f"const uint8_t {ident}_px[] = {{{body}}};")
            lines.append(f"const lv_img_dsc_t {ident} = {{{{LV_IMG_CF_ALPHA_8BIT, 0, 0, {size}, {size}}}, {len(data)}, {ident}_px}};")
            table.append(f"    {{Icon::{name.title().replace('_', '')}, {size}, &{ident}}},")
    lines += ["}", "", "const IconImage iconImages[] = {"] + table + ["};",
              "const unsigned iconImageCount = sizeof(iconImages) / sizeof(*iconImages);", ""]
    (ROOT / "src/icons_data.cpp").write_text("\n".join(lines))
    print(f"{len(table)} icon images")


if __name__ == "__main__":
    main()
