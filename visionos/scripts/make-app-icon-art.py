#!/usr/bin/env python3
"""Draws Animal Crossing VR's app icon layers procedurally (no game art, no logos): an evening
sky over two hills (Back), a round fruit tree on the near hill (Middle) and a bug net held up
to the light (Front). Writes Back.png, Middle.png, Front.png and preview.png into <out dir>;
the plugin's make-app-icon.py packages them into the asset catalog:

    visionos/scripts/make-app-icon-art.py <out dir>
    make-app-icon.py visionos/App/Assets.xcassets --back <out>/Back.png --middle <out>/Middle.png \
        --front <out>/Front.png --preview <out>/preview.png
"""
import math
import os
import sys

from PIL import Image, ImageDraw, ImageFilter

S = 1024
SS = 2  # supersampling for smooth edges


def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(len(a)))


def back():
    w = S * SS
    im = Image.new("RGB", (w, w))
    px = im.load()
    top, mid, low = (78, 150, 214), (150, 200, 236), (255, 214, 170)
    for y in range(w):
        t = y / (w - 1)
        c = lerp(top, mid, t / 0.55) if t < 0.55 else lerp(mid, low, (t - 0.55) / 0.45)
        for x in range(w):
            px[x, y] = c
    d = ImageDraw.Draw(im, "RGBA")
    # a low sun with a soft halo
    glow = Image.new("RGBA", (w, w), (0, 0, 0, 0))
    gd = ImageDraw.Draw(glow)
    cx, cy, r = 0.70 * w, 0.50 * w, 0.11 * w
    gd.ellipse([cx - r * 2.2, cy - r * 2.2, cx + r * 2.2, cy + r * 2.2], fill=(255, 236, 190, 90))
    im.paste(glow.filter(ImageFilter.GaussianBlur(60)), (0, 0), glow.filter(ImageFilter.GaussianBlur(60)))
    d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=(255, 241, 205, 255))
    # far hill, then near hill
    far = [(0, 0.66 * w)]
    for i in range(0, 101):
        x = i / 100 * w
        far.append((x, 0.64 * w - math.sin(i / 100 * math.pi * 1.3 + 0.4) * 0.07 * w))
    far += [(w, w), (0, w)]
    d.polygon(far, fill=(104, 168, 96, 255))
    near = [(0, 0.80 * w)]
    for i in range(0, 101):
        x = i / 100 * w
        near.append((x, 0.78 * w - math.sin(i / 100 * math.pi + 0.2) * 0.10 * w))
    near += [(w, w), (0, w)]
    d.polygon(near, fill=(72, 140, 70, 255))
    # a winding path down the near hill
    path = []
    for i in range(0, 41):
        t = i / 40
        x = 0.5 * w + math.sin(t * 3.2) * 0.08 * w * (1 - t) + (t - 0.5) * 0.10 * w
        y = 0.74 * w + t * 0.30 * w
        path.append((x, y))
    for i in range(len(path) - 1):
        wd = int((0.012 + 0.05 * i / len(path)) * w)
        d.line([path[i], path[i + 1]], fill=(222, 196, 140, 255), width=wd)
    return im.resize((S, S), Image.LANCZOS)


