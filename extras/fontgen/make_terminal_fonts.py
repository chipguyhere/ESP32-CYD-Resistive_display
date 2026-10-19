#!/usr/bin/env python3
"""
make_terminal_fonts.py - builds the fonts for chipguy_CYD_resistive_terminal.

Writes:
  src/cydr_font_mono16.cpp      Latin + symbols + box drawing (always linked
                                when the terminal is used)
  src/cydr_font_mono16_cjk.h    opt-in: Chinese + Japanese + Korean
  src/cydr_font_mono16_zh.h     opt-in: Chinese only
  src/cydr_font_mono16_ja.h     opt-in: Japanese only
  src/cydr_font_mono16_ja_kana.h  opt-in: Japanese without kanji
  src/cydr_font_mono16_ko.h     opt-in: Korean only
  The opt-in fonts hold only double-width glyphs and fall back to
  cydr_font_mono16 for everything else.

Sources (all freely redistributable; licenses in this folder):
  Inconsolata-Regular.ttf  (SIL OFL 1.1)  primary face, the open Consolas homage
  DejaVuSansMono.ttf       (Bitstream Vera / public domain)  symbols Inconsolata lacks
  fa-solid-900.ttf, fa-brands-400.ttf  (Font Awesome Free 5.15.4: icons CC BY
                           4.0, fonts SIL OFL 1.1)  LVGL's symbol set, at the
                           code points LVGL uses
  NotoSansCJK{sc,jp,kr}-Regular.otf (SIL OFL 1.1)  ideographs, kana, Hangul
Box-drawing lines (U+2500..257F, except dashes / arcs / diagonals) and block
elements (U+2580..259F) are drawn by this script, not taken from a font, so
they meet exactly at cell edges.

Requires Python 3 with Pillow and fontTools:
  pip install pillow fonttools
Example:
  python3 make_terminal_fonts.py --inconsolata Inconsolata-Regular.ttf \
      --dejavu DejaVuSansMono.ttf --fontawesome fontawesome-free-5.15.4-web/webfonts \\
      --cjk-sc NotoSansCJKsc-Regular.otf \\
      --cjk-jp NotoSansCJKjp-Regular.otf --cjk-kr NotoSansCJKkr-Regular.otf --out ../../src
  Edit LATIN_SET / CJK_SETS below to change what goes in.

Glyph format (decoded by chipguy_CYD_resistive_terminal.cpp):
  Each glyph is 16 (narrow) or 32 (wide) pixels by cellH, row-major, 4-bit
  coverage 0..15, as a byte stream of tokens:
    0x00..0x3F  run of (n & 0x3F) + 1 background pixels (coverage 0)
    0x40..0x7F  run of (n & 0x3F) + 1 foreground pixels (coverage 15)
    0x80..0xFF  (n & 0x7F) + 1 literal pixels follow, two per byte, high
                nibble first (an odd count leaves the last low nibble unused)
  The stream may end early; the rest of the glyph is background.
  offsets[] has count + 1 entries; bit 31 of offsets[i] marks a wide glyph.
"""

import argparse, os, sys, unicodedata
from PIL import Image, ImageDraw, ImageFont
from fontTools.ttLib import TTFont

CELL_W = 16
SS = 8          # supersampling factor for synthesized glyphs

# ── Character sets ─────────────────────────────────────────────────────────

def rng(a, b):
    return list(range(a, b + 1))

