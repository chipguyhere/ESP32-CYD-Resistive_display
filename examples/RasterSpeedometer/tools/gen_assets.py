#!/usr/bin/env python3
"""
gen_assets.py - Pre-renders the raster speedometer's still parts for the
RasterSpeedometer example and writes them to ../assets.h, in flash.

For each picture size (320 x 240 and 480 x 320):
  DIAL    the dial without its numbers: chrome bezel, smoked-glass face,
          LED track, tick marks, labels, RASTER on odometer wheels, the
          hub's shadow.  Premultiplied
          RGB565 plus a transmission (how much of the starfield behind shows
          through, 0..32), as column runs (only the pixels inside the bezel).
  LIGHT   the pixels the speed can light: the LED arc and its glow, and the
          "pie" sector of the face inside it.  Per pixel, the angle along
          the dial (0..1023) and a role/intensity (1..39 arc, 40..63 pie),
          as column runs.  The sketch decides each frame what is lit.
  NUMS    the scale numbers 0..200, drawn 1.4x larger than they appear at
          rest (they grow when the speed reaches them): 4-bit core + 4-bit
          glow, column-major.
  HUB     the needle's chrome hub, premultiplied RGB565 + alpha.
  BIG     digits 0-9 of the digital read-out, 4-bit core + 4-bit glow.
  SMALL   digits 0-9 for the km/h line, 4-bit alpha.

Everything is stored COLUMN-major, because the sketch draws the landscape
picture one column (= one panel row) at a time.

    python3 tools/gen_assets.py

Needs numpy and Pillow, and the Source Code Pro fonts (SIL Open Font
License; adobe-fonts/source-code-pro) -- set SCP_DIR if they aren't in
/Library/Fonts.  Writes preview_*.png next to this script.
"""

import os
import numpy as np
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "assets.h")
SCP_DIR = os.environ.get("SCP_DIR", "/Library/Fonts")
F_BOLD = os.path.join(SCP_DIR, "SourceCodePro-Bold.ttf")
F_BLACK = os.path.join(SCP_DIR, "SourceCodePro-Black.ttf")
F_SEMI = os.path.join(SCP_DIR, "SourceCodePro-Semibold.ttf")

SS = 4                      # supersampling
SWEEP0, SWEEP = -135.0, 270.0
VMAX = 200
REDLINE = 160
NUM_MAX = 1.4               # numbers are stored at this scale
BAYER = np.array([[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]], np.float64)

# Layout, in units of the dial radius R from the dial's centre (y down).
NUM_R = 0.545               # the scale numbers' centres
NUM_H = 0.080               # their digit height at rest
BIG_CY = 0.44               # the read-out's centre
BIG_HT = 0.18               # its digit height
MPH_CY = 0.615
KMH_CY = 0.745
SMALL_HT = 0.075
TICK_R1 = 0.775
# "RASTER" on odometer wheels, above the hub.
ODO_CY = -0.30              # the slot's centre
ODO_HT = 0.064              # letter (cap) height
ODO_WH = 0.062              # half the window's height
ODO_TH = 58.0               # how far round the drum the window shows, each way (deg)
ODO_PITCH = 1.5             # letters round the drum, in letter heights apart
ODO_ROLL = [0, 0, 0, 0, 0, 0]  # each wheel's turn off true, in pitches (all aligned)


def ang(v):
    return SWEEP0 + SWEEP * v / VMAX


def smoothstep(e0, e1, x):
    t = np.clip((x - e0) / (e1 - e0), 0, 1)
    return t * t * (3 - 2 * t)


def down(a):
    """Box-filter a supersampled array (H*SS, W*SS[, c]) to pixels."""
    h, w = a.shape[0] // SS, a.shape[1] // SS
    if a.ndim == 3:
        return a.reshape(h, SS, w, SS, a.shape[2]).mean(axis=(1, 3))
    return a.reshape(h, SS, w, SS).mean(axis=(1, 3))


