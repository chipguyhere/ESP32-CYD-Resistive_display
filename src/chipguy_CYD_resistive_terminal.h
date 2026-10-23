/*
 * Console-style text terminal for the resistive-touch CYD family -- the
 * alternative to LVGL.  print(), printf() and putc() onto a grid of 16-pixel
 * character cells, scrolled by the PANEL'S OWN HARDWARE, one pixel row at a
 * time, so a screenful of text scrolls without redrawing it.
 *
 *   #include <chipguy_CYD_resistive_terminal.h>
 *   chipguy_CYD_resistive_display  display;
 *   chipguy_CYD_resistive_terminal term;
 *   void setup() { term.begin(display); term.println("Hello"); }
 *
 * ─────────────────────────────────────────────────────────────────────────
 *  HARDWARE SCROLLING WORKS ON ONE AXIS ONLY -- THE PANEL'S LONG ONE.
 *
 *  The ILI9341 / ST7789 / ST7796 controllers scroll their frame memory along
 *  the panel's native rows, which run down the long side of the glass.  Text
 *  lines scroll that way only in PORTRAIT (rotation 0, or 2 for upside down).
 *  In landscape the same hardware would slide the text sideways, which is no
 *  use to a terminal, so begin() refuses rotations 1 and 3 and uses 0.
 *
 *  If your project needs the display ROTATED (landscape), use LVGL instead:
 *  see README-lvgl.md and the Lvgl* examples.  LVGL redraws in software and
 *  works in every orientation.
 * ─────────────────────────────────────────────────────────────────────────
 *
 * What it costs, all allocated statically inside the object -- the terminal
 * never touches the heap:
 *   - one array of character cells, ONE TEXT LINE long (character, foreground
 *     and background color per cell).  Lines above the cursor's line are on
 *     the panel only; the terminal does not remember them.
 *   - one DMA buffer of exactly TWO pixel rows (1.3 KB on a 320-wide panel).
 *   - one high-priority FreeRTOS task with a small stack, which renders the
 *     line, runs the scroll, clears the screen and blinks the cursor.
 *
 * Blocking: print() returns as soon as the text is in the cell array.  The
 * task renders it from there, and the caller only waits when it gets more than
 * one line ahead -- text that would need a SECOND line while the current one
 * is still being drawn or scrolled into view.  One trailing '\n' never waits.
 * See README-terminal.md, "When print() waits".
 *
 * Coordinates: columns and rows count character cells from the top-left of
 * the scrolling (unfrozen) area.  Row 0 is always the top line on screen,
 * even when scrolling or a frozen area has left only part of it showing; see
 * evictPartialLine().  drawBitmap() and pollTouch(..., CYDR_TOUCH_PIXELS) use
 * absolute screen pixels instead.
 *
 * Control characters:
 *   '\n'  new line (column 0 of the next line, scrolling if needed)
 *   '\r'  column 0 of this line
 *   '\t'  spaces up to the next column that is a multiple of 4
 *   '\b'  cursor one cell left (does not erase)
 *   '\v'  (Ctrl-K, as in Emacs and readline) clear to the end of the line;
 *         also spelled CYDR_CLEAR_EOL
 *   '\f'  (Ctrl-L) clear the screen, as clear()
 * Other control characters are ignored.  Text is UTF-8; code points above
 * U+FFFF and characters missing from the font print as U+FFFD.
 *
 * Do not call the display object's own drawing functions (fillScreen,
 * flushRegion, setRotation) once the terminal is running: the terminal's task
 * owns the panel.  setBacklight() is fine.  The touch driver is fine too.
 */

#pragma once

#include <Arduino.h>
#include <stdarg.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include "chipguy_CYD_resistive_display.h"
#include "chipguy_CYD_resistive_touch.h"

// ── Tuning (define before including to change) ─────────────────────────────

// Task stack, bytes.  The task keeps its frames small; check the margin on
// your build with stackHighWater().
#ifndef CYDR_TERM_STACK_BYTES
#define CYDR_TERM_STACK_BYTES 2560
#endif
// Task priority (the Arduino loop task runs at 1) and core (tskNO_AFFINITY =
// either core).
#ifndef CYDR_TERM_TASK_PRIORITY
#define CYDR_TERM_TASK_PRIORITY 10
#endif
#ifndef CYDR_TERM_TASK_CORE
#define CYDR_TERM_TASK_CORE tskNO_AFFINITY
#endif
// printf() renders one number at a time with snprintf into a buffer this big
// on the caller's stack.  %s strings never go through it.  A single number
// longer than this (e.g. "%.80f") is cut short.
#ifndef CYDR_TERM_PRINTF_NUMBUF
#define CYDR_TERM_PRINTF_NUMBUF 48
#endif