LATIN_SET = (
    rng(0x20, 0x7E) + rng(0xA0, 0xFF) + rng(0x100, 0x17F) +
    [0x192, 0x218, 0x219, 0x21A, 0x21B, 0x2C6, 0x2C7, 0x2D8, 0x2D9, 0x2DA, 0x2DB, 0x2DC, 0x2DD] +
    [0x37E] + rng(0x384, 0x38A) + [0x38C] + rng(0x38E, 0x3A1) +        # Greek, monotonic
    rng(0x3A3, 0x3CE) +                                                   # (tonos, dialytika)
    rng(0x2010, 0x2027) + [0x2030, 0x2032, 0x2033, 0x2039, 0x203A, 0x203C, 0x2044] +
    [0x2070, 0x2071] + rng(0x2074, 0x208E) +                              # super/subscripts
    [0x20A4, 0x20A9, 0x20AC, 0x20B9, 0x20BD, 0x20BF] +                    # currency
    [0x2103, 0x2109, 0x2116, 0x2122, 0x2126, 0x212B] +                    # ℃ ℉ № ™ Ω Å
    rng(0x2150, 0x215E) +                                                 # vulgar fractions
    rng(0x2190, 0x21FF) +                                                 # arrows
    rng(0x2200, 0x22FF) +                                                 # math operators
    [0x2300, 0x2302, 0x2303, 0x2304, 0x2318, 0x2325, 0x2326, 0x2327, 0x232B, 0x23CE, 0x23CF] +
    rng(0x23E9, 0x23F3) + rng(0x23FB, 0x23FE) +                           # media / power symbols
    rng(0x2500, 0x257F) + rng(0x2580, 0x259F) +                           # box drawing, blocks
    rng(0x25A0, 0x25FF) +                                                 # geometric shapes
    rng(0x2600, 0x26FF) + rng(0x2700, 0x27BF) +                           # misc symbols, dingbats
    [0xFFFD]
)

# LVGL's built-in symbols: the names and Private Use Area code points of
# lv_symbol_def.h (LVGL 9), taken from its UTF-8 strings (the comment beside
# FILE there says 0xF158; the string is U+F15B).  LVGL drew these from Font Awesome 5 Free; so do
# we, straight from Font Awesome's own font files -- nothing is taken from
# LVGL.  At the same code points, LV_SYMBOL_* strings print the same glyph on
# the terminal as in LVGL.  NEW_LINE is not in Font Awesome Free; U+21B5 from
# DejaVu stands in.
LVGL_SYMBOLS = [
    ('AUDIO', 0xF001), ('VIDEO', 0xF008), ('LIST', 0xF00B), ('OK', 0xF00C),
    ('CLOSE', 0xF00D), ('POWER', 0xF011), ('SETTINGS', 0xF013), ('HOME', 0xF015),
    ('DOWNLOAD', 0xF019), ('DRIVE', 0xF01C), ('REFRESH', 0xF021), ('MUTE', 0xF026),
    ('VOLUME_MID', 0xF027), ('VOLUME_MAX', 0xF028), ('IMAGE', 0xF03E), ('TINT', 0xF043),
    ('PREV', 0xF048), ('PLAY', 0xF04B), ('PAUSE', 0xF04C), ('STOP', 0xF04D),
    ('NEXT', 0xF051), ('EJECT', 0xF052), ('LEFT', 0xF053), ('RIGHT', 0xF054),
    ('PLUS', 0xF067), ('MINUS', 0xF068), ('EYE_OPEN', 0xF06E), ('EYE_CLOSE', 0xF070),
    ('WARNING', 0xF071), ('SHUFFLE', 0xF074), ('UP', 0xF077), ('DOWN', 0xF078),
    ('LOOP', 0xF079), ('DIRECTORY', 0xF07B), ('UPLOAD', 0xF093), ('CALL', 0xF095),
    ('CUT', 0xF0C4), ('COPY', 0xF0C5), ('SAVE', 0xF0C7), ('BARS', 0xF0C9),
    ('ENVELOPE', 0xF0E0), ('CHARGE', 0xF0E7), ('PASTE', 0xF0EA), ('BELL', 0xF0F3),
    ('KEYBOARD', 0xF11C), ('GPS', 0xF124), ('FILE', 0xF15B), ('WIFI', 0xF1EB),
    ('BATTERY_FULL', 0xF240), ('BATTERY_3', 0xF241), ('BATTERY_2', 0xF242),
    ('BATTERY_1', 0xF243), ('BATTERY_EMPTY', 0xF244), ('USB', 0xF287),
    ('BLUETOOTH', 0xF293), ('TRASH', 0xF2ED), ('EDIT', 0xF304), ('BACKSPACE', 0xF55A),
    ('SD_CARD', 0xF7C2), ('NEW_LINE', 0xF8A2),
]

