#!/usr/bin/env python3
"""
gen_assets.py - Pre-renders the three backdrops (sky, sun / moon, mountains)
for the RasterFireRainSnow example and writes them to ../assets.h as RGB565
arrays in flash.

Each backdrop is the sky region of the landscape frame, in two sizes: 320
x 128 for a 320 x 240 frame, 480 x 171 for 480 x 320.  Stored COLUMN-major
(index x * HZ + y) because the sketch renders one landscape column per panel
row (see scene.cpp), so a column is one contiguous copy.

Also writes a per-column skyline (top row of the mountains) for each scene,
and preview PNGs next to this script (preview_*.png) for checking the art.

    python3 tools/gen_assets.py

Needs numpy and Pillow.  Deterministic (fixed seeds).
"""

import os
import numpy as np
from PIL import Image, ImageDraw, ImageFont

W, HZ = 320, 128          # set per size by set_size()
SX = SY = 1.0             # scale against the 320 x 128 original
HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "assets.h")


# ------------------------------------------------------------------ noise

def value_noise_1d(x, seed):
    rng = np.random.default_rng(seed)
    lat = rng.random(4096)
    xi = np.floor(x).astype(int)
    f = x - xi
    f = f * f * (3 - 2 * f)
    a = lat[xi % 4096]
    b = lat[(xi + 1) % 4096]
    return a + (b - a) * f


def fbm_1d(x, seed, octaves=6, lac=2.0, gain=0.5, ridged=False):
    tot, amp, norm = 0.0, 1.0, 0.0
    for o in range(octaves):
        n = value_noise_1d(x * lac ** o, seed + o * 17)
        if ridged:
            n = 1.0 - abs(n * 2 - 1)
        tot = tot + n * amp
        norm += amp
        amp *= gain
    return tot / norm


def value_noise_2d(x, y, seed):
    rng = np.random.default_rng(seed)
    lat = rng.random((257, 257))
    xi = np.floor(x).astype(int)
    yi = np.floor(y).astype(int)
    fx = x - xi
    fy = y - yi
    fx = fx * fx * (3 - 2 * fx)
    fy = fy * fy * (3 - 2 * fy)
    x0, x1 = xi % 256, (xi + 1) % 256
    y0, y1 = yi % 256, (yi + 1) % 256
    a = lat[y0, x0] + (lat[y0, x1] - lat[y0, x0]) * fx
    b = lat[y1, x0] + (lat[y1, x1] - lat[y1, x0]) * fx
    return a + (b - a) * fy


def fbm_2d(x, y, seed, octaves=5, lac=2.0, gain=0.5):
    tot, amp, norm = 0.0, 1.0, 0.0
    for o in range(octaves):
        tot = tot + value_noise_2d(x * lac ** o, y * lac ** o, seed + o * 31) * amp
        norm += amp
        amp *= gain
    return tot / norm


def smoothstep(e0, e1, x):
    t = np.clip((x - e0) / (e1 - e0), 0, 1)
    return t * t * (3 - 2 * t)


def lerp(a, b, t):
    return a + (b - a) * t


YY = XX = None


def set_size(w, hz):
    global W, HZ, SX, SY, YY, XX
    W, HZ = w, hz
    SX, SY = w / 320.0, hz / 128.0
    YY, XX = np.mgrid[0:HZ, 0:W].astype(float)


def vgrad(stops):
    """Vertical gradient: stops = [(y, (r,g,b)), ...]; returns HZ x W x 3."""
    ys = np.array([s[0] if s[0] == "end" else s[0] for s in stops], float)
    cs = np.array([s[1] for s in stops], float)
    col = np.stack([np.interp(np.arange(HZ), ys, cs[:, k]) for k in range(3)], -1)
    return np.repeat(col[:, None, :], W, axis=1)


# ------------------------------------------------------------------ mountains

STORM_CLOUD_ALPHA = None
SUB = 4                     # silhouette samples per pixel, for anti-aliasing


def ridge(seed, base, amp, scale, octaves=6, ridged=True):
    """Top row of a mountain range (y grows downward): per column, and at SUB
    points across each column (W x SUB) for the anti-aliased edge."""
    x = (np.arange(W * SUB, dtype=float) + 0.5) / SUB - 0.5
    h = fbm_1d(x / scale, seed, octaves, ridged=ridged)
    h = (h - h.min()) / (h.max() - h.min())
    fine = (HZ - (base + amp * h)).reshape(W, SUB)
    return fine.mean(axis=1), fine