def blur(a, sigma):
    r = max(1, int(3 * sigma))
    k = np.exp(-0.5 * (np.arange(-r, r + 1) / sigma) ** 2)
    k /= k.sum()
    p = np.pad(a, r)
    t = np.apply_along_axis(lambda v: np.convolve(v, k, "valid"), 0, p)
    return np.apply_along_axis(lambda v: np.convolve(v, k, "valid"), 1, t)


def to565(rgb, x0=0, y0=0):
    """Ordered-dithered RGB565 of an (h, w, 3) 0..255 image."""
    h, w = rgb.shape[:2]
    d = BAYER[(np.arange(h)[:, None] + y0) % 4, (np.arange(w)[None, :] + x0) % 4] / 16.0 - 0.5
    r = np.clip(np.floor(rgb[..., 0] / 8 + 0.5 + d), 0, 31).astype(np.uint16)
    g = np.clip(np.floor(rgb[..., 1] / 4 + 0.5 + d), 0, 63).astype(np.uint16)
    b = np.clip(np.floor(rgb[..., 2] / 8 + 0.5 + d), 0, 31).astype(np.uint16)
    return (r << 11) | (g << 5) | b


def c_array(name, arr, per_line=16, ctype="uint16_t", fmt="0x{:04X}"):
    flat = np.asarray(arr).ravel()
    lines = []
    for i in range(0, len(flat), per_line):
        lines.append("    " + ", ".join(fmt.format(int(v)) for v in flat[i:i + per_line]) + ",")
    return f"static const {ctype} {name}[{len(flat)}] = {{\n" + "\n".join(lines) + "\n};\n\n"


def undot(a):
    """Source Code Pro's zero carries a dot, to tell it from O; a car's
    speedometer doesn't.  Removes every blob of ink whose box lies inside
    another blob's box -- the dot inside a 0's ring.  a: a supersampled
    alpha array; returns a copy."""
    m = a > 0
    h, w = m.shape
    lab = np.zeros((h, w), np.int32)
    boxes = []
    for y0, x0 in zip(*np.nonzero(m)):
        if lab[y0, x0]:
            continue
        k = len(boxes) + 1
        lab[y0, x0] = k
        stack = [(y0, x0)]
        b = [y0, y0, x0, x0]
        while stack:
            y, x = stack.pop()
            b[0] = min(b[0], y); b[1] = max(b[1], y); b[2] = min(b[2], x); b[3] = max(b[3], x)
            for yy in (y - 1, y, y + 1):
                for xx in (x - 1, x, x + 1):
                    if 0 <= yy < h and 0 <= xx < w and m[yy, xx] and not lab[yy, xx]:
                        lab[yy, xx] = k
                        stack.append((yy, xx))
        boxes.append(b)
    out = a.copy()
    for i, b in enumerate(boxes):
        if any(j != i and o[0] < b[0] and o[1] > b[1] and o[2] < b[2] and o[3] > b[3]
               for j, o in enumerate(boxes)):
            out[lab == i + 1] = 0
    return out


def text_mask(text, font_path, px_height, canvas, center, spacing=0):
    """Supersampled alpha of text whose digits are px_height (supersampled px)
    tall, centred at center on a canvas of the given shape."""
    size = int(round(px_height / 0.66))        # Source Code Pro: digit height ~0.66 em
    font = ImageFont.truetype(font_path, size)
    img = Image.new("L", (canvas[1], canvas[0]), 0)
    d = ImageDraw.Draw(img)
    if spacing:
        widths = [font.getlength(ch) for ch in text]
        total = sum(widths) + spacing * (len(text) - 1)
        x = center[0] - total / 2
        for ch, wd in zip(text, widths):
            d.text((x + wd / 2, center[1]), ch, fill=255, font=font, anchor="mm")
            x += wd + spacing
    else:
        d.text(center, text, fill=255, font=font, anchor="mm")
    return np.asarray(img, np.float64) / 255.0