// Widest panel in the family (the 3.5" / 4.0" in portrait) and the fixed
// character cell width.
#define CYDR_TERM_MAX_WIDTH 320
#define CYDR_TERM_CELL_W    16
#define CYDR_TERM_MAX_COLS  (CYDR_TERM_MAX_WIDTH / CYDR_TERM_CELL_W)

#define CYDR_CLEAR_EOL     '\v'
#define CYDR_CLEAR_EOL_STR "\v"

// ── LVGL's symbols ─────────────────────────────────────────────────────────
// The built-in font carries LVGL's symbol set at the code points LVGL uses,
// rendered from Font Awesome Free (icons CC BY 4.0 by Fonticons, Inc.), and
// the names below are LVGL's own (from lv_symbol_def.h, LVGL, MIT license).
// So a line written for the terminal moves to an LVGL label unchanged, and
// back:
//   term.print(LV_SYMBOL_WIFI " connected");
// Each is guarded as LVGL guards its own, so lvgl.h may be included before or
// after this header.  Each prints in one cell.  The TerminalSymbols example
// shows them all with their names.
#ifndef LV_SYMBOL_BULLET
#define LV_SYMBOL_BULLET        "\xE2\x80\xA2"   // U+2022
#endif
#ifndef LV_SYMBOL_AUDIO
#define LV_SYMBOL_AUDIO         "\xEF\x80\x81"   // U+F001
#endif
#ifndef LV_SYMBOL_VIDEO
#define LV_SYMBOL_VIDEO         "\xEF\x80\x88"   // U+F008
#endif
#ifndef LV_SYMBOL_LIST
#define LV_SYMBOL_LIST          "\xEF\x80\x8B"   // U+F00B
#endif
#ifndef LV_SYMBOL_OK
#define LV_SYMBOL_OK            "\xEF\x80\x8C"   // U+F00C
#endif
#ifndef LV_SYMBOL_CLOSE
#define LV_SYMBOL_CLOSE         "\xEF\x80\x8D"   // U+F00D
#endif
#ifndef LV_SYMBOL_POWER
#define LV_SYMBOL_POWER         "\xEF\x80\x91"   // U+F011
#endif
#ifndef LV_SYMBOL_SETTINGS
#define LV_SYMBOL_SETTINGS      "\xEF\x80\x93"   // U+F013
#endif
#ifndef LV_SYMBOL_HOME
#define LV_SYMBOL_HOME          "\xEF\x80\x95"   // U+F015
#endif
#ifndef LV_SYMBOL_DOWNLOAD
#define LV_SYMBOL_DOWNLOAD      "\xEF\x80\x99"   // U+F019
#endif
#ifndef LV_SYMBOL_DRIVE
#define LV_SYMBOL_DRIVE         "\xEF\x80\x9C"   // U+F01C
#endif
#ifndef LV_SYMBOL_REFRESH
#define LV_SYMBOL_REFRESH       "\xEF\x80\xA1"   // U+F021
#endif
#ifndef LV_SYMBOL_MUTE
#define LV_SYMBOL_MUTE          "\xEF\x80\xA6"   // U+F026
#endif
#ifndef LV_SYMBOL_VOLUME_MID
#define LV_SYMBOL_VOLUME_MID    "\xEF\x80\xA7"   // U+F027
#endif
#ifndef LV_SYMBOL_VOLUME_MAX
#define LV_SYMBOL_VOLUME_MAX    "\xEF\x80\xA8"   // U+F028
#endif
#ifndef LV_SYMBOL_IMAGE
#define LV_SYMBOL_IMAGE         "\xEF\x80\xBE"   // U+F03E
#endif
#ifndef LV_SYMBOL_TINT
#define LV_SYMBOL_TINT          "\xEF\x81\x83"   // U+F043
#endif
#ifndef LV_SYMBOL_PREV
#define LV_SYMBOL_PREV          "\xEF\x81\x88"   // U+F048
#endif
#ifndef LV_SYMBOL_PLAY
#define LV_SYMBOL_PLAY          "\xEF\x81\x8B"   // U+F04B
#endif
#ifndef LV_SYMBOL_PAUSE
#define LV_SYMBOL_PAUSE         "\xEF\x81\x8C"   // U+F04C
#endif
#ifndef LV_SYMBOL_STOP
#define LV_SYMBOL_STOP          "\xEF\x81\x8D"   // U+F04D
#endif
#ifndef LV_SYMBOL_NEXT
#define LV_SYMBOL_NEXT          "\xEF\x81\x91"   // U+F051
#endif
#ifndef LV_SYMBOL_EJECT
#define LV_SYMBOL_EJECT         "\xEF\x81\x92"   // U+F052
#endif
#ifndef LV_SYMBOL_LEFT
#define LV_SYMBOL_LEFT          "\xEF\x81\x93"   // U+F053
#endif
#ifndef LV_SYMBOL_RIGHT
#define LV_SYMBOL_RIGHT         "\xEF\x81\x94"   // U+F054
#endif
#ifndef LV_SYMBOL_PLUS
#define LV_SYMBOL_PLUS          "\xEF\x81\xA7"   // U+F067
#endif
#ifndef LV_SYMBOL_MINUS
#define LV_SYMBOL_MINUS         "\xEF\x81\xA8"   // U+F068
#endif
#ifndef LV_SYMBOL_EYE_OPEN
#define LV_SYMBOL_EYE_OPEN      "\xEF\x81\xAE"   // U+F06E
#endif
#ifndef LV_SYMBOL_EYE_CLOSE
#define LV_SYMBOL_EYE_CLOSE     "\xEF\x81\xB0"   // U+F070
#endif
#ifndef LV_SYMBOL_WARNING
#define LV_SYMBOL_WARNING       "\xEF\x81\xB1"   // U+F071
#endif
#ifndef LV_SYMBOL_SHUFFLE
#define LV_SYMBOL_SHUFFLE       "\xEF\x81\xB4"   // U+F074
#endif
#ifndef LV_SYMBOL_UP
#define LV_SYMBOL_UP            "\xEF\x81\xB7"   // U+F077
#endif
#ifndef LV_SYMBOL_DOWN
#define LV_SYMBOL_DOWN          "\xEF\x81\xB8"   // U+F078
#endif
#ifndef LV_SYMBOL_LOOP
#define LV_SYMBOL_LOOP          "\xEF\x81\xB9"   // U+F079
#endif
#ifndef LV_SYMBOL_DIRECTORY
#define LV_SYMBOL_DIRECTORY     "\xEF\x81\xBB"   // U+F07B
#endif
#ifndef LV_SYMBOL_UPLOAD
#define LV_SYMBOL_UPLOAD        "\xEF\x82\x93"   // U+F093
#endif
#ifndef LV_SYMBOL_CALL
#define LV_SYMBOL_CALL          "\xEF\x82\x95"   // U+F095
#endif
#ifndef LV_SYMBOL_CUT
#define LV_SYMBOL_CUT           "\xEF\x83\x84"   // U+F0C4
#endif
#ifndef LV_SYMBOL_COPY
#define LV_SYMBOL_COPY          "\xEF\x83\x85"   // U+F0C5
#endif
#ifndef LV_SYMBOL_SAVE
#define LV_SYMBOL_SAVE          "\xEF\x83\x87"   // U+F0C7
#endif
#ifndef LV_SYMBOL_BARS
#define LV_SYMBOL_BARS          "\xEF\x83\x89"   // U+F0C9
#endif
#ifndef LV_SYMBOL_ENVELOPE
#define LV_SYMBOL_ENVELOPE      "\xEF\x83\xA0"   // U+F0E0
#endif
#ifndef LV_SYMBOL_CHARGE
#define LV_SYMBOL_CHARGE        "\xEF\x83\xA7"   // U+F0E7
#endif
#ifndef LV_SYMBOL_PASTE
#define LV_SYMBOL_PASTE         "\xEF\x83\xAA"   // U+F0EA
#endif
#ifndef LV_SYMBOL_BELL
#define LV_SYMBOL_BELL          "\xEF\x83\xB3"   // U+F0F3
#endif
#ifndef LV_SYMBOL_KEYBOARD
#define LV_SYMBOL_KEYBOARD      "\xEF\x84\x9C"   // U+F11C
#endif
#ifndef LV_SYMBOL_GPS
#define LV_SYMBOL_GPS           "\xEF\x84\xA4"   // U+F124
#endif
#ifndef LV_SYMBOL_FILE
#define LV_SYMBOL_FILE          "\xEF\x85\x9B"   // U+F15B
#endif
#ifndef LV_SYMBOL_WIFI
#define LV_SYMBOL_WIFI          "\xEF\x87\xAB"   // U+F1EB
#endif
#ifndef LV_SYMBOL_BATTERY_FULL
#define LV_SYMBOL_BATTERY_FULL  "\xEF\x89\x80"   // U+F240
#endif
#ifndef LV_SYMBOL_BATTERY_3
#define LV_SYMBOL_BATTERY_3     "\xEF\x89\x81"   // U+F241
#endif
#ifndef LV_SYMBOL_BATTERY_2
#define LV_SYMBOL_BATTERY_2     "\xEF\x89\x82"   // U+F242
#endif
#ifndef LV_SYMBOL_BATTERY_1
#define LV_SYMBOL_BATTERY_1     "\xEF\x89\x83"   // U+F243
#endif
#ifndef LV_SYMBOL_BATTERY_EMPTY
#define LV_SYMBOL_BATTERY_EMPTY "\xEF\x89\x84"   // U+F244
#endif
#ifndef LV_SYMBOL_USB
#define LV_SYMBOL_USB           "\xEF\x8A\x87"   // U+F287
#endif
#ifndef LV_SYMBOL_BLUETOOTH
#define LV_SYMBOL_BLUETOOTH     "\xEF\x8A\x93"   // U+F293
#endif
#ifndef LV_SYMBOL_TRASH
#define LV_SYMBOL_TRASH         "\xEF\x8B\xAD"   // U+F2ED
#endif
#ifndef LV_SYMBOL_EDIT
#define LV_SYMBOL_EDIT          "\xEF\x8C\x84"   // U+F304
#endif
#ifndef LV_SYMBOL_BACKSPACE
#define LV_SYMBOL_BACKSPACE     "\xEF\x95\x9A"   // U+F55A
#endif
#ifndef LV_SYMBOL_SD_CARD
#define LV_SYMBOL_SD_CARD       "\xEF\x9F\x82"   // U+F7C2
#endif
#ifndef LV_SYMBOL_NEW_LINE
#define LV_SYMBOL_NEW_LINE      "\xEF\xA2\xA2"   // U+F8A2
#endif