def mountain_layer(top, seed, light_from_left=True):
    """Mask and a 0..1 light term for a range whose top row is `top`.

    The light is a fake: the slope of the silhouette, measured at a scale that
    grows with depth below the crest, so faces break up into facets that run
    down from the peaks; plus vertical gullies from stretched noise."""
    inside = YY >= top[None, :]
    depth = np.clip(YY - top[None, :], 0, None)
    light = np.zeros((HZ, W))
    xs = np.arange(W)
    for y in range(HZ):
        d = depth[y]
        s = np.clip(1 + d * 0.6, 1, 24 * SX).astype(int)
        l = top[np.clip(xs - s, 0, W - 1)]
        r = top[np.clip(xs + s, 0, W - 1)]
        slope = (r - l) / (2 * s)                       # +: falls away to the right
        light[y] = (0.5 - 1.6 * slope) if light_from_left else (0.5 + 1.6 * slope)
    gully = fbm_2d(XX / (7.0 * SX), YY / (30.0 * SY), seed + 5, 4)
    light = light + (gully - 0.5) * 0.7
    return inside, np.clip(light, 0.05, 1.0), depth


# ------------------------------------------------------------------ scenes

def scene_fire():
    """Night: deep blue sky, stars, a crescent moon, two dark ranges."""
    ranges = []
    sky = vgrad([(0, (3, 5, 16)), (70 * SY, (10, 14, 38)), (HZ - 1, (34, 30, 56))])
    rng = np.random.default_rng(7)
    img = sky.copy()
    # Stars: more of them, and brighter, higher up.
    for _ in range(int(260 * SX * SY)):
        x, y = rng.integers(0, W), int(rng.random() ** 1.6 * (HZ - 20 * SY))
        b = rng.random() ** 2.2
        tint = np.array([1.0, 0.95 + 0.05 * rng.random(), 0.85 + 0.25 * rng.random()])
        img[y, x] += 40 + 215 * b * tint
        if b > 0.75:                                    # a few with a soft halo
            for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                if 0 <= x + dx < W and 0 <= y + dy < HZ:
                    img[y + dy, x + dx] += 60 * b * tint
    # Crescent moon with a halo.
    mx, my, mr = 262.0 * SX, 30.0 * SY, 10.0 * SY
    d = np.hypot(XX - mx, YY - my)
    img += (np.exp(-np.clip(d - mr, 0, None) / (14.0 * SY)) * 38)[..., None] * np.array([0.8, 0.85, 1.0])
    disc = np.clip(mr + 0.5 - d, 0, 1)
    shadow = np.clip(mr + 0.5 - np.hypot(XX - mx - 5.5 * SY, YY - my + 2.5 * SY), 0, 1)
    lit = disc * (1 - shadow)
    img = lerp(img, np.array([235, 232, 210]), lit[..., None])
    # Far range: dim, lit faintly by the moon from the right.
    far, far_f = ridge(11, 22 * SY, 34 * SY, 70 * SX)
    inside, light, depth = mountain_layer(far, 11, light_from_left=False)
    col = lerp(np.array([16, 18, 36]), np.array([40, 44, 72]), light[..., None])
    ranges.append((far_f, col))
    near, near_f = ridge(23, 6 * SY, 24 * SY, 45 * SX)
    inside, light, depth = mountain_layer(near, 23, light_from_left=False)
    col = lerp(np.array([6, 6, 12]), np.array([20, 20, 34]), light[..., None])
    ranges.append((near_f, col))
    return img, ranges