def glyph_box(chars, font_path, height_px, pad, scale=1.0):
    """Monospace glyphs, each in the same box: returns (list of (h, w) alpha
    arrays at pixel resolution, box w, box h, advance, ink top offset)."""
    hpx = height_px * scale * SS
    size = int(round(hpx / 0.66))
    font = ImageFont.truetype(font_path, size)
    adv = font.getlength("0")
    asc, desc = font.getmetrics()
    bw = int(np.ceil(adv / SS)) + 2 * pad
    bh = int(np.ceil(hpx / SS)) + 2 + 2 * pad
    out = []
    for ch in chars:
        img = Image.new("L", (bw * SS, bh * SS), 0)
        d = ImageDraw.Draw(img)
        d.text((bw * SS / 2, bh * SS / 2), ch, fill=255, font=font, anchor="mm")
        out.append(down(undot(np.asarray(img, np.float64) / 255.0)))
    return out, bw, bh, adv / SS


def odometer(word, over, n, R):
    """word on the wheels of a mechanical odometer, in a dark slot: white
    letters on black drums.  Each window shows a drum's curved face -- the
    letters squeeze and darken toward the window's top and bottom, where the
    edges of the letters before and after peek in.  Each wheel turns
    ODO_ROLL off true (all zero: aligned).  Drawn with over() onto the n x n
    supersampled dial."""
    AL = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    lh = ODO_HT * R * SS                               # supersampled px from here on
    font = ImageFont.truetype(F_BOLD, int(round(lh / 0.66)))   # cap height ~0.66 em
    cw = font.getlength("M") + 0.018 * R * SS          # a wheel's width
    gap = 0.008 * R * SS                               # the dark gap between wheels
    wh = ODO_WH * R * SS
    pad = 0.014 * R * SS
    total = len(word) * cw + (len(word) - 1) * gap
    x0 = n / 2 - total / 2
    cy = n / 2 + ODO_CY * R * SS
    pc = np.arange(n) + 0.5

    def box(xa, xb, ya, yb):
        return (((pc[None, :] >= xa) & (pc[None, :] < xb)) & ((pc[:, None] >= ya) & (pc[:, None] < yb))).astype(np.float64)

    # The bezel round the slot, its lower lip catching the light, the slot.
    over((34, 36, 44), box(x0 - pad, x0 + total + pad, cy - wh - pad, cy + wh + pad))
    over((84, 88, 100), box(x0 - pad, x0 + total + pad, cy + wh + 0.45 * pad, cy + wh + pad))
    over((0, 0, 0), box(x0, x0 + total, cy - wh, cy + wh))

    # A row of the window looks at the drum at angle th: its surface position
    # is th * rd (rd: the drum's radius), and it faces away from the eye by th.
    pitch = ODO_PITCH * lh
    rd = wh / np.sin(np.radians(ODO_TH))
    rows = np.nonzero(np.abs(pc - cy) < wh)[0]
    th = np.arcsin(np.clip((pc[rows] - cy) / rd, -1, 1))
    shade = np.cos(th) ** 2.2
    spec = 0.10 * np.exp(-((th + 0.55) / 0.22) ** 2)   # a soft sheen high on the drum (light from above)
    drum = np.array([16, 16, 18.0])
    ink = np.array([240, 240, 236.0])
    for k, ch in enumerate(word):
        xa = x0 + k * (cw + gap)
        cols = np.nonzero((pc >= xa) & (pc < xa + cw))[0]
        bw, bh = int(np.ceil(cw)) + 1, int(np.ceil(3 * pitch))
        # The drum's face, unrolled: the letter before, this one, the one after.
        img = Image.new("L", (bw, bh), 0)
        d = ImageDraw.Draw(img)
        i = AL.index(ch)
        for m in (-1, 0, 1):
            d.text((cw / 2, bh / 2 + m * pitch + lh / 2), AL[(i + m) % len(AL)], fill=255, font=font, anchor="ms")
        band = np.asarray(img, np.float64) / 255.0
        s = np.clip((th * rd + ODO_ROLL[k] * pitch + bh / 2).astype(int), 0, bh - 1)
        g = band[s][:, np.clip((pc[cols] - xa).astype(int), 0, bw - 1)]          # (rows, cols)
        c = (drum + (ink - drum) * g[..., None]) * shade[:, None, None] + 255 * spec[:, None, None]
        full = np.zeros((n, n, 3))
        a = np.zeros((n, n))
        full[np.ix_(rows, cols)] = c
        a[np.ix_(rows, cols)] = 1
        over(full, a)