// ── Fonts ──────────────────────────────────────────────────────────────────
// A font is a sorted table of code points with 4-bit greyscale glyphs,
// run-length coded; see extras/fontgen/make_terminal_fonts.py for the format.
// Glyphs are CYDR_TERM_CELL_W wide, or twice that for double-width (CJK)
// characters.  A code point not in a font is looked up in its fallback.
struct cydr_font_t {
    uint8_t  cellW, cellH;
    uint16_t count;
    const uint16_t *codepoints;   // sorted, `count` entries
    const uint32_t *offsets;      // count + 1 entries; bit 31 = double width
    const uint8_t  *data;
    const cydr_font_t *fallback;
};

// The built-in font: Inconsolata (the open-source Consolas homage) for Latin,
// with accents and Greek; DejaVu Sans Mono for arrows, math, geometric shapes,
// dingbats and other symbols; box drawing and block elements drawn to meet at
// cell edges.  16 x 32 pixel cells.  About 140 KB of flash.
extern const cydr_font_t cydr_font_mono16;

// OPT-IN double-width fonts for Chinese, Japanese and Korean, each in its own
// header so a sketch pays only for what it includes.  Each falls back to
// cydr_font_mono16 for everything else, so it replaces it in setFont():
//   cydr_font_mono16_zh.h   Chinese (GB2312 level-1 hanzi)        ~1.2 MB flash
//   cydr_font_mono16_ja.h   Japanese (JIS level-1 kanji, kana,
//                           half-width katakana)                  ~1.0 MB flash
//   cydr_font_mono16_ja_kana.h  Japanese without kanji (kana,
//                           half-width katakana)                  ~66 KB flash
//   cydr_font_mono16_ko.h   Korean (KS X 1001 Hangul, half-width
//                           jamo)                                 ~0.6 MB flash
//   cydr_font_mono16_cjk.h  all three (Chinese glyph forms)       ~2.2 MB flash
// Half-width forms take one cell; the rest of these fonts, two.
// On a 4 MB board anything but Korean or kana-only Japanese needs a
// partition scheme with a 3 MB app slot (PartitionScheme=huge_app).  See the TerminalCJK example.

