# The text terminal

This is one of the two ways to drive the screen on these boards: a **text terminal**,
like a computer's console. You `print()` and `printf()`, and the text appears on a grid
of character cells and scrolls up the screen as it fills. If you haven't chosen yet,
the [README](README.md#two-ways-to-drive-the-screen--pick-one) compares it with the
other way, [LVGL](README-lvgl.md).

It needs no other library, takes about 5 KB of RAM, and scrolls very fast: the screen's
own hardware moves the picture, so the ESP32 only ever draws the new line.

> **Portrait only.** The screen's hardware can only scroll along its long side. With
> the board upright (portrait) that moves text up the screen, which is what a terminal
> needs; turned sideways it would move text left or right. So the terminal works with
> the board upright or upside down, not sideways. **For a landscape screen, use
> [LVGL](README-lvgl.md)**, which draws in software and works any way round.

---

## Getting started

```cpp
#include <chipguy_CYD_resistive_terminal.h>

chipguy_CYD_resistive_display  display;
chipguy_CYD_resistive_terminal term;

void setup() {
    term.begin(display);              // start the screen; it clears to black
    term.println("Hello, CYD!");
    term.printf("%d x %d cells\n", term.columns(), term.rows());
}

void loop() {
}
```

That's all: no setup file, no settings file. The screen is **15 characters wide and
10 lines tall** on the 2.4", 2.8" and 3.2" boards, and **20 by 15** on the 3.5" and
4.0". The characters are large — each cell is 16 × 32 pixels — so they read well across
a room.

## The examples

Open them from **File ▸ Examples ▸ chipguy_ESP32-CYD-Resistive_display**.

| Example | What it shows |
|---|---|
| **TerminalHello** | The basics: colors, `printf`, tabs, symbols, box drawing, the three cursor styles. Then it counts upward, faster and faster. **Start your project from this one.** |
| **TerminalScrollSpeed** | How fast it goes: bursts of a pretend log, too fast to read, with the speed shown after each. |
| **TerminalTouchButtons** | Buttons made of colored text that you tap; they switch the RGB LED. |
| **TerminalHeaderFooter** | A picture at the top and bottom of the screen that stays put while text scrolls between them. |
| **TerminalSymbols** | Every LVGL symbol the terminal can print, with its name. |
| **TerminalCJK** | Chinese, Japanese and Korean. Uses a lot of flash on purpose — see [CJK](#chinese-japanese-and-korean). |
| **TerminalMatrixRain** | The green "digital rain" from *The Matrix*, in half-width katakana. |

---

## Printing

`print()`, `println()`, `printf()` and `putc()` all work as they do with `Serial`.
Text is **UTF-8**, so you can write `"21.5°C"` or `"±5%"` straight into your sketch.

`printf()` here never uses the heap. Width and padding work as usual —
`printf("%5d", n)`, `printf("%-8s", name)`, `printf("%06.2f", v)` — and for strings the
width counts screen cells, so columns line up even with Chinese characters, which take
two cells. One limit: a single number longer than 47 characters (say `"%.60f"`) is cut
short.

### Special characters

| Character | In a string | What it does |
|---|---|---|
| New line | `\n` | Go to the start of the next line, scrolling if needed. |
| Return | `\r` | Go back to the start of this line (handy for a progress counter that rewrites itself). |
| Tab | `\t` | Print spaces up to the next column that's a multiple of 4. |
| Backspace | `\b` | Move back one cell (doesn't erase). |
| Clear to end of line | `\v`, or `CYDR_CLEAR_EOL_STR` | Erase from the cursor to the end of the line, in the current background color. It's Ctrl-K, the "kill line" key in Emacs and many shells. |
| Clear screen | `\f` | Same as `clear()`. It's Ctrl-L, the clear-screen key in many terminals. |

### Lines that fill the screen exactly

When text fills the last cell of a line, the cursor waits just past the right edge
instead of jumping to the next line. It only moves on when you print something more.
So a line exactly as wide as the screen, followed by `\n`, takes one line, not two —
no surprise blank lines. And a full bottom line doesn't scroll the screen until there
is more to show. `setImmediateWrap(true)` switches back to jumping at once.

## Colors

```cpp
term.setTextColor(CYDR_YELLOW);                 // text color
term.setTextColor(CYDR_BLACK, CYDR_CYAN);       // text and background
term.setTextColor(cydr_rgb565(255, 128, 0));    // any color
```

Ready-made colors: `CYDR_BLACK`, `WHITE`, `GREY`, `RED`, `GREEN`, `BLUE`, `YELLOW`,
`CYAN`, `MAGENTA`, `ORANGE`. Colors are standard RGB565 numbers, the same as in LVGL
and most Arduino graphics libraries.

`clear()` fills the screen with the current **background** color and puts the cursor at
the top left. It returns immediately and the screen clears in the background.

## Where the text goes

Columns and lines are counted from 0 at the top left. `setCursor(col, row)` moves
there; `cursorCol()` and `cursorRow()` say where the cursor is; `columns()` and
`rows()` say how big the grid is.

**Row 0 is always the top line you can see** — even when only part of it is showing,
which can happen after the screen has scrolled with a [frozen area](#a-header-and-footer-that-stay-put)
that isn't a whole number of lines tall. `evictPartialLine()` scrolls on just far
enough to push a cut-off top line out of sight. `evictPartialLine(true)` scrolls back
instead, pushing a cut-off bottom line out and bringing blank space in at the top; if
the cursor was on that bottom line, it moves up one.

### The cursor

`setCursorStyle(CYDR_CURSOR_BLOCK)` (the default), `CYDR_CURSOR_UNDERLINE` or
`CYDR_CURSOR_OFF`. It blinks by itself. The terminal only remembers the line the
cursor is on, so after `setCursor()` moves to a line printed earlier, the cursor hides
over text it can't redraw, and reappears as you print there.

## Scrolling

When the screen is full, each new line scrolls everything up by sliding it smoothly
upward, one or two pixel rows at a time. Measured speeds (2026-10-19):

| Board | Screen lines per second |
|---|---|
| 2.4", 2.8", 3.2" (240 × 320) | about 115 |
| 3.5", 4.0" (320 × 480) | about 90 |

That's lines of *scrolling*: a printed line too long for the screen wraps and counts as
two.

**One small trade-off.** Each scroll step moves the picture, then draws the new row at
the bottom. If the screen happens to refresh in between, a row or two can flash for a
moment. `setScrollMode()` chooses what to do about it:

- **`CYDR_SCROLL_AUTO`** (the default) — when text is pouring in, go fast and accept it:
  with the whole screen moving, nobody can see a one-row flash. When a single line
  scrolls on its own, which you *could* notice, go the clean way instead.
- **`CYDR_SCROLL_FAST`** — always fast.
- **`CYDR_SCROLL_CLEAN`** — never flash, at about half the speed.

### When print() waits

`print()` normally returns at once: it puts the text in memory, and a background task
draws it. Your sketch only waits when it gets more than a line ahead of the screen —
for example, printing ten lines in a row while the first is still scrolling into view.
A single `\n` at the end of what you print never makes it wait. `flush()` waits until
everything printed so far is on the screen.

## A header and footer that stay put

`freeze(topPixels, bottomPixels)` takes strips at the top and bottom of the screen out
of the scrolling. Text scrolls between them, and you draw what you like into them with
`drawBitmap()`. `freeze(0, 0)` undoes it. **TerminalHeaderFooter** shows how.

```cpp
term.freeze(48, 32);                                  // 48 px header, 32 px footer
term.drawBitmap(0, 0, 240, 48, headerPixels);         // into the header
term.drawBitmap(0, term.height() - 32, 240, 32, footerPixels);
```

`drawBitmap(x, y, w, h, pixels)` draws a picture at a position on the screen, counted in
pixels from the top left. `pixels` is `w × h` RGB565 colors, row by row — the format
most image-to-C converters produce for "RGB565". Parts off the screen are cut off.
It's fine to keep drawing into the frozen strips while text scrolls; a picture drawn
onto the scrolling part scrolls away with the text.

## Touch

```cpp
chipguy_CYD_resistive_touch touch;      // next to display and term

// in setup(), after term.begin():
touch.begin(display);
touch.loadCalibration();                // what TouchCalibrate saved, if it has run
touch.loadFilters();
term.attachTouch(touch);

// in loop():
int16_t col, row;
if (term.pollTouch(col, row)) {
    // the finger is on cell (col, row): same numbers as setCursor()
}
```

`pollTouch()` answers in character cells, so "was my button touched?" is a simple
comparison — see **TerminalTouchButtons**. Touches on a frozen header or footer don't
count in cells; ask for pixels instead with
`term.pollTouch(x, y, CYDR_TOUCH_PIXELS)`, which covers the whole screen.

Touch needs calibrating once per board: see
[Touch calibration](README.md#touch-calibration--filtering).

## Symbols and fonts

The built-in font has:

- English and the accented letters of Western, Central and Eastern European languages;
- Greek, with the accents of modern Greek;
- symbols common in projects: `° ± µ Ω ℃ ≤ ≥ ≈ √ ∞ ← ↑ → ↓ ✓ ✗ ⚠ ⚡ ★ ♥`, fractions,
  superscripts and subscripts (`CO₂`, `m²`), currency (`€ £ ¥`);
- **box drawing** (`┌─┐ │ └─┘ ═ ║ ╔ ╗`), drawn so lines meet exactly between cells,
  and block shapes (`█ ▌ ▐ ▀ ▄ ░ ▒ ▓`);
- **LVGL's symbols**, under LVGL's own names: `term.print(LV_SYMBOL_WIFI)` shows the
  same Wi-Fi icon an LVGL label would. **TerminalSymbols** shows them all.

The letters are drawn from **Inconsolata**, a free font in the style of Consolas;
the symbols from **DejaVu Sans Mono** and **Font Awesome Free**. A character the font
doesn't have prints as `�`.

### Chinese, Japanese and Korean

These characters are wide — two cells each — and there are thousands of them, so they
come in a **separate font that you choose to include**. Pick the smallest that covers
what you print:

| Include | Covers | Flash it adds |
|---|---|---|
| `cydr_font_mono16_ja_kana.h` | Japanese without kanji: hiragana, katakana, half-width katakana | about 66 KB |
| `cydr_font_mono16_ko.h` | Korean: the 2,350 everyday Hangul syllables, and half-width jamo | about 0.6 MB |
| `cydr_font_mono16_ja.h` | Japanese: the 2,965 common kanji, hiragana, katakana, and half-width katakana | about 1.0 MB |
| `cydr_font_mono16_zh.h` | Chinese: the 3,755 common simplified characters | about 1.2 MB |
| `cydr_font_mono16_cjk.h` | All three, half-width forms included (Japanese kanji drawn the Chinese way) | about 2.2 MB |

```cpp
#include <cydr_font_mono16_ja.h>
#include <chipguy_CYD_resistive_terminal.h>
// ...
term.begin(display, 0, &cydr_font_mono16_ja);
```

Half-width characters (ｶﾀｶﾅ) take one cell, like Latin letters; everything else in
these fonts takes two. The CJK font includes everything in the built-in font as well.
Except for Korean alone and Japanese without kanji, it is too big for the board's
normal program space: in **Tools ▸ Partition
Scheme**, choose **"Huge APP (3MB No OTA/1MB SPIFFS)"**. **TerminalCJK** shows all
three languages.

The fonts are made by a script, [extras/fontgen/](extras/fontgen/), which can be
changed to add or drop characters.

## Upside down

`term.begin(display, 2)` runs the terminal with the board upside down (`180` works
too). `0` is the normal way up. `1` and `3` — sideways — aren't possible here (see the
top of this page); the terminal uses `0` instead and says so on the serial monitor.

## Good to know

- **The terminal owns the screen.** Once it's running, don't call the display's own
  drawing functions (`display.fillScreen()` and so on); draw pictures with
  `term.drawBitmap()`. `display.setBacklight()` is fine.
- **It uses one background task**, with a small fixed-size memory area. Printing from
  more than one of your own tasks is safe; each `print()` or `printf()` comes out whole.
- **Memory:** about 5 KB of RAM, all reserved when the sketch starts — nothing is taken
  while it runs. The built-in font and the terminal's code take about 160 KB of flash.

---

## Where to go next

- Back to the [README](README.md) for the rest of the board — LED, buttons, power,
  sound, SD card — and the full list of guides.
- [README-engineering.md](README-engineering.md#text-terminal) — how the terminal
  works inside: the hardware scrolling, the one-line buffer, the font format and the
  measurements.
