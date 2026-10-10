#!/usr/bin/env python3
# Renders a scene-mirror dump (ACVR_TEST_MIRROR_DUMP on the macOS bench) as the window lays it
# out (MirrorScene.place, scene_mirror's sky push), seen from a viewer in the room: how the 3D in
# the window is shaped, without the Simulator. render-mirror-dump.py checks the capture from the
# game camera; this checks the layout (scale, depth compression, tilt) from anywhere.
#
#   visionos/scripts/render-window-view.py <dump folder> <out.png> [options]   (numpy, Pillow)
#     --width-m 1.2          the window's width in metres
#     --viewer x,y,z         the eye, metres from the window's centre (+z towards the viewer): 0,0,1.2
#     --look x,y,z           what it looks at (default: the window's centre)
#     --fov 50               the viewer's vertical field of view, degrees
#     --size 640x480
#     --min-view-tangent 0.3 MirrorScene.minViewTangent: narrower views have their depths compressed
#                            (0.3 since 2026-10-09; 0.7 before, TPVR's)
#     --focus steady|camera|<units>   the window's focus (camera.txt has the window's and the camera's)
#     --level <fraction>     turn the world's up that fraction of the way to the window's up, about the
#                            focus point (0: the camera's frame, as the window draws it; 1: level)
#     --no-portal            draw everything, not only what the window's opening shows (side views)
#     --slice <metres>       only triangles within this distance of the window's vertical centre line
#                            (a cross-section, for side views); --no-sky leaves the sky out
# Not modelled: the easing of what's nearer than the glass into the window's front allowance.
import argparse
import glob
import math
import re

import numpy as np
from PIL import Image, ImageDraw

ap = argparse.ArgumentParser()
ap.add_argument("dump")
ap.add_argument("out")
ap.add_argument("--width-m", type=float, default=1.2)
ap.add_argument("--viewer", default="0,0,1.2")
ap.add_argument("--look", default="0,0,0")
ap.add_argument("--fov", type=float, default=50)
ap.add_argument("--size", default="640x480")
ap.add_argument("--min-view-tangent", type=float, default=0.3)
ap.add_argument("--focus", default="steady")
ap.add_argument("--level", type=float, default=0.0)
ap.add_argument("--no-portal", action="store_true")
ap.add_argument("--slice", type=float, default=0.0)
ap.add_argument("--no-sky", action="store_true")
ap.add_argument("--label", default="")
a = ap.parse_args()
W, H = (int(x) for x in a.size.split("x"))
d = a.dump

v = np.fromfile(d + "/vertices.bin", dtype=np.uint8).reshape(-1, 36)
pos = v[:, 0:12].copy().view(np.float32).reshape(-1, 3).astype(np.float64)
uv = v[:, 12:20].copy().view(np.float32).reshape(-1, 2).astype(np.float64)
mul = v[:, 20:28].copy().view(np.float16).reshape(-1, 4).astype(np.float64)
add = v[:, 28:36].copy().view(np.float16).reshape(-1, 4).astype(np.float64)
idx = np.fromfile(d + "/indices.bin", dtype=np.uint32).reshape(-1, 3)
parts = [line.split() for line in open(d + "/parts.txt")]
tex = {}
for f in glob.glob(d + "/tex-*.rgba"):
    i, w, h = map(int, re.search(r"tex-(\d+)-(\d+)x(\d+)", f).groups())
    tex[i] = np.fromfile(f, dtype=np.uint8).reshape(h, w, 4).astype(np.float64) / 255
cam = [float(x) for x in open(d + "/camera.txt").read().split()]
tan_x, tan_y = cam[0], cam[1]
if a.focus == "steady":
    focus = cam[2]
elif a.focus == "camera":
    focus = cam[3] if len(cam) > 3 else cam[2]  # older dumps have one focus: the camera's
else:
    focus = float(a.focus)
up = np.array(cam[4:7]) if len(cam) >= 7 else np.array([0.0, 1.0, 0.0])