// ── Colors (RGB565) ────────────────────────────────────────────────────────
static constexpr uint16_t cydr_rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
static constexpr uint16_t CYDR_BLACK   = 0x0000;
static constexpr uint16_t CYDR_WHITE   = 0xFFFF;
static constexpr uint16_t CYDR_GREY    = cydr_rgb565(128, 128, 128);
static constexpr uint16_t CYDR_RED     = 0xF800;
static constexpr uint16_t CYDR_GREEN   = 0x07E0;
static constexpr uint16_t CYDR_BLUE    = 0x001F;
static constexpr uint16_t CYDR_YELLOW  = 0xFFE0;
static constexpr uint16_t CYDR_CYAN    = 0x07FF;
static constexpr uint16_t CYDR_MAGENTA = 0xF81F;
static constexpr uint16_t CYDR_ORANGE  = cydr_rgb565(255, 165, 0);

enum cydr_cursor_t : uint8_t {
    CYDR_CURSOR_OFF = 0,
    CYDR_CURSOR_BLOCK,       // the whole cell, solid in the text color
    CYDR_CURSOR_UNDERLINE,   // a bar along the bottom of the cell
    CYDR_CURSOR_BAR,         // a thin vertical line at the cell's left edge (a caret)
};

// What a scroll step does about the rows it rewrites.  See setScrollMode().
enum cydr_scroll_mode_t : uint8_t {
    CYDR_SCROLL_AUTO = 0,    // fast while text pours in, clean for a lone line
    CYDR_SCROLL_FAST,        // always fast: a just-wrapped row may flash briefly
    CYDR_SCROLL_CLEAN,       // always clean: no flash; half the speed
};