def scene_storm():
    """Thunderstorm at sunset: a low sun behind breaking storm clouds."""
    ranges = []
    img = vgrad([(0, (34, 26, 48)), (40 * SY, (82, 44, 70)), (80 * SY, (178, 72, 58)),
                 (110 * SY, (236, 128, 56)), (HZ - 1, (252, 170, 80))])
    # The sun itself (its disc and inner glow) is drawn live, setting through
    # the clouds; only the glow over the point where it sets is baked in.
    # (The sketch's sun_step() sets it at 0.62 of the width.)
    sx, sy = 0.62 * W, HZ - 6 * SY
    d = np.hypot((XX - sx) * 0.7, (YY - sy) * 1.2)
    glow = np.exp(-d / (70.0 * SY)) * 75
    img += glow[..., None] * np.array([1.0, 0.62, 0.28])
    # Storm clouds: heavy at the top, breaking up toward the horizon.
    n = fbm_2d(XX / (58.0 * SX) + 3, YY / (16.0 * SY), 41, 6)
    thresh = 0.33 + 0.20 * smoothstep(30 * SY, 108 * SY, YY)
    dens = smoothstep(thresh, thresh + 0.16, n)
    below = fbm_2d(XX / (58.0 * SX) + 3, (YY + 4 * SY) / (16.0 * SY), 41, 6)
    under = np.clip((n - below) * 6, 0, 1)                  # lower edges: lit from below
    sunprox = np.exp(-np.hypot(XX - sx, YY - sy) / (90.0 * SY))
    cloud = lerp(np.array([30, 26, 40]), np.array([62, 48, 66]), (n - 0.4)[..., None].clip(0, 1))
    cloud = cloud + (under * (0.35 + 1.4 * sunprox))[..., None] * np.array([230, 110, 50])
    img = lerp(img, cloud, (dens * 0.94)[..., None])
    global STORM_CLOUD_ALPHA
    STORM_CLOUD_ALPHA = dens * 0.94
    # Ranges: hazy far one, dark near one, both backlit.
    far, far_f = ridge(52, 14 * SY, 30 * SY, 64 * SX)
    inside, light, depth = mountain_layer(far, 52, light_from_left=False)
    col = lerp(np.array([92, 44, 66]), np.array([140, 70, 80]), light[..., None])
    col = lerp(col, np.array([200, 110, 80]), smoothstep(HZ - 30 * SY, HZ, YY)[..., None] * 0.5)
    rim = np.exp(-depth / (1.5 * SY)) * np.exp(-np.abs(XX - sx) / (60.0 * SX))
    col = col + (rim * 120)[..., None] * np.array([1.0, 0.6, 0.3])
    ranges.append((far_f, col))
    near, near_f = ridge(64, 4 * SY, 20 * SY, 40 * SX)
    inside, light, depth = mountain_layer(near, 64, light_from_left=False)
    col = lerp(np.array([28, 16, 34]), np.array([52, 30, 50]), light[..., None])
    rim = np.exp(-depth / (1.2 * SY)) * np.exp(-np.abs(XX - sx) / (50.0 * SX))
    col = col + (rim * 140)[..., None] * np.array([1.0, 0.55, 0.25])
    ranges.append((near_f, col))
    return img, ranges


def scene_snow():
    """Overcast winter: snow-capped peaks under a pale sky."""
    ranges = []
    img = vgrad([(0, (120, 132, 152)), (70 * SY, (168, 178, 196)), (HZ - 1, (206, 212, 224))])
    n = fbm_2d(XX / (70.0 * SX), YY / (22.0 * SY), 77, 5)
    img = img + ((n - 0.5) * 10)[..., None]     # faint: the drifting clouds are drawn live
    # Far range: tall, pale, hazy.
    far, far_f = ridge(81, 30 * SY, 50 * SY, 80 * SX)
    inside, light, depth = mountain_layer(far, 81, light_from_left=True)
    h = HZ - YY
    snowline = (34 + fbm_1d(np.arange(W) / (12.0 * SX), 83, 4)[None, :] * 14) * SY
    gully = fbm_2d(XX / (5.0 * SX), YY / (24.0 * SY), 85, 4)
    snow = smoothstep(-3 * SY, 3 * SY, h - snowline + (gully - 0.5) * 22 * SY) * smoothstep(0.15, 0.35, light + 0.1)
    rock = lerp(np.array([92, 102, 128]), np.array([140, 150, 172]), light[..., None])
    sn = lerp(np.array([176, 188, 214]), np.array([238, 242, 250]), light[..., None])
    col = lerp(rock, sn, snow[..., None])
    col = lerp(col, np.array([196, 204, 220]), 0.35 + 0.3 * smoothstep(HZ - 40 * SY, HZ, YY)[..., None])
    ranges.append((far_f, col))
    # Near range: darker rock, crisp snow.
    near, near_f = ridge(91, 8 * SY, 40 * SY, 52 * SX)
    inside, light, depth = mountain_layer(near, 91, light_from_left=True)
    snowline = (22 + fbm_1d(np.arange(W) / (10.0 * SX), 93, 4)[None, :] * 14) * SY
    gully = fbm_2d(XX / (4.0 * SX), YY / (18.0 * SY), 95, 4)
    snow = smoothstep(-2 * SY, 2 * SY, h - snowline + (gully - 0.5) * 26 * SY) * smoothstep(0.12, 0.3, light + 0.05)
    rock = lerp(np.array([48, 54, 70]), np.array([92, 98, 116]), light[..., None])
    sn = lerp(np.array([160, 174, 204]), np.array([246, 248, 252]), light[..., None])
    col = lerp(rock, sn, snow[..., None])
    col = lerp(col, np.array([200, 206, 220]), 0.12 + 0.35 * smoothstep(HZ - 22 * SY, HZ, YY)[..., None])
    ranges.append((near_f, col))
    return img, ranges