def render_icon(font_path, cp, W, H, box_w, box_h, cy):
    """An icon scaled to fit box_w x box_h (aspect kept), centered
    horizontally and on row cy."""
    f = ImageFont.truetype(font_path, size=128)
    big = Image.new('L', (256, 256), 0)
    ImageDraw.Draw(big).text((64, 192), chr(cp), font=f, fill=255, anchor='ls')
    bb = big.getbbox()
    g = big.crop(bb)
    k = min(box_w / g.width, box_h / g.height)
    gw, gh = max(1, round(g.width * k)), max(1, round(g.height * k))
    g = g.resize((gw, gh), Image.LANCZOS)
    im = Image.new('L', (W, H), 0)
    im.paste(g, ((W - gw) // 2, round(cy - gh / 2)))
    return im

def _decode_rows(enc, lead_lo, lead_hi):
    out = set()
    for a in range(lead_lo, lead_hi + 1):
        for b in range(0xA1, 0xFF):
            try:
                out.add(ord(bytes([a, b]).decode(enc)))
            except Exception:
                pass
    return out

# Per-language subsets.  Each carries the CJK punctuation and fullwidth forms;
# the superset is their union.
CJK_COMMON = set(rng(0x3000, 0x303F)) | set(rng(0xFF01, 0xFF60)) | set(rng(0xFFE0, 0xFFE6))
CJK_SETS = {
    # GB2312 level-1 hanzi: the 3755 most used Simplified Chinese characters
    'zh': lambda: _decode_rows('gb2312', 0xB0, 0xD7) | CJK_COMMON,
    # JIS X 0208 level-1 kanji (2965) plus hiragana and katakana
    'ja': lambda: _decode_rows('euc_jp', 0xB0, 0xCF) | set(rng(0x3041, 0x3096)) |
                  set(rng(0x3099, 0x30FF)) | CJK_COMMON,
    # Japanese without kanji: hiragana and katakana
    'ja_kana': lambda: set(rng(0x3041, 0x3096)) | set(rng(0x3099, 0x30FF)) | CJK_COMMON,
    # KS X 1001 Hangul syllables (the 2350 in everyday use) plus jamo
    'ko': lambda: _decode_rows('euc_kr', 0xB0, 0xC8) | set(rng(0x3131, 0x318E)) | CJK_COMMON,
}

# Half-width forms, one cell each: katakana with the Japanese half-width
# punctuation, Hangul jamo, and a few symbols.  (Unicode has no half-width
# hiragana.)  They belong with their language: a smaller Japanese set, if one
# is ever wanted, should drop kanji only, never the kana.
HALF_WIDTH = {
    'ja': rng(0xFF61, 0xFF9F),
    'ko': rng(0xFFA0, 0xFFDC),
}
HALF_WIDTH_SYMBOLS = rng(0xFFE8, 0xFFEE)

def cjk_set(lang):
    if lang == 'cjk':
        s = set()
        for f in CJK_SETS.values():
            s |= f()
    else:
        s = CJK_SETS[lang]()
    s = {c for c in s if is_wide(c)}
    for k, half in HALF_WIDTH.items():
        if lang == 'cjk' or lang.startswith(k):
            s |= set(half)
    if lang == 'cjk':
        s |= set(HALF_WIDTH_SYMBOLS)
    return sorted(s)

def is_wide(cp):
    return unicodedata.east_asian_width(chr(cp)) in ('W', 'F')

# ── Font sources ───────────────────────────────────────────────────────────

class Source:
    """A font file scaled so its advance (or em, for CJK) fits the cell."""
    def __init__(self, path, adv_px=None, em_px=None):
        tt = TTFont(path, fontNumber=0, lazy=True)
        self.cmap = tt.getBestCmap()
        upem = tt['head'].unitsPerEm
        if adv_px is not None:
            adv = tt['hmtx'][self.cmap[ord('0')]][0]
            self.size = adv_px * upem / adv
        else:
            self.size = em_px
        self.font = ImageFont.truetype(path, size=self.size)
        self.upem = upem
        self.hhea = (tt['hhea'].ascent, tt['hhea'].descent)

    def has(self, cp):
        return cp in self.cmap

    def render(self, cp, w, h, baseline):
        """8-bit coverage image w x h with the glyph's baseline at `baseline`.
        A glyph whose ink would cross the top or bottom of the cell but is
        short enough to fit (Ș, Ģ, ĺ ...) is moved up or down until it does."""
        pad = h
        big = Image.new('L', (w, h + 2 * pad), 0)
        ImageDraw.Draw(big).text((0, baseline + pad), chr(cp), font=self.font, fill=255, anchor='ls')
        bb = big.getbbox()
        shift = 0
        if bb:
            top, bot = bb[1] - pad, bb[3] - pad
            if bot > h:
                shift = h - bot
            if top + shift < 0:
                shift = -top
        return big.crop((0, pad - shift, w, pad - shift + h))

    def render_centered(self, cp, w, h):
        """CJK: em box centered in the w x h cell (ideographic em box sits
        from descent -0.12 em to ascent 0.88 em)."""
        em = self.size
        top = (h - em) / 2.0
        baseline = top + 0.88 * em
        im = Image.new('L', (w, h), 0)
        ImageDraw.Draw(im).text((w / 2.0, baseline), chr(cp), font=self.font, fill=255, anchor='ms')
        return im

    def render_stretched(self, cp, w, h):
        """Render the font's full line box (ascent..descent) for one advance and
        scale it to exactly w x h, so box-drawing glyphs designed to touch the
        line box edges touch the cell edges."""
        k = 4
        asc, desc = self.hhea
        scale = self.size / self.upem
        lh = (asc - desc) * scale
        big = ImageFont.truetype(self.font.path, size=self.size * k)
        bw, bh = int(round(w * k)), int(round(lh * k))
        im = Image.new('L', (bw, bh), 0)
        ImageDraw.Draw(im).text((0, asc * scale * k), chr(cp), font=big, fill=255, anchor='ls')
        return im.resize((w, h), Image.LANCZOS)

# ── Synthesized box drawing and block elements ─────────────────────────────

NONE, LIGHT, HEAVY, DOUBLE = 0, 1, 2, 3
DIRS = ('UP', 'RIGHT', 'DOWN', 'LEFT')

def parse_box_name(cp):
    """Arm weights {UP,RIGHT,DOWN,LEFT} for a plain line glyph, from its
    Unicode name; None for dashes, arcs and diagonals."""
    name = unicodedata.name(chr(cp), '')
    if not name.startswith('BOX DRAWINGS '):
        return None
    body = name[len('BOX DRAWINGS '):]
    if any(w in body for w in ('DASH', 'ARC', 'DIAGONAL')):
        return None
    weights = {'LIGHT': LIGHT, 'SINGLE': LIGHT, 'HEAVY': HEAVY, 'DOUBLE': DOUBLE}
    arms = dict.fromkeys(DIRS, NONE)
    inherit = None
    for clause in body.split(' AND '):
        words = clause.split()
        wt = next((weights[w] for w in words if w in weights), None)
        if wt is None:
            wt = inherit
        inherit = wt
        for w in words:
            if w == 'VERTICAL':
                arms['UP'] = arms['DOWN'] = wt
            elif w == 'HORIZONTAL':
                arms['LEFT'] = arms['RIGHT'] = wt
            elif w in DIRS:
                arms[w] = wt
    return arms

def synth_box(cp, W, H):
    """Line glyphs at SS x resolution.  Light = 2 px, heavy = 4 px, double =
    two 2 px lines with a 2 px gap.  Each line of an arm runs from the cell
    edge toward the center and stops where it meets the perpendicular arms,
    so corners, tees and crossings join like the classic drawings."""
    arms = parse_box_name(cp)
    if arms is None:
        return None
    S = SS
    cx, cy = (W // 2) * S, (H // 2) * S
    t, g = 2 * S, 2 * S

    def lines(wt, c):
        return {LIGHT: [(c, t)], HEAVY: [(c, 2 * t)],
                DOUBLE: [(c - g, t), (c + g, t)]}.get(wt, [])

    L = {d: lines(arms[d], cx if d in ('UP', 'DOWN') else cy) for d in DIRS}
    opposite = {'UP': 'DOWN', 'DOWN': 'UP', 'LEFT': 'RIGHT', 'RIGHT': 'LEFT'}
    perps = {'UP': ('LEFT', 'RIGHT'), 'DOWN': ('LEFT', 'RIGHT'),
             'LEFT': ('UP', 'DOWN'), 'RIGHT': ('UP', 'DOWN')}
    center = {'UP': cy, 'DOWN': cy, 'LEFT': cx, 'RIGHT': cx}
    fromLow = {'UP': True, 'LEFT': True, 'DOWN': False, 'RIGHT': False}

    # Lines of perpendicular arm P lie across arm A's axis.  "Cover" = the edge
    # of P's line on the side away from A's origin, so A overlaps it fully.
    def near_cover(A, P):
        c, th = min(L[P]) if fromLow[A] else max(L[P])
        return c + th // 2 if fromLow[A] else c - th // 2
    def far_cover(A, P):
        c, th = max(L[P]) if fromLow[A] else min(L[P])
        return c + th // 2 if fromLow[A] else c - th // 2

    im = Image.new('L', (W * S, H * S), 0)
    dr = ImageDraw.Draw(im)
    for A in DIRS:
        p1, p2 = perps[A]
        for (c, th) in L[A]:
            if arms[A] == DOUBLE:
                if A in ('UP', 'DOWN'):
                    side = 'LEFT' if c < cx else 'RIGHT'
                else:
                    side = 'UP' if c < cy else 'DOWN'
                other = p2 if side == p1 else p1
                if L[side]:
                    end = near_cover(A, side)
                elif L[opposite[A]]:
                    end = center[A]
                elif L[other]:
                    end = far_cover(A, other)
                else:
                    end = center[A]
            else:
                if L[opposite[A]]:
                    end = center[A]
                elif L[p1] and L[p2]:
                    ends = (near_cover(A, p1), near_cover(A, p2))
                    end = max(ends) if fromLow[A] else min(ends)
                elif L[p1] or L[p2]:
                    end = far_cover(A, p1 if L[p1] else p2)
                else:
                    end = center[A]
            lo, hi = (0, end) if fromLow[A] else (end, (H if A == 'DOWN' else W) * S)
            if A in ('UP', 'DOWN'):
                dr.rectangle([c - th // 2, lo, c + th // 2 - 1, hi - 1], fill=255)
            else:
                dr.rectangle([lo, c - th // 2, hi - 1, c + th // 2 - 1], fill=255)
    return im.resize((W, H), Image.BOX)

def synth_block(cp, W, H):
    """Block elements U+2580..259F as exact fractions of the cell; the three
    shades are flat grey levels (the panel can show grey, so no dither)."""
    if not (0x2580 <= cp <= 0x259F):
        return None
    im = Image.new('L', (W * SS, H * SS), 0)
    dr = ImageDraw.Draw(im)
    w, h = W * SS, H * SS
    def box(x0, y0, x1, y1, v=255):
        dr.rectangle([round(x0 * w), round(y0 * h), round(x1 * w) - 1, round(y1 * h) - 1], fill=v)
    q = {'UL': (0, 0, .5, .5), 'UR': (.5, 0, 1, .5), 'LL': (0, .5, .5, 1), 'LR': (.5, .5, 1, 1)}
    if cp == 0x2580: box(0, 0, 1, .5)
    elif 0x2581 <= cp <= 0x2588: box(0, 1 - (cp - 0x2580) / 8, 1, 1)
    elif 0x2589 <= cp <= 0x258F: box(0, 0, (0x2590 - cp) / 8, 1)
    elif cp == 0x2590: box(.5, 0, 1, 1)
    elif cp in (0x2591, 0x2592, 0x2593):
        box(0, 0, 1, 1, {0x2591: 64, 0x2592: 128, 0x2593: 192}[cp])
    elif cp == 0x2594: box(0, 0, 1, 1 / 8)
    elif cp == 0x2595: box(7 / 8, 0, 1, 1)
    else:
        quads = {0x2596: 'LL', 0x2597: 'LR', 0x2598: 'UL', 0x2599: 'UL LL LR',
                 0x259A: 'UL LR', 0x259B: 'UL UR LL', 0x259C: 'UL UR LR',
                 0x259D: 'UR', 0x259E: 'UR LL', 0x259F: 'UR LL LR'}[cp]
        for k in quads.split():
            box(*q[k])
    return im.resize((W, H), Image.BOX)

# ── Encoding ───────────────────────────────────────────────────────────────

def quantize(im):
    return [min(15, (v * 15 + 127) // 255) for v in im.tobytes()]

def rle(px):
    out = bytearray()
    while px and px[-1] == 0:          # trailing background is implicit
        px = px[:-1]
    i, n = 0, len(px)
    lit = []
    def flush_lit():
        while lit:
            chunk = lit[:128]
            del lit[:128]
            out.append(0x80 | (len(chunk) - 1))
            for k in range(0, len(chunk), 2):
                hi = chunk[k]
                lo = chunk[k + 1] if k + 1 < len(chunk) else 0
                out.append((hi << 4) | lo)
    while i < n:
        v = px[i]
        if v in (0, 15):
            j = i
            while j < n and px[j] == v and j - i < 64:
                j += 1
            # a run of 1-2 inside literals is cheaper as literal
            if j - i >= 3 or not lit:
                flush_lit()
                out.append((0x00 if v == 0 else 0x40) | (j - i - 1))
                i = j
                continue
        lit.append(v)
        i += 1
    flush_lit()
    return bytes(out)

def unrle(data, npx):
    px, i = [], 0
    while i < len(data) and len(px) < npx:
        t = data[i]; i += 1
        if t < 0x80:
            px += [0 if t < 0x40 else 15] * ((t & 0x3F) + 1)
        else:
            k = (t & 0x7F) + 1
            for m in range(k):
                b = data[i + m // 2]
                px.append(b >> 4 if m % 2 == 0 else b & 15)
            i += (k + 1) // 2
    return (px + [0] * npx)[:npx]

# ── Output ─────────────────────────────────────────────────────────────────

def c_array(name, ctype, values, per_line=16, fmt='0x{:02X}', storage='static'):
    lines = []
    for k in range(0, len(values), per_line):
        lines.append('    ' + ', '.join(fmt.format(v) for v in values[k:k + per_line]) + ',')
    return f'{storage} const {ctype} {name}[] = {{\n' + '\n'.join(lines) + '\n};\n'

def c_bytes(name, data, per_line=100, storage='static'):
    """Bytes as a string literal: compiles far faster than a brace list and
    keeps the source about a third the size.  Octal escapes are always three
    digits, so a following digit can't be swallowed."""
    out = [f'{storage} const uint8_t {name}[] =']
    for k in range(0, len(data), per_line):
        lit = []
        for b in data[k:k + per_line]:
            ch = chr(b)
            if 0x20 <= b < 0x7F and ch not in '"\\?':
                lit.append(ch)
            else:
                lit.append('\\%03o' % b)
        out.append('    "' + ''.join(lit) + '"')
    return '\n'.join(out) + ';\n'

def emit(path, symbol, glyphs, cell_h, fallback, header_text, as_header):
    cps = [g[0] for g in glyphs]
    data, offs = bytearray(), []
    for cp, wide, enc in glyphs:
        offs.append(len(data) | (0x80000000 if wide else 0))
        data += enc
    offs.append(len(data))
    with open(path, 'w') as f:
        f.write(header_text)
        if as_header:
            f.write('#pragma once\n\n')
        f.write('#include "chipguy_CYD_resistive_terminal.h"\n\n')
        f.write(f'// {len(cps)} glyphs, {len(data)} bytes of glyph data, '
                f'{len(cps) * 6 + 4} bytes of index.\n\n')
        p = symbol + '_'
        st = 'inline' if as_header else 'static'
        f.write(c_array(p + 'cp', 'uint16_t', cps, 12, '0x{:04X}', st))
        f.write(c_array(p + 'off', 'uint32_t', offs, 8, '0x{:08X}', st))
        f.write(c_bytes(p + 'data', bytes(data), storage=st))
        fb = f'&{fallback}' if fallback else 'nullptr'
        inline = 'inline ' if as_header else ''
        f.write(f'\n{inline}const cydr_font_t {symbol} = {{\n'
                f'    {CELL_W}, {cell_h}, {len(cps)},\n'
                f'    {p}cp, {p}off, {p}data, {fb}\n}};\n')
    return len(data) + len(cps) * 6

def preview(path, glyphs, cell_h, cols=32):
    rows = (len(glyphs) + cols - 1) // cols
    im = Image.new('L', (cols * 33, rows * (cell_h + 1)), 40)
    for k, (cp, wide, enc) in enumerate(glyphs):
        w = 32 if wide else 16
        px = unrle(enc, w * cell_h)
        g = Image.new('L', (w, cell_h))
        g.putdata([v * 17 for v in px])
        im.paste(g, ((k % cols) * 33, (k // cols) * (cell_h + 1)))
    im.save(path)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--inconsolata', required=True)
    ap.add_argument('--dejavu', required=True)
    ap.add_argument('--fontawesome', help='Font Awesome 5 Free webfonts/ folder (LVGL symbols)')
    ap.add_argument('--cjk-sc', help='NotoSansCJKsc-Regular.otf (superset and Chinese)')
    ap.add_argument('--cjk-jp', help='NotoSansCJKjp-Regular.otf (Japanese)')
    ap.add_argument('--cjk-kr', help='NotoSansCJKkr-Regular.otf (Korean)')
    ap.add_argument('--out', required=True)
    ap.add_argument('--preview', help='directory for PNG previews')
    ap.add_argument('--cell-height', type=int, default=32)
    ap.add_argument('--baseline', type=int, default=26)
    a = ap.parse_args()

    inc = Source(a.inconsolata, adv_px=CELL_W)
    dv = Source(a.dejavu, adv_px=CELL_W)

    # Cell 16 x 32 with the baseline 26 px down: accented capitals (26 px of
    # ink above the baseline at this size) and g / y / ç (6 below) fit; the
    # handful taller than that are shifted to fit by Source.render().
    cell_h, baseline = a.cell_height, a.baseline
    print(f'Inconsolata at {inc.size:.1f} px: cell {CELL_W}x{cell_h}, baseline {baseline}')

    glyphs, src_count = [], {}
    for cp in LATIN_SET:
        im = synth_block(cp, CELL_W, cell_h) or synth_box(cp, CELL_W, cell_h)
        src = 'synth'
        if im is None:
            if 0x2500 <= cp <= 0x257F:
                s = inc if inc.has(cp) else dv
                im, src = s.render_stretched(cp, CELL_W, cell_h), 'stretch'
            elif inc.has(cp):
                im, src = inc.render(cp, CELL_W, cell_h, baseline), 'inconsolata'
            elif dv.has(cp):
                im, src = dv.render(cp, CELL_W, cell_h, baseline), 'dejavu'
            else:
                continue
        src_count[src] = src_count.get(src, 0) + 1
        glyphs.append((cp, False, rle(quantize(im))))
    print('Latin sources:', src_count)

    # LVGL symbols, 16 px wide by up to 20 tall, centered on the capitals.
    if a.fontawesome:
        fa = {k: (os.path.join(a.fontawesome, f'fa-{k}.ttf'))
              for k in ('solid-900', 'brands-400')}
        fa_cmap = {k: TTFont(v, lazy=True).getBestCmap() for k, v in fa.items()}
        cap_mid = baseline - 10
        for name, cp in LVGL_SYMBOLS:
            path = next((fa[k] for k in fa if cp in fa_cmap[k]), None)
            src_cp = cp
            if path is None:            # NEW_LINE: not in Font Awesome Free
                path, src_cp = a.dejavu, 0x21B5
            im = render_icon(path, src_cp, CELL_W, cell_h, CELL_W, 20, cap_mid)
            glyphs.append((cp, False, rle(quantize(im))))
        glyphs.sort(key=lambda g: g[0])
        print(f'LVGL symbols: {len(LVGL_SYMBOLS)}')

    hdr = ('// Generated by extras/fontgen/make_terminal_fonts.py - do not edit.\n'
           '// Glyphs rendered from Inconsolata (SIL OFL 1.1, (c) The Inconsolata\n'
           '// Project Authors) and DejaVu Sans Mono (Bitstream Vera license /\n'
           '// public domain); box drawing and block elements drawn by the script.\n'
           '// LVGL\'s symbol set at its LV_SYMBOL_* code points (U+F001..U+F8A2),\n'
           '// rendered from Font Awesome Free 5.15.4 by Fonticons, Inc. (icons CC BY\n'
           '// 4.0, fonts SIL OFL 1.1; https://fontawesome.com/license/free).\n'
           '// See extras/fontgen/ for the licenses and the generator.\n\n')
    n = emit(os.path.join(a.out, 'cydr_font_mono16.cpp'), 'cydr_font_mono16',
             glyphs, cell_h, None, hdr, False)
    print(f'cydr_font_mono16: {len(glyphs)} glyphs, {n} bytes')
    if a.preview:
        preview(os.path.join(a.preview, 'mono16.png'), glyphs, cell_h)

    # Opt-in CJK headers: the superset, and one per language so a sketch
    # needing only one doesn't carry the others.  Japanese and Korean use the
    # regional Noto variants (Japanese kanji forms); the superset must pick one
    # set of Han forms and uses Simplified Chinese.
    em = min(cell_h, 2 * CELL_W) - 2
    variants = [
        ('cjk', a.cjk_sc, 'Chinese (GB2312 level-1 hanzi), Japanese (JIS X 0208 level-1\n'
                          '// kanji, kana) and Korean (KS X 1001 Hangul, jamo), rendered from Noto\n'
                          '// Sans CJK SC -- so Japanese kanji take the Simplified Chinese forms'),
        ('zh', a.cjk_sc, 'Simplified Chinese: GB2312 level-1 hanzi (the 3755 most used),\n'
                         '// rendered from Noto Sans CJK SC'),
        ('ja', a.cjk_jp, 'Japanese: JIS X 0208 level-1 kanji (2965), hiragana and katakana,\n'
                         '// rendered from Noto Sans CJK JP'),
        ('ja_kana', a.cjk_jp, 'Japanese without kanji: hiragana and katakana,\n'
                              '// rendered from Noto Sans CJK JP'),
        ('ko', a.cjk_kr, 'Korean: the 2350 KS X 1001 Hangul syllables and jamo,\n'
                         '// rendered from Noto Sans CJK KR'),
    ]
    for lang, path, what in variants:
        if not path:
            continue
        src = Source(path, em_px=em)
        cg = []
        for cp in cjk_set(lang):
            if src.has(cp):
                w = is_wide(cp)
                im = src.render_centered(cp, (2 if w else 1) * CELL_W, cell_h)
                cg.append((cp, w, rle(quantize(im))))
        sym = 'cydr_font_mono16_' + lang
        hdr2 = ('// Generated by extras/fontgen/make_terminal_fonts.py - do not edit.\n'
                '// OPT-IN font for chipguy_CYD_resistive_terminal, mostly double-width cells:\n'
                '// ' + what + '.\n'
                '// Plus CJK punctuation, fullwidth forms, and the half-width (one-cell) forms\n'
                '// of the language.  Noto Sans CJK is (c) Adobe,\n'
                '// SIL OFL 1.1 (extras/fontgen/).  Everything else falls back to\n'
                '// cydr_font_mono16.\n'
                '//\n'
                '// FLASH: about SIZE_PLACEHOLDER.  Include this header in ONE file of a sketch,\n'
                '// then term.setFont(&' + sym + ').\n\n')
        out = os.path.join(a.out, sym.replace('cydr_font_', 'cydr_font_') + '.h')
        n2 = emit(out, sym, cg, cell_h, 'cydr_font_mono16', hdr2, True)
        txt = open(out).read().replace('SIZE_PLACEHOLDER', f'{(n2 + 1023) // 1024} KB ({len(cg)} glyphs)')
        open(out, 'w').write(txt)
        print(f'{sym}: {len(cg)} glyphs, {n2} bytes')
        if a.preview:
            preview(os.path.join(a.preview, lang + '.png'), cg[:1024], cell_h)

if __name__ == '__main__':
    main()