enum cydr_touch_units_t : uint8_t {
    CYDR_TOUCH_CELLS = 0,    // column / row; touches on frozen areas are ignored
    CYDR_TOUCH_PIXELS,       // absolute screen pixels, frozen areas included
};

// The formatter behind printf(), separate from the terminal so it can be
// tested against the C library's own printf.  Output goes to the sink:
// bytes() for text, codepoint() for %lc / %ls characters; wide() says whether
// a character takes two cells (for padding).
struct cydr_fmt_sink {
    void (*bytes)(void *ctx, const char *s, size_t n);
    void (*codepoint)(void *ctx, uint32_t cp);
    bool (*wide)(void *ctx, uint16_t cp);
    void *ctx;
};
size_t cydr_term_vformat(const cydr_fmt_sink &sink, const char *fmt, va_list ap);

class chipguy_CYD_resistive_terminal : public Print {
public:
    chipguy_CYD_resistive_terminal() {}

    // Start the terminal on `display`, beginning the display first if the
    // sketch hasn't.  rotation: 0 = portrait, 2 = portrait upside down (also
    // 0 / 180 degrees).  1 and 3 (landscape) are refused -- hardware scrolling
    // only runs along the panel's long axis; use LVGL for landscape.  The
    // screen is cleared to black with white text and the cursor at (0, 0).
    // Returns false if the display could not be started.
    bool begin(chipguy_CYD_resistive_display &display, uint16_t rotation = 0,
               const cydr_font_t *font = &cydr_font_mono16);

    // ── Text ──────────────────────────────────────────────────────────────
    // Print's print() / println() / write() all arrive here.
    size_t write(uint8_t b) override;
    size_t write(const uint8_t *buf, size_t n) override;
    using Print::write;

    // One byte, as write(); UTF-8 sequences may be split across calls.
    int putc(int c) { write((uint8_t)c); return c; }
    // One Unicode code point (U+0000..U+FFFF).
    void putCodepoint(uint32_t cp);

    // printf without the heap.  %s / %ls strings are copied straight to the
    // screen, with width and precision applied here (width counts screen
    // cells, so a CJK character counts 2).  Numbers go through snprintf one at
    // a time into a CYDR_TERM_PRINTF_NUMBUF buffer on the caller's stack, and
    // their width padding is done here too.  %lc prints a code point.  %n is
    // accepted and ignored.  Returns the number of bytes sent to the screen.
    size_t printf(const char *fmt, ...) __attribute__((format(printf, 2, 3)));
    size_t vprintf(const char *fmt, va_list ap);

    // Clear the scrolling area to the current background color and put the
    // cursor at (0, 0).  Returns at once; the task clears the panel by DMA.
    // includeFrozen also clears the frozen header / footer areas.
    void clear(bool includeFrozen = false);
    // Clear from the cursor to the end of its line (same as '\v').
    void clearToEndOfLine();