def compose_mountains(ranges):
    """The ranges, far to near, as one RGBA layer: colour (H x W x 3) and
    alpha 0..1, the edge anti-aliased from the SUB samples per column.
    Colour is bled upward into the transparent part so that bilinear
    sampling at the edge never picks up black."""
    A = np.zeros((HZ, W))
    C = np.zeros((HZ, W, 3))
    for fine, col in ranges:
        cov = np.clip(YY[..., None] + 1 - fine[None, :, :], 0, 1).mean(axis=2)
        a_new = cov + A * (1 - cov)
        safe = np.where(a_new > 0, a_new, 1)
        C = (col * cov[..., None] + C * (A * (1 - cov))[..., None]) / safe[..., None]
        A = a_new
    for x in range(W):
        ys = np.nonzero(A[:, x] > 0.02)[0]
        if len(ys):
            C[:ys[0], x] = C[ys[0], x]
    return C, A


def clouds(seed, cover):
    """A strip of drifting clouds for the snow scene, 2W wide and tileable:
    luminance and alpha, 0..255 each."""
    CW, CH = 2 * W, int(HZ * 0.62) // 2 * 2
    B = W // 4                                     # blended seam
    yy, xx = np.mgrid[0:CH, 0:CW + B].astype(float)
    n = fbm_2d(xx / (46.0 * SX), yy / (13.0 * SY), seed, 6)
    up = fbm_2d(xx / (46.0 * SX), (yy - 3 * SY) / (13.0 * SY), seed, 6)
    t = np.linspace(0, 1, B)[None, :]
    n[:, :B] = n[:, :B] * t + n[:, CW:] * (1 - t)
    up[:, :B] = up[:, :B] * t + up[:, CW:] * (1 - t)
    n, up = n[:, :CW], up[:, :CW]
    band = smoothstep(0, 10 * SY, yy[:, :CW]) * (1 - smoothstep(CH * 0.55, CH, yy[:, :CW]))
    dens = smoothstep(cover, cover + 0.17, n) * band
    lit = np.clip((n - up) * 7 + 0.55, 0, 1)        # tops lit from above, bellies grey
    lum = 150 + 100 * lit
    return lum, dens * 0.92


# ------------------------------------------------------------------ output

def to565(img):
    """Floyd-Steinberg dither to RGB565; returns (H x W uint16, preview RGB)."""
    a = np.clip(img, 0, 255).astype(float).copy()
    out = np.zeros(a.shape[:2], np.uint16)
    prev = np.zeros(a.shape, np.uint8)
    levels = np.array([31, 63, 31], float)
    for y in range(a.shape[0]):
        for x in range(a.shape[1]):
            old = a[y, x]
            q = np.clip(np.round(old / 255 * levels), 0, levels)
            new = q / levels * 255
            err = old - new
            prev[y, x] = new.astype(np.uint8)
            out[y, x] = (int(q[0]) << 11) | (int(q[1]) << 5) | int(q[2])
            if x + 1 < a.shape[1]:
                a[y, x + 1] += err * 7 / 16
            if y + 1 < a.shape[0]:
                if x > 0:
                    a[y + 1, x - 1] += err * 3 / 16
                a[y + 1, x] += err * 5 / 16
                if x + 1 < a.shape[1]:
                    a[y + 1, x + 1] += err * 1 / 16
    return out, prev


def c_array(name, arr, per_line=16, ctype="uint16_t", fmt="0x{:04X}"):
    flat = arr.flatten()
    lines = []
    for i in range(0, len(flat), per_line):
        lines.append("    " + ",".join(fmt.format(int(v)) for v in flat[i:i + per_line]) + ",")
    return f"static const {ctype} {name}[{len(flat)}] = {{\n" + "\n".join(lines) + "\n};\n"