# scene_mirror Finish: the sky pushed past the level's farthest point.
background = np.zeros(len(pos), bool)
for p in parts:
    first, count, flags = int(p[0]), int(p[1]), int(p[4])
    if flags & 16:
        background[idx[first // 3:(first + count) // 3].ravel()] = True
if background.any() and (~background).any():
    farthest = (-pos[~background, 2]).max()
    nearest_bg = np.linalg.norm(pos[background], axis=1).min()
    push = min(max(1.25 * farthest / max(nearest_bg, 1e-3), 1.0), 100.0)
    pos[background] *= push

pos0 = pos.copy()

# Optional levelling: about the focus point, the world's up turned towards +y by `level`.
if a.level:
    u = up / np.linalg.norm(up)
    angle = math.atan2(u[2], u[1]) * a.level  # the camera's pitch shows as up leaning towards +z
    c, s = math.cos(-angle), math.sin(-angle)
    R = np.array([[1, 0, 0], [0, c, -s], [0, s, c]])
    pivot = np.array([0.0, 0.0, -focus])
    pos = (pos - pivot) @ R.T + pivot

# scene_mirror's clip: the view's sides with kMargin = 2 (approximated: a triangle goes when all of
# its corners are outside), before any levelling moved them.
kept = (np.abs(pos0[:, 0]) <= 2 * tan_x * -pos0[:, 2]) & (np.abs(pos0[:, 1]) <= 2 * tan_y * -pos0[:, 2]) & (pos0[:, 2] < 0)

# MirrorScene.place: view space to window widths, then metres.
upm = 40.0
plane = min(max(focus * 0.85, 0.5 * upm), 200 * upm)
depth_scale = tan_x / max(tan_x, a.min_view_tangent)
scale = 1 / (2 * plane * tan_x)
win = np.stack([pos[:, 0] * scale, pos[:, 1] * scale, (pos[:, 2] + plane) * scale * depth_scale], -1) * a.width_m
aspect = tan_x / tan_y
win_w, win_h = a.width_m, a.width_m / aspect

# The viewer's camera.
eye = np.array([float(x) for x in a.viewer.split(",")])
look = np.array([float(x) for x in a.look.split(",")])
fwd = look - eye
fwd /= np.linalg.norm(fwd)
right = np.cross(fwd, [0, 1, 0])
right /= np.linalg.norm(right)
vup = np.cross(right, fwd)
rel = win - eye
vx, vy, vz = rel @ right, rel @ vup, rel @ fwd
f = 1 / math.tan(math.radians(a.fov) / 2)
asp = W / H
sx = (vx / np.maximum(vz, 1e-4) * f / asp * 0.5 + 0.5) * W
sy = (0.5 - vy / np.maximum(vz, 1e-4) * f * 0.5) * H
near = 0.02

# The window's opening, per pixel: does the ray from the eye through the pixel cross the glass
# (z = 0) inside it?
X, Y = np.meshgrid(np.arange(W) + 0.5, np.arange(H) + 0.5)
dx = (X / W - 0.5) * 2 * asp / f
dy = (0.5 - Y / H) * 2 / f
rays = dx[..., None] * right + dy[..., None] * vup + fwd
t = -eye[2] / np.where(np.abs(rays[..., 2]) < 1e-9, 1e-9, rays[..., 2])
hit = eye + t[..., None] * rays
opening = (t > 0) & (np.abs(hit[..., 0]) <= win_w / 2) & (np.abs(hit[..., 1]) <= win_h / 2)
if a.no_portal:
    opening[:] = True

img = np.zeros((H, W, 3))
img[:] = (0.78, 0.76, 0.72)  # the room
img[opening] = 0
zb = np.full((H, W), np.inf)
for p in parts:
    first, count, texid, kind, flags = int(p[0]), int(p[1]), int(p[2]), int(p[3]), int(p[4])
    weights = [float(x) for x in p[6:11]]
    T = tex.get(texid)
    if a.no_sky and flags & 16:
        continue
    for i0, i1, i2 in idx[first // 3:(first + count) // 3]:
        if min(vz[i0], vz[i1], vz[i2]) < near:
            continue
        if a.slice and abs(win[i0, 0] + win[i1, 0] + win[i2, 0]) / 3 > a.slice:
            continue
        if not flags & 16 and not kept[i0] and not kept[i1] and not kept[i2]:
            continue  # scene_mirror clips at the view's sides with a margin (kMargin 2)
        ax, ay, bx, by, cx, cy = sx[i0], sy[i0], sx[i1], sy[i1], sx[i2], sy[i2]
        area = (bx - ax) * (cy - ay) - (cx - ax) * (by - ay)
        if area == 0:
            continue
        x0, x1 = int(max(0, np.floor(min(ax, bx, cx)))), int(min(W - 1, np.ceil(max(ax, bx, cx))))
        y0, y1 = int(max(0, np.floor(min(ay, by, cy)))), int(min(H - 1, np.ceil(max(ay, by, cy))))
        if x0 > x1 or y0 > y1:
            continue
        PX, PY = np.meshgrid(np.arange(x0, x1 + 1) + 0.5, np.arange(y0, y1 + 1) + 0.5)
        w0 = ((bx - PX) * (cy - PY) - (cx - PX) * (by - PY)) / area
        w1 = ((cx - PX) * (ay - PY) - (ax - PX) * (cy - PY)) / area
        w2 = 1 - w0 - w1
        inside = (w0 >= 0) & (w1 >= 0) & (w2 >= 0) & opening[y0:y1 + 1, x0:x1 + 1]
        if not inside.any():
            continue
        iz = w0 / vz[i0] + w1 / vz[i1] + w2 / vz[i2]
        depth = 1 / iz
        pw = np.stack([w0 / vz[i0], w1 / vz[i1], w2 / vz[i2]], -1) / iz[..., None]
        uu = pw @ uv[[i0, i1, i2]]
        m = pw @ mul[[i0, i1, i2]]
        ad = pw @ add[[i0, i1, i2]]
        if T is not None:
            th, tw = T.shape[:2]
            smp = T[(np.floor(uu[..., 1] * th).astype(int)) % th, (np.floor(uu[..., 0] * tw).astype(int)) % tw]
        else:
            smp = np.ones(uu.shape[:-1] + (4,))
        col = np.clip(smp[..., :3] * m[..., :3] + ad[..., :3], 0, 1)
        al = np.clip(smp[..., 3] * m[..., 3] + ad[..., 3], 0, 1)
        if kind == 1:
            inside &= al >= float(p[5])
        sub = zb[y0:y1 + 1, x0:x1 + 1]
        ok = inside & (depth < sub)
        if kind == 2:
            cb, ca, ob, oa, ol = weights
            luma = col @ np.array([0.2126, 0.7152, 0.0722])
            op = np.clip(ob + oa * al + ol * luma, 0, 1)[..., None]
            cc = col * (cb + ca * al)[..., None]
            region = img[y0:y1 + 1, x0:x1 + 1]
            region[ok] = (cc + region * (1 - op))[ok]
            if flags & 4:
                sub[ok] = depth[ok]
        else:
            img[y0:y1 + 1, x0:x1 + 1][ok] = col[ok]
            sub[ok] = depth[ok]

out = Image.fromarray((img * 255).astype(np.uint8))
draw = ImageDraw.Draw(out)
# The window's frame.
corners = np.array([[-1, -1, 0], [1, -1, 0], [1, 1, 0], [-1, 1, 0]]) * [win_w / 2, win_h / 2, 0]
rc = corners - eye
cz = rc @ fwd
if (cz > near).all():
    px = (rc @ right / cz * f / asp * 0.5 + 0.5) * W
    py = (0.5 - rc @ vup / cz * f * 0.5) * H
    draw.line(list(zip(px, py)) + [(px[0], py[0])], fill=(250, 250, 250), width=2)
label = a.label or (f"min view tangent {a.min_view_tangent}, depth x{depth_scale:.2f}, focus {focus:.0f}, "
                    f"level {a.level:g}")
draw.rectangle([0, 0, W, 16], fill=(0, 0, 0))
draw.text((4, 2), label, fill=(255, 255, 0))
out.save(a.out)
print(f"{a.out}: depth x{depth_scale:.2f}, focus {focus:.1f}, glass at {plane:.0f} units, "
      f"{scale * a.width_m * 1000:.2f} mm a unit")