def middle():
    w = S * SS
    im = Image.new("RGBA", (w, w), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    # trunk
    tx, ty = 0.32 * w, 0.70 * w
    d.rounded_rectangle([tx - 0.035 * w, ty - 0.20 * w, tx + 0.035 * w, ty + 0.03 * w], radius=int(0.02 * w),
                        fill=(122, 82, 52, 255))
    # canopy: overlapping rounds
    canopy = (58, 132, 64, 255)
    light = (86, 162, 82, 255)
    for (ox, oy, r) in [(-0.10, -0.30, 0.13), (0.10, -0.30, 0.13), (0.0, -0.40, 0.15), (-0.02, -0.25, 0.14)]:
        cx, cy, rr = tx + ox * w, ty + oy * w, r * w
        d.ellipse([cx - rr, cy - rr, cx + rr, cy + rr], fill=canopy)
    for (ox, oy, r) in [(-0.06, -0.42, 0.07), (0.07, -0.36, 0.05)]:
        cx, cy, rr = tx + ox * w, ty + oy * w, r * w
        d.ellipse([cx - rr, cy - rr, cx + rr, cy + rr], fill=light)
    # fruit
    for (ox, oy) in [(-0.09, -0.30), (0.08, -0.27), (0.02, -0.40), (-0.02, -0.21), (0.12, -0.36)]:
        cx, cy, rr = tx + ox * w, ty + oy * w, 0.026 * w
        d.ellipse([cx - rr, cy - rr, cx + rr, cy + rr], fill=(214, 58, 52, 255))
        d.ellipse([cx - rr * 0.45, cy - rr * 0.6, cx - rr * 0.05, cy - rr * 0.2], fill=(255, 150, 140, 255))
    shadow = Image.new("RGBA", (w, w), (0, 0, 0, 0))
    sd = ImageDraw.Draw(shadow)
    sd.ellipse([tx - 0.16 * w, ty + 0.0 * w, tx + 0.16 * w, ty + 0.06 * w], fill=(20, 60, 20, 110))
    shadow = shadow.filter(ImageFilter.GaussianBlur(14))
    im = Image.alpha_composite(shadow, im)
    return im.resize((S, S), Image.LANCZOS)


def front():
    w = S * SS
    im = Image.new("RGBA", (w, w), (0, 0, 0, 0))
    # a bug net swung up: an oval hoop seen at an angle, its mesh bag trailing behind, the handle
    # down to the lower right
    cx, cy, rx, ry = 0.62 * w, 0.34 * w, 0.15 * w, 0.085 * w
    tilt = -0.35
    def hoop_point(a):
        x, y = math.cos(a) * rx, math.sin(a) * ry
        return (cx + x * math.cos(tilt) - y * math.sin(tilt), cy + x * math.sin(tilt) + y * math.cos(tilt))
    hoop = [hoop_point(i / 72 * 2 * math.pi) for i in range(73)]
    # the bag: from the hoop's back half down to a rounded tip below and behind it
    tip = (cx - 0.06 * w, cy + 0.30 * w)
    bag_outline = []
    for i in range(0, 37):
        bag_outline.append(hoop_point(math.pi * 0.05 + i / 36 * math.pi * 0.9))
    left, right = bag_outline[-1], bag_outline[0]
    curve = []
    for i in range(1, 30):
        t = i / 30
        # left side down to the tip, then back up the right side
        if t < 0.5:
            u = t / 0.5
            p0, p2 = left, tip
            p1 = (left[0] - 0.05 * w, left[1] + 0.16 * w)
        else:
            u = (t - 0.5) / 0.5
            p0, p2 = tip, right
            p1 = (right[0] + 0.02 * w, right[1] + 0.20 * w)
        curve.append(((1 - u) ** 2 * p0[0] + 2 * (1 - u) * u * p1[0] + u * u * p2[0],
                      (1 - u) ** 2 * p0[1] + 2 * (1 - u) * u * p1[1] + u * u * p2[1]))
    bag_poly = [right] + bag_outline[::-1][:0] + [left] + curve
    bag = Image.new("RGBA", (w, w), (0, 0, 0, 0))
    bd = ImageDraw.Draw(bag)
    bd.polygon(hoop + curve, fill=(246, 249, 255, 150))
    step = int(0.026 * w)
    for k in range(-w, w, step):
        bd.line([(k, 0), (k + w * 0.6, w)], fill=(255, 255, 255, 170), width=5)
        bd.line([(k + w * 0.6, 0), (k, w)], fill=(255, 255, 255, 170), width=5)
    mask = Image.new("L", (w, w), 0)
    md = ImageDraw.Draw(mask)
    md.polygon(hoop, fill=255)
    md.polygon([left] + curve + [right], fill=255)
    clipped = Image.new("RGBA", (w, w), (0, 0, 0, 0))
    clipped.paste(bag, (0, 0), mask)
    im = Image.alpha_composite(im, clipped)
    d = ImageDraw.Draw(im)
    # handle from the hoop's lower right rim to the corner
    hx1, hy1 = hoop_point(0.35)
    hx0, hy0 = 0.95 * w, 0.97 * w
    d.line([(hx0, hy0), (hx1, hy1)], fill=(150, 104, 62, 255), width=int(0.034 * w))
    d.line([(hx0 - 6, hy0 - 8), (hx1 - 6, hy1 - 8)], fill=(190, 142, 92, 255), width=int(0.010 * w))
    rim = int(0.022 * w)
    d.line(hoop, fill=(232, 174, 52, 255), width=rim, joint="curve")
    d.line(hoop[40:60], fill=(255, 222, 135, 255), width=int(rim * 0.4), joint="curve")
    shadow = Image.new("RGBA", (w, w), (0, 0, 0, 0))
    shadow.paste((10, 30, 20, 80), (16, 24), im.split()[3])
    shadow = shadow.filter(ImageFilter.GaussianBlur(18))
    im = Image.alpha_composite(shadow, im)
    return im.resize((S, S), Image.LANCZOS)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "."
    os.makedirs(out, exist_ok=True)
    b, m, f = back(), middle(), front()
    b.save(os.path.join(out, "Back.png"))
    m.save(os.path.join(out, "Middle.png"))
    f.save(os.path.join(out, "Front.png"))
    preview = b.convert("RGBA")
    preview = Image.alpha_composite(preview, m)
    preview = Image.alpha_composite(preview, f)
    mask = Image.new("L", (S, S), 0)
    ImageDraw.Draw(mask).ellipse([0, 0, S - 1, S - 1], fill=255)
    round_preview = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    round_preview.paste(preview, (0, 0), mask)
    round_preview.save(os.path.join(out, "preview.png"))
    print(out)


if __name__ == "__main__":
    main()