def make(LW, LH, R):
    CX, CY = LW // 2, LH // 2
    S = 2 * (int(np.ceil(R)) + 2)
    ox, oy = CX - S // 2, CY - S // 2                 # picture coords of the square's corner
    n = S * SS
    c = (np.arange(n) + 0.5) / SS - S / 2             # supersampled pixel centres, from the dial centre
    X, Y = np.meshgrid(c, c)                           # Y[row, col]
    rr = np.hypot(X, Y)
    r = rr / R
    th = np.degrees(np.arctan2(X, -Y))                 # from up, clockwise

    rgb = np.zeros((n, n, 3))
    T = np.ones((n, n))                                 # transmission

    def over(color, a):
        nonlocal rgb, T
        a = np.clip(a, 0, 1)
        col = np.asarray(color, np.float64)
        if col.ndim == 1:
            col = col[None, None, :]
        rgb = rgb * (1 - a)[..., None] + col * a[..., None]
        T = T * (1 - a)

    # --- the face: smoked glass, a little of the starfield shows through
    fr = np.clip(r / 0.88, 0, 1)
    tface = 0.66 - 0.36 * fr ** 1.6
    face = np.dstack([6 + 10 * fr, 9 + 11 * fr, 22 + 16 * fr])
    face += (1.6 * np.sin(rr * 2 * np.pi / 2.2))[..., None]          # faint spun rings
    over(face, (r < 0.88) * (1 - tface))
    # The hub's shadow (the light is top-left).
    sx, sy = 0.035 * R, 0.06 * R
    hs = np.hypot(X - sx, Y - sy) / R
    over((0, 0, 0), 0.55 * (1 - smoothstep(0.09, 0.17, hs)))

    inSweep = (th >= SWEEP0 - 1.2) & (th <= SWEEP0 + SWEEP + 1.2)
    red = th >= ang(REDLINE)
    # --- the LED track
    groove = (r >= 0.795) & (r <= 0.875) & inSweep
    gcol = np.where(red[..., None], np.array([40, 8, 10.0])[None, None], np.array([16, 18, 26.0])[None, None])
    over(gcol, groove * 0.94)
    seg = ((th - SWEEP0) / 2.5) % 1.0
    led = (r >= 0.807) & (r <= 0.863) & inSweep & (seg < 0.72)
    lcol = np.where(red[..., None], np.array([70, 16, 18.0])[None, None], np.array([28, 32, 44.0])[None, None])
    over(lcol, led)

    # --- ticks (kept as a coverage too: the pie's light doesn't fall on them)
    tick = np.zeros((n, n))
    band = (r > 0.6) & (r < 0.8)
    idx = np.nonzero(band)
    bx, by = X[idx], Y[idx]
    for v in range(0, VMAX + 1, 5):
        t = np.radians(ang(v))
        e = (np.sin(t), -np.cos(t))
        along = (bx * e[0] + by * e[1]) / R
        perp = np.abs(bx * np.cos(t) + by * np.sin(t)) / R
        if v % 20 == 0:
            r0, hw, col = 0.665, 0.012, (235, 240, 250)
        elif v % 10 == 0:
            r0, hw, col = 0.705, 0.0075, (175, 185, 200)
        else:
            r0, hw, col = 0.737, 0.005, (115, 122, 136)
        if v >= REDLINE:
            col = {(235, 240, 250): (255, 70, 60), (175, 185, 200): (215, 60, 50)}.get(col, (150, 50, 45))
        m = (along >= r0) & (along <= TICK_R1) & (perp <= hw)
        a = np.zeros((n, n))
        a[idx] = m
        tick = np.maximum(tick, a)
        over(col, a)

    # --- labels
    kh = SMALL_HT * R * SS
    over((110, 165, 210), text_mask("MPH", F_SEMI, 0.06 * R * SS, (n, n), (n / 2, n / 2 + MPH_CY * R * SS), spacing=0.02 * R * SS))
    over((90, 120, 150), text_mask("km/h", F_SEMI, 0.055 * R * SS, (n, n),
                                   (n / 2 + 0.17 * R * SS, n / 2 + KMH_CY * R * SS)))
    odometer("RASTER", over, n, R)

    # --- the lip and the chrome bezel
    over((3, 3, 6), (r >= 0.88) & (r < 0.91))
    s = np.clip((r - 0.955) / 0.045, -1, 1)
    ux, uy = X / np.maximum(rr, 1e-6), Y / np.maximum(rr, 1e-6)
    nx, ny, nz = ux * s * 0.9, uy * s * 0.9, np.sqrt(np.clip(1 - (s * 0.9) ** 2, 0, 1))
    L = np.array([-0.45, -0.75, 0.6]); L /= np.linalg.norm(L)
    H = L + np.array([0, 0, 1.0]); H /= np.linalg.norm(H)
    diff = np.clip(nx * L[0] + ny * L[1] + nz * L[2], 0, 1)
    spec = np.clip(nx * H[0] + ny * H[1] + nz * H[2], 0, 1) ** 80
    ry = 2 * nz * ny                                    # the reflected ray's y (up is negative)
    env = 0.55 * smoothstep(0.25, -0.6, ry) + 0.12 * smoothstep(0.9, 0.3, ry)
    env += 0.10 * np.cos(np.radians(th) * 3 + 0.7)     # a few soft bands, like a brushed ring
    chrome = 0.10 + 0.6 * env + 0.25 * diff + 1.1 * spec
    chrome = np.clip(chrome, 0, 1.3)
    bez = np.dstack([chrome * 205, chrome * 215, chrome * 235]).clip(0, 255)
    over(bez, (r >= 0.91) & (r <= 1.0))
    over((10, 10, 14), (r >= 0.995) & (r <= 1.0))       # a dark outer edge

    # --- to pixels
    rgbP = down(rgb)
    Tp = down(T)
    tickP = down(tick)
    cover = Tp < 1 - 1 / 64.0
    dial_rgb = to565(rgbP, ox, oy)
    dial_T = np.clip(np.round(Tp * 32), 0, 32).astype(np.uint8)
    cols, rgbs, ts = [], [], []
    off = 0
    for x in range(S):
        ys = np.nonzero(cover[:, x])[0]
        if len(ys) == 0:
            cols += [0, 0, off]
            continue
        y0, y1 = ys[0], ys[-1] + 1
        cols += [oy + y0, y1 - y0, off]
        rgbs.append(dial_rgb[y0:y1, x])
        ts.append(dial_T[y0:y1, x])
        off += y1 - y0
    dial_rgb_flat = np.concatenate(rgbs)
    dial_t_flat = np.concatenate(ts)
    prev = np.clip(rgbP + 70 * Tp[..., None], 0, 255).astype(np.uint8)   # over a grey "starfield"
    Image.fromarray(prev).save(os.path.join(HERE, f"preview_dial_{LW}.png"))

    # --- what the speed lights: the arc (and its glow) and the pie
    pc = (np.arange(S) + 0.5) - S / 2
    PX, PY = np.meshgrid(pc, pc)
    pr = np.hypot(PX, PY) / R
    pth = np.degrees(np.arctan2(PX, -PY))
    core = down(((r >= 0.807) & (r <= 0.863) & inSweep & (seg < 0.72)).astype(np.float64))
    bandc = down(((r >= 0.807) & (r <= 0.863) & inSweep).astype(np.float64))
    glow = blur(bandc, 0.028 * R)
    arcI = np.clip(core * 1.0 + glow * 0.75, 0, 1)
    # The pie: the face between the numbers and the track, brighter outward,
    # but not on the ticks.
    pieI = smoothstep(0.60, 0.79, pr) * (1 - smoothstep(0.785, 0.80, pr)) * (1 - tickP)
    pieI = pieI * ((pth >= SWEEP0) & (pth <= SWEEP0 + SWEEP))
    pieI = 0.25 + 0.75 * pieI ** 1.3
    pieI = np.where((pr >= 0.60) & (pr < 0.80) & (pth >= SWEEP0) & (pth <= SWEEP0 + SWEEP), pieI, 0)
    a10 = np.clip(np.round((pth - SWEEP0) / SWEEP * 1023), 0, 1023).astype(np.uint16)
    kA = np.round(arcI * 39).astype(np.int32)
    kP = np.round(pieI * 24).astype(np.int32)
    # The stronger of the two decides the role.
    k = np.where((kP > 0) & (kA < 8), 39 + kP, kA)
    light = (a10 << 6) | k.astype(np.uint16)
    runs, lcols, ldata = [], [], []
    loff = 0
    for x in range(S):
        on = k[:, x] > 0
        first = len(runs) // 3
        y = 0
        while y < S:
            if on[y]:
                y1 = y
                while y1 < S and on[y1]:
                    y1 += 1
                runs += [oy + y, y1 - y, loff]
                ldata.append(light[y:y1, x])
                loff += y1 - y
                y = y1
            else:
                y += 1
        lcols += [first, len(runs) // 3 - first]
    ldata = np.concatenate(ldata)
    lp = np.zeros((S, S, 3))
    lp[..., 0] = np.where(k >= 40, (k - 39) * 8, 0)
    lp[..., 2] = np.where((k > 0) & (k < 40), k * 6, 0)
    Image.fromarray(lp.clip(0, 255).astype(np.uint8)).save(os.path.join(HERE, f"preview_light_{LW}.png"))

    # --- the scale numbers, at NUM_MAX scale, core + glow
    nums = []
    pad = int(np.ceil(0.05 * R))
    for v in range(0, VMAX + 1, 20):
        t = np.radians(ang(v))
        cxn, cyn = CX + NUM_R * R * np.sin(t), CY - NUM_R * R * np.cos(t)
        hpx = NUM_H * R * NUM_MAX * SS
        txt = str(v)
        size = int(round(hpx / 0.66))
        font = ImageFont.truetype(F_BOLD, size)
        tw = font.getlength(txt) / SS
        w = int(np.ceil(tw)) + 2 * pad + 2
        h = int(np.ceil(hpx / SS)) + 2 * pad + 2
        img = Image.new("L", (w * SS, h * SS), 0)
        ImageDraw.Draw(img).text((w * SS / 2, h * SS / 2), txt, fill=255, font=font, anchor="mm")
        a = down(undot(np.asarray(img, np.float64) / 255.0))
        g = np.clip(blur(a, 0.022 * R * NUM_MAX) * 1.8, 0, 1)
        q = (np.round(a * 15).astype(np.uint8) << 4) | np.round(g * 15).astype(np.uint8)
        nums.append((cxn, cyn, w, h, q.T.copy()))       # column-major

    # --- the hub
    hr = 0.115 * R
    hn = 2 * int(np.ceil(hr)) + 2
    hc = (np.arange(hn * SS) + 0.5) / SS - hn / 2
    HX, HY = np.meshgrid(hc, hc)
    hd = np.hypot(HX, HY) / hr
    inside = hd <= 1
    nz2 = np.sqrt(np.clip(1 - hd ** 2, 0, 1))
    hnx, hny = HX / hr, HY / hr
    hdiff = np.clip(hnx * L[0] + hny * L[1] + nz2 * L[2], 0, 1)
    hspec = np.clip(hnx * H[0] + hny * H[1] + nz2 * H[2], 0, 1) ** 40
    hry = 2 * nz2 * hny
    henv = 0.5 * smoothstep(0.3, -0.7, hry) + 0.1
    hb = np.clip(0.08 + 0.55 * henv + 0.3 * hdiff + 1.2 * hspec, 0, 1.3)
    hrgb = np.dstack([hb * 200, hb * 210, hb * 230])
    ring = (hd > 0.86) & inside
    hrgb[ring] *= 0.45
    jewel = hd < 0.36
    jl = np.clip(1 - np.hypot(HX / hr + 0.12, HY / hr + 0.12) / 0.36, 0, 1)
    hrgb[jewel] = np.dstack([90 + 165 * jl, 10 + 60 * jl ** 3, 10 + 40 * jl ** 3])[jewel]
    ha = down(inside.astype(np.float64))
    hp = down(hrgb * inside[..., None])
    hub_rgb = to565(np.where(ha[..., None] > 0, hp / np.maximum(ha[..., None], 1e-6), 0))
    hub_a = np.round(ha * 32).astype(np.uint8)
    hub_x0, hub_y0 = CX - hn // 2, CY - hn // 2

    # --- read-out digits
    bpad = int(np.ceil(0.05 * R))
    big, bbw, bbh, badv = glyph_box("0123456789", F_BLACK, BIG_HT * R, bpad)
    bigq = []
    for a in big:
        g = np.clip(blur(a, 0.03 * R) * 1.6, 0, 1)
        bigq.append(((np.round(a * 15).astype(np.uint8) << 4) | np.round(g * 15).astype(np.uint8)).T)
    small, sbw, sbh, sadv = glyph_box("0123456789", F_BOLD, SMALL_HT * R, 1)
    smallq = [np.round(a * 15).astype(np.uint8).T for a in small]

    return dict(LW=LW, LH=LH, CX=CX, CY=CY, R=R, S=S, ox=ox, oy=oy,
                cols=cols, rgb=dial_rgb_flat, t=dial_t_flat,
                lcols=lcols, runs=runs, ldata=ldata, nums=nums,
                hn=hn, hub_x0=hub_x0, hub_y0=hub_y0, hub_rgb=hub_rgb.T, hub_a=hub_a.T,
                big=bigq, bbw=bbw, bbh=bbh, badv=badv, small=smallq, sbw=sbw, sbh=sbh, sadv=sadv)


def emit(a, sfx):
    p = []
    p.append(c_array(f"DIAL_COL{sfx}", a["cols"], 12, "int32_t", "{}"))
    p.append(c_array(f"DIAL_RGB{sfx}", a["rgb"]))
    p.append(c_array(f"DIAL_T{sfx}", a["t"], 32, "uint8_t", "{}"))
    p.append(c_array(f"LIGHT_COL{sfx}", a["lcols"], 16, "int16_t", "{}"))
    p.append(c_array(f"LIGHT_RUN{sfx}", a["runs"], 12, "int32_t", "{}"))
    p.append(c_array(f"LIGHT{sfx}", a["ldata"]))
    nd = []
    meta = []
    off = 0
    for (cx, cy, w, h, q) in a["nums"]:
        meta += [int(round(cx * 16)), int(round(cy * 16)), w, h, off]
        nd.append(q.ravel())
        off += w * h
    p.append(c_array(f"NUM_META{sfx}", meta, 15, "int32_t", "{}"))
    p.append(c_array(f"NUM_DATA{sfx}", np.concatenate(nd), 24, "uint8_t", "0x{:02X}"))
    p.append(c_array(f"HUB_RGB{sfx}", a["hub_rgb"]))
    p.append(c_array(f"HUB_A{sfx}", a["hub_a"], 32, "uint8_t", "{}"))
    p.append(c_array(f"BIG{sfx}", np.concatenate([g.ravel() for g in a["big"]]), 24, "uint8_t", "0x{:02X}"))
    p.append(c_array(f"SMALL{sfx}", np.concatenate([g.ravel() for g in a["small"]]), 32, "uint8_t", "{}"))
    R = a["R"]
    CX, CY = a["CX"], a["CY"]
    # Read-out positions (top-left of the first glyph box).
    bigX0 = int(round(CX - 1.5 * a["badv"] - (a["bbw"] - a["badv"]) / 2))
    bigY0 = int(round(CY + BIG_CY * R - a["bbh"] / 2))
    smX0 = int(round(CX + 0.06 * R - 3 * a["sadv"] - (a["sbw"] - a["sadv"]) / 2))
    smY0 = int(round(CY + KMH_CY * R - a["sbh"] / 2))
    p.append(f"""static const SpeedoAsset ASSET{sfx} = {{
    {a['LW']}, {a['LH']}, {CX}, {CY}, {R}f,
    {a['ox']}, {a['S']}, DIAL_COL{sfx}, DIAL_RGB{sfx}, DIAL_T{sfx},
    LIGHT_COL{sfx}, LIGHT_RUN{sfx}, LIGHT{sfx},
    {len(a['nums'])}, NUM_META{sfx}, NUM_DATA{sfx},
    {a['hub_x0']}, {a['hub_y0']}, {a['hn']}, HUB_RGB{sfx}, HUB_A{sfx},
    BIG{sfx}, {a['bbw']}, {a['bbh']}, {a['badv']:.3f}f, {bigX0}, {bigY0},
    SMALL{sfx}, {a['sbw']}, {a['sbh']}, {a['sadv']:.3f}f, {smX0}, {smY0},
}};

""")
    return "".join(p)


def main():
    parts = ["""// assets.h - made by tools/gen_assets.py; don't edit.
// The raster speedometer's still parts, for 320 x 240 and 480 x 320.
// Digits and labels: Source Code Pro (c) Adobe, SIL Open Font License 1.1.
#pragma once
#include <stdint.h>

struct SpeedoAsset {
    int lw, lh, cx, cy; float R;
    int dialX0, dialW;                 // the dial's columns: picture x of the first, how many
    const int32_t *dialCol;            // per column: first row, rows, offset into the data
    const uint16_t *dialRgb;           // premultiplied RGB565
    const uint8_t *dialT;              // transmission of what's behind, 0..32
    const int16_t *lightCol;           // per dial column: first run, runs
    const int32_t *lightRun;           // per run: first row, rows, offset
    const uint16_t *light;             // angle (0..1023) << 6 | role (1..39 arc, 40..63 pie)
    int nNums;
    const int32_t *numMeta;            // per number: centre x*16, y*16, w, h, offset
    const uint8_t *numData;            // core << 4 | glow, column-major, at NUM_MAX scale
    int hubX0, hubY0, hubN;
    const uint16_t *hubRgb; const uint8_t *hubA;
    const uint8_t *big; int bigW, bigH; float bigAdv; int bigX0, bigY0;
    const uint8_t *small; int smallW, smallH; float smallAdv; int smallX0, smallY0;
};

#define NUM_MAX_SCALE """ + f"{NUM_MAX}f\n\n"]
    for (lw, lh, R, sfx) in [(320, 240, 113.0, ""), (480, 320, 151.0, "_L")]:
        a = make(lw, lh, R)
        parts.append(emit(a, sfx))
        print(f"{lw}x{lh}: dial {len(a['rgb'])} px, light {len(a['ldata'])} px")
    with open(OUT, "w") as f:
        f.write("".join(parts))
    print("wrote", os.path.normpath(OUT), os.path.getsize(OUT), "bytes")


if __name__ == "__main__":
    main()