    // Block until everything printed so far is on the panel.
    void flush() override;

    // ── Cursor and colors ─────────────────────────────────────────────────
    // Column and row in cells.  Row 0 is the top line on screen.  Out-of-range
    // values are clamped.
    void setCursor(int16_t col, int16_t row);
    int16_t cursorCol() const;
    int16_t cursorRow() const;

    void setTextColor(uint16_t fg) { _fg = fg; }
    void setTextColor(uint16_t fg, uint16_t bg) { _fg = fg; _bg = bg; }
    uint16_t textColor() const { return _fg; }
    uint16_t backgroundColor() const { return _bg; }

    // Blinking cursor: off, block, underline or bar.  The task blinks it, in
    // the current text color, alternating with what the cell shows.  Over
    // text the terminal no longer holds (earlier lines, after setCursor()),
    // the task reads the covered pixels back from the panel first, to put
    // them back; on a panel that can't be read back the cursor hides there.
    void setCursorStyle(cydr_cursor_t style);

    // Whether the panel passed the start-up readback check (see above).
    bool readbackAvailable() const { return _rbOK; }

    // What filling the last cell of a line does.  false (the default): the
    // cursor waits just past the right edge, and moves to the next line only
    // when something more is printed -- so a line exactly as wide as the
    // screen followed by '\n' or "\r\n" takes one line, not two, and a full
    // bottom line doesn't scroll until it has to.  '\r' while waiting returns
    // to the start of the same line.  true: move to the start of the next
    // line at once (scrolling if it was the bottom line).
    void setImmediateWrap(bool wrapNow) { _immediateWrap = wrapNow; }

    // Scrolling moves the panel's start line and then rewrites the rows that
    // wrapped round from the top to the bottom edge.  For the moment between
    // the two, those rows still show what they held at the top.
    //   fast: accept that.  Two rows per step.  When the panel happens to
    //     refresh mid-step, a row or two flashes for a fraction of a frame.
    //   clean: avoid it.  One row per step, blanked while still at the top
    //     edge before it moves (that top row going blank is the only sign).
    //     About half the speed of fast.
    //   CYDR_SCROLL_AUTO (default): fast while text is pouring in -- the last
    //     four line scrolls all within 0.4 s, or more than a line queued --
    //     when the whole screen is in motion and a flash can't be seen; clean
    //     when a line scrolls on its own, when it could and speed doesn't
    //     matter.
    void setScrollMode(cydr_scroll_mode_t mode) { _scrollMode = mode; }

    // Pixel rows scrolled since begin() (wraps at 2^32): for measuring.
    uint32_t pixelsScrolled() const { return _scrolled; }

    // ── Geometry ──────────────────────────────────────────────────────────
    int16_t columns() const { return _cols; }
    int16_t rows() const;                 // rows on screen, a partial one included
    int16_t width() const { return _W; }  // panel pixels
    int16_t height() const { return _H; }
    uint8_t cellWidth() const { return CYDR_TERM_CELL_W; }
    uint8_t cellHeight() const { return _CH; }

    // Freeze `topPixels` at the top and `bottomPixels` at the bottom of the
    // screen: they stop scrolling, so a header or footer drawn there with
    // drawBitmap() stays put while text scrolls between them.  freeze(0, 0)
    // unfreezes.  At least one text line must remain.  The cursor is moved
    // into the scrolling area if it is no longer inside it.  Text already on
    // screen keeps its place where it can; where it can't (lines that crossed
    // the old area's wrap point, or that are now under a frozen area), what
    // shows is unspecified until it is printed over or cleared.  Waits for
    // pending output first.  Returns false if the sizes don't fit.
    bool freeze(uint16_t topPixels, uint16_t bottomPixels);
    uint16_t frozenTop() const { return _fixT; }
    uint16_t frozenBottom() const { return _fixB; }

    // After scrolling, or a freeze() that isn't a multiple of the cell height,
    // the top line on screen can be partly cut off.  This scrolls forward just
    // far enough to push it off.  backward = true instead scrolls back just far
    // enough to push the partial line at the bottom off: blank space comes in
    // at the top, and the cursor moves up a line if it was on that bottom line.
    void evictPartialLine(bool backward = false);