SIZES = [("", 320, 128),        # 320 x 240 frame (2.8" and 3.2" panels)
         ("_L", 480, 171)]     # 480 x 320 frame (3.5" and 4.0" panels)
LAYER_SCALE = {}                 # a scene whose mountains are rendered larger than the picture (none)
CLOUD_SETS = [(201, 0.50), (307, 0.56), (409, 0.46)]


def main():
    scenes = [("FIRE", scene_fire), ("STORM", scene_storm), ("SNOW", scene_snow)]
    parts = ["// Generated by tools/gen_assets.py -- do not edit.\n"
             "// Per scene, in two sizes (320 x HZ 128 for a 320 x 240 frame, and, suffixed\n"
             "// _L, 480 x HZ 171 for 480 x 320):\n"
             "//   SKY_*     the sky without mountains, RGB565, column-major [x * HZ + y]\n"
             "//   MCOL_*    the mountains' colour, rows MTOP .. HZ-1, column-major [x * MH + row]\n"
             "//   MALPHA_*  their coverage, 0..255, same layout (the edge anti-aliased)\n"
             "//   MFIRST_*  per column, the first row (of the layer) with any coverage\n"
             "//   MSOLID_*  per column, the first row from which every row is opaque\n"
             "// CLOUDA_STORM*: the storm clouds' cover, 0..255, sky layout (the sun sets behind it);\n"
             "// and CLOUDn: tileable strips for the snow scene, (lum << 8 | alpha),\n"
             "// column-major [x * CLOUD_H + y], CLOUD_W wide, at half resolution.\n"
             "#pragma once\n#include <stdint.h>\n\n"
             "// The mountain layer is lw wide with its horizon at row lhz, rendered at\n"
             "// `scale` times the picture's size (the snow's, at its largest zoom).\n"
             "// sky is null where the sketch draws the sky itself.\n"
             "struct SceneAsset {\n    const uint16_t *sky, *mcol;\n    const uint8_t *malpha;\n"
             "    const uint16_t *mfirst, *msolid;\n    int mtop, mh, lw, lhz;\n    float scale;\n};\n\n"]
    for suffix, w, hz in SIZES:
        set_size(w, hz)
        parts.append(f"#define ASSET_W{suffix}  {w}\n#define ASSET_HZ{suffix} {hz}\n\n")
        for name, fn in scenes:
            # The snow's mountains grow to LAYER_SCALE: rendered that large and
            # shrunk at first, rather than enlarged from a small render.
            scale = LAYER_SCALE.get(name, 1.0)
            set_size(int(round(w * scale / 2)) * 2, int(round(hz * scale)))
            sky, ranges = fn()
            C, A = compose_mountains(ranges)
            rows = np.nonzero(A.max(axis=1) > 0.002)[0]
            mtop = max(0, int(rows[0]) - 1)
            mh = HZ - mtop
            skypx, skyprev = to565(sky)
            mpx, mprev = to565(np.where(A[..., None] > 0, C, sky))
            a8 = np.clip(np.round(A * 255), 0, 255).astype(np.uint8)
            prev = (skyprev * (1 - A[..., None]) + mprev * A[..., None]).astype(np.uint8)
            Image.fromarray(prev).save(os.path.join(HERE, f"preview_{name.lower()}{suffix.lower()}.png"))
            first = np.zeros(W, np.uint16)
            solid = np.zeros(W, np.uint16)
            for x in range(W):
                col = a8[mtop:, x]
                nz = np.nonzero(col)[0]
                first[x] = nz[0] if len(nz) else mh - 1
                notfull = np.nonzero(col < 255)[0]
                solid[x] = min(mh - 1, notfull[-1] + 1) if len(notfull) else 0
            sfx = f"{name}{suffix}"
            lw, lhz = W, HZ
            set_size(w, hz)
            has_sky = name == "STORM"            # fire and snow skies are drawn live
            if has_sky:
                parts.append(c_array(f"SKY_{sfx}", skypx.T))
            parts.append(c_array(f"MCOL_{sfx}", mpx[mtop:].T))
            parts.append(c_array(f"MALPHA_{sfx}", a8[mtop:].T, 32, "uint8_t", "{}"))
            parts.append(c_array(f"MFIRST_{sfx}", first, 32, "uint16_t", "{}"))
            parts.append(c_array(f"MSOLID_{sfx}", solid, 32, "uint16_t", "{}"))
            if name == "STORM":
                parts.append(c_array(f"CLOUDA_{sfx}", np.clip(np.round(STORM_CLOUD_ALPHA * 255), 0, 255)
                                     .astype(np.uint8).T, 32, "uint8_t", "{}"))
            parts.append(f"static const SceneAsset ASSET_{sfx} = {{ {'SKY_' + sfx if has_sky else 'nullptr'}, "
                         f"MCOL_{sfx}, MALPHA_{sfx}, MFIRST_{sfx}, MSOLID_{sfx}, {mtop}, {mh}, {lw}, {lhz}, "
                         f"{scale}f }};\n\n")
        # The fire's stars: a catalog over the sky and up and to the left of it
        # (they wheel down to the right over a scene).  x, y in the picture
        # at the start; brightness; twinkle phase.
        rng = np.random.default_rng(17)
        n = min(640, int(260 * SX * SY * 1.6))
        st = np.zeros((n, 4), np.int32)
        st[:, 0] = rng.uniform(-0.35 * W, W, n)
        st[:, 1] = -0.35 * HZ + rng.random(n) ** 1.3 * 1.35 * HZ
        st[:, 2] = 40 + 215 * rng.random(n) ** 2.2
        st[:, 3] = rng.integers(0, 256, n)
        parts.append(f"#define NSTARS{suffix} {n}\n")
        parts.append(c_array(f"STARS{suffix}", st, 16, "int16_t", "{}"))
        ch = int(HZ * 0.62) // 2 * 2
        parts.append(f"#define CLOUD_W{suffix} {W}\n#define CLOUD_H{suffix} {ch // 2}   // half resolution\n")
        for k, (seed, cover) in enumerate(CLOUD_SETS):
            lum, a = clouds(seed, cover)
            # Half resolution both ways (the clouds are soft): a quarter of
            # the flash to read each frame.
            lum = lum.reshape(lum.shape[0] // 2, 2, lum.shape[1] // 2, 2).mean(axis=(1, 3))
            a = a.reshape(a.shape[0] // 2, 2, a.shape[1] // 2, 2).mean(axis=(1, 3))
            v = (np.clip(lum, 0, 255).astype(np.uint16) << 8) | np.clip(np.round(a * 255), 0, 255).astype(np.uint16)
            parts.append(c_array(f"CLOUD{k}{suffix}", v.T))
            pv = np.dstack([lum * a + 160 * (1 - a)] * 3).clip(0, 255).astype(np.uint8)
            Image.fromarray(pv).save(os.path.join(HERE, f"preview_cloud{k}{suffix.lower()}.png"))
        parts.append(f"static const uint16_t *const CLOUDS{suffix}[{len(CLOUD_SETS)}] = "
                     f"{{ {', '.join(f'CLOUD{k}{suffix}' for k in range(len(CLOUD_SETS)))} }};\n\n")
    # The notice shown when the board has no speed-test record: three lines,
    # 1 bit a pixel, rows of NOTICE_W bits packed MSB first.
    lines = ["Performance data not saved to board NVS.", "Run stress test sketch first.",
             "Demo in degraded mode."]
    font = ImageFont.load_default(size=14)
    nw = max(font.getbbox(t)[2] for t in lines) + 4
    lh = 20
    img = Image.new("1", (nw, lh * len(lines)), 0)
    d = ImageDraw.Draw(img)
    for k, t in enumerate(lines):
        tw = font.getbbox(t)[2]
        d.text(((nw - tw) // 2, k * lh + 2), t, fill=1, font=font)
    bits = np.array(img, dtype=np.uint8)
    stride = (nw + 7) // 8
    packed = np.packbits(bits, axis=1)[:, :stride]
    parts.append(f"#define NOTICE_W {nw}\n#define NOTICE_H {img.height}\n")
    parts.append(c_array("NOTICE", packed, 32, "uint8_t", "0x{:02X}"))
    img.convert("L").save(os.path.join(HERE, "preview_notice.png"))
    with open(OUT, "w") as f:
        f.write("".join(parts))
    print("wrote", os.path.normpath(OUT))


if __name__ == "__main__":
    main()