    // ── Graphics ──────────────────────────────────────────────────────────
    // Draw a w x h RGB565 image (one uint16_t per pixel, row by row, in the
    // usual native order: 0xF800 is red) at absolute screen position (x, y).
    // Clipped to the screen.  Anywhere in the frozen areas it is drawn between
    // scroll steps; overlapping the scrolling area it first waits for pending
    // output, then lands where (x, y) is on screen now, and scrolls with the
    // text from then on.  Blocks until drawn, so `pixels` may be on the
    // caller's stack.
    void drawBitmap(int16_t x, int16_t y, int16_t w, int16_t h, const uint16_t *pixels);

    // ── Touch ─────────────────────────────────────────────────────────────
    // Hand over a begun touch driver; its rotation is matched to the terminal.
    void attachTouch(chipguy_CYD_resistive_touch &touch);
    // Read the touch screen.  CYDR_TOUCH_CELLS (default): x = column, y = row,
    // in the same terms as setCursor(); returns false for no touch and for
    // touches on the frozen areas.  CYDR_TOUCH_PIXELS: absolute screen pixels.
    bool pollTouch(int16_t &x, int16_t &y, cydr_touch_units_t units = CYDR_TOUCH_CELLS);

    // ── Misc ──────────────────────────────────────────────────────────────
    // Change font.  It must have the same cell size; text already on screen
    // keeps the old glyphs.  Returns false if the cell size differs.
    bool setFont(const cydr_font_t *font);

    // Bytes of the task's stack never used so far (for tuning
    // CYDR_TERM_STACK_BYTES).
    uint32_t stackHighWater() const;


private:
    struct Cell { uint16_t ch, fg, bg; };
    static constexpr uint16_t CELL_KEEP   = 0xFFFF;   // unknown: leave the panel alone
    static constexpr uint16_t CELL_WIDE_R = 0xFFFE;   // right half of the cell to its left

    struct BlitReq {
        int16_t x0, y0, x1, y1;     // clipped, inclusive-exclusive (x1, y1 exclusive)
        int16_t ox, oy, w;          // the image's unclipped origin and stride
        const uint16_t *px;
    };

    // ── Static storage ────────────────────────────────────────────────────
    Cell _cells[CYDR_TERM_MAX_COLS];                       // the one line of cells
    // The two-row DMA buffer.  The rows sit back to back at the panel's own
    // width, so both go out as one transfer: use _half(), not fixed offsets.
    alignas(4) uint16_t _dma[2 * CYDR_TERM_MAX_WIDTH];
    uint16_t *_half(int i) { return _dma + i * _W; }
    StackType_t  _stack[CYDR_TERM_STACK_BYTES / sizeof(StackType_t)];
    StaticTask_t _tcb;
    StaticSemaphore_t _mutexBuf, _waitBuf;

    // ── Fixed after begin() ───────────────────────────────────────────────
    chipguy_CYD_resistive_display *_display = nullptr;
    chipguy_CYD_resistive_touch   *_touch = nullptr;
    spi_device_handle_t _dev = nullptr;
    TaskHandle_t _task = nullptr;
    SemaphoreHandle_t _mutex = nullptr;     // serializes callers
    SemaphoreHandle_t _waitSem = nullptr;   // task -> waiting caller
    const cydr_font_t *_font = nullptr;
    int16_t _W = 0, _H = 0, _cols = 0;
    uint8_t _CH = 32;
    bool _rot2 = false;
    bool _rbOK = false;                 // the panel reads back
    uint8_t _rbBpp = 3;                 // bytes per pixel it reads back in
    uint32_t _wrHz = 0;                 // the device's own (write) clock

    // ── Shared state, guarded by _mux ─────────────────────────────────────
    // Positions in the scrolling area are "virtual" pixel rows: an ever-
    // increasing coordinate whose value mod _VSA is the row of the panel's
    // frame memory (inside the scroll area) that holds it.
    portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;
    int16_t _fixT = 0, _fixB = 0, _VSA = 0;   // frozen top, frozen bottom, scroll area height
    int32_t _vtop = 0;                  // virtual row at the top of the scroll area, as on the panel
    int32_t _vtopT = 0;                 // ... once queued scrolling is done
    int32_t _grid = 0;                  // text lines start at virtual rows == _grid (mod _CH)
    int32_t _lineTop = 0;               // the line the cell array holds
    uint32_t _lineGen = 0;              // bumped whenever the cell array changes lines
    uint32_t _pending = 0;              // cells changed since the task drew them
    bool _taskOnLine = false;           // task is reading the cell array right now
    bool _swPending = false;            // cell array must move to _swTop once drawn
    bool _swFresh = false;
    int32_t _swTop = 0;
    int32_t _blankFrom = 0;             // lines at or below this are known blank
    int32_t _curTop = 0;                // cursor line
    int16_t _curCol = 0;                // == _cols: parked past the right edge
    uint16_t _screenBg = CYDR_BLACK;
    bool _clearReq = false, _clearAll = false;
    uint16_t _clearColor = CYDR_BLACK;
    bool _cfgReq = false;
    BlitReq *volatile _blit = nullptr;
    volatile bool _waiting = false;
    cydr_cursor_t _cursorStyle = CYDR_CURSOR_BLOCK;
    bool _blinkOn = true;
    TickType_t _blinkAt = 0;
    TickType_t _scrollTicks[4] = {};    // when the last four line scrolls began
    uint8_t _scrollTickIdx = 0;

    // ── Caller-side only ──────────────────────────────────────────────────
    uint16_t _fg = CYDR_WHITE, _bg = CYDR_BLACK;
    bool _immediateWrap = false;
    volatile cydr_scroll_mode_t _scrollMode = CYDR_SCROLL_AUTO;
    uint32_t _u8cp = 0;
    uint8_t _u8need = 0;

    // ── Task-side only ────────────────────────────────────────────────────
    spi_transaction_t _trans = {};
    bool _dmaBusy = false;
    int16_t _casetX0 = -1, _casetX1 = -1;
    uint16_t _palFg = 0, _palBg = 0;
    bool _palValid = false;
    uint16_t _pal[16];
    uint16_t _blankColor = 0;
    bool _blankValid = false;
    volatile uint32_t _scrolled = 0;
    // The cursor over a cell the terminal doesn't hold: the pixels it covers
    // are read back into the start of _dma and written back to take it off.
    // A cell on the frame-memory wrap point is two rects: rows _ghY[0] ..
    // at the bottom of the scroll area, then _ghY[1] .. at its top.
    bool _ghSaved = false;              // _dma holds the rects' pixels
    bool _ghShown = false;              // the cursor is on the panel there
    int16_t _ghX = 0, _ghW = 0;
    int16_t _ghY[2] = {}, _ghH[2] = {}; // panel rows; _ghH[1] == 0: one rect
    uint16_t _ghColor = 0;

    // caller side
    void _feedByte(uint8_t b);
    void _putCp(uint32_t cp);
    void _putGlyph(uint16_t cp);
    void _newline();
    void _clearEolLocked();
    void _waitSwitch();
    void _waitTask();
    void _kick();
    bool _idleLocked() const;
    bool _lineDoneLocked() const;
    void _requestSwitchLocked(int32_t top, bool fresh);
    void _doSwitchLocked();
    void _setCursorLocked(int32_t top, int16_t col);
    void _markCursorLocked();
    int32_t _lineFloor(int32_t v) const;
    int32_t _ring(int32_t v) const;
    bool _isWide(uint16_t cp) const;
    const uint8_t *_glyph(uint16_t cp, uint32_t &len, bool &wide) const;

    // task side
    static void _taskEntry(void *arg);
    void _taskLoop();
    bool _step();
    bool _doConfig();
    bool _doClear();
    bool _doBlit();
    bool _renderPending();
    bool _trySwitch();
    bool _scrollStep();
    bool _ghostUpdate();
    void _ghostLift();
    bool _readbackCheck();
    void _ghostWrite();
    void _readRect(int x, int y, int w, int h, uint16_t *out);
    void _fillRect(int x, int y, int w, int h, uint16_t color, uint16_t *spare);
    void _renderRuns(uint32_t bits, uint32_t gen, int32_t top, int py0, int py1, bool pickup);
    void _renderRect(int c0, int c1, uint32_t gen, int32_t top, int py0, int py1, bool pickup);
    int  _renderRow(int c0, int c1, int py, uint16_t *out);
    int  _renderCell(int c, int py, uint16_t *out);
    void _alignSpan(int &c0, int &c1) const;
    void _palette(uint16_t fg, uint16_t bg);
    void _fillBlank(uint16_t color);
    void _busBegin();
    void _busEnd();
    void _cmd(uint8_t cmd, const uint8_t *data, int n);
    void _window(int x0, int x1, int y0, int y1, bool read = false);
    void _queue(const uint16_t *buf, int bytes);
    void _waitDma();
    void _sendScrollDef(int16_t T, int16_t VSA, int16_t B);
    void _sendVsp(int32_t vtop, int16_t T, int16_t VSA, int16_t B);
};
