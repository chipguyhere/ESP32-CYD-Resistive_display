/*
 * Console-style text terminal with hardware scrolling for the resistive-touch
 * CYD family.  See chipguy_CYD_resistive_terminal.h for the API and
 * README-terminal.md / README-engineering.md for how it works.
 *
 * Two sides share the state below:
 *   - the CALLER (print / printf / setCursor ...) writes characters into the
 *     one-line cell array, moves the cursor, and queues work: a line switch,
 *     a scroll target, a clear, a blit;
 *   - the TASK owns the SPI device.  It draws changed cells, scrolls one pixel
 *     row per step, moves the cell array to the next line once the old one is
 *     fully drawn, clears, blits and blinks the cursor.
 * Shared fields change only under _mux, and only for a few dozen
 * instructions; nothing waits while holding it.  The task reads the cell
 * array without the lock: a cell the caller changes mid-read has its
 * _pending bit set again, so a torn read is redrawn moments later.
 */

#include "chipguy_CYD_resistive_terminal.h"

#define CW CYDR_TERM_CELL_W
static constexpr TickType_t BLINK_TICKS = pdMS_TO_TICKS(530);
// CYDR_SCROLL_AUTO goes fast when the last four line scrolls all began
// within this long (about ten lines a second or more).
static constexpr TickType_t RAPID_TICKS = pdMS_TO_TICKS(400);

// Panel commands for vertical scrolling (the same on all three controllers).
#define CMD_CASET    0x2A
#define CMD_RASET    0x2B
#define CMD_RAMWR    0x2C
#define CMD_VSCRDEF  0x33
#define CMD_VSCRSADD 0x37

static inline int32_t pmod(int32_t a, int32_t m) {
    int32_t r = a % m;
    return r < 0 ? r + m : r;
}

int32_t chipguy_CYD_resistive_terminal::_ring(int32_t v) const { return pmod(v, _VSA); }

// Top of the text line containing virtual row v.
int32_t chipguy_CYD_resistive_terminal::_lineFloor(int32_t v) const {
    return v - pmod(v - _grid, _CH);
}

// ── Fonts ──────────────────────────────────────────────────────────────────

const uint8_t *chipguy_CYD_resistive_terminal::_glyph(uint16_t cp, uint32_t &len,
                                                      bool &wide) const {
    for (int pass = 0; pass < 2; pass++) {
        for (const cydr_font_t *f = _font; f; f = f->fallback) {
            int lo = 0, hi = (int)f->count - 1;
            while (lo <= hi) {
                int mid = (lo + hi) >> 1;
                uint16_t c = f->codepoints[mid];
                if (c == cp) {
                    uint32_t a = f->offsets[mid], b = f->offsets[mid + 1];
                    wide = (a & 0x80000000u) != 0;
                    a &= 0x7FFFFFFFu;
                    b &= 0x7FFFFFFFu;
                    len = b - a;
                    return f->data + a;
                }
                if (c < cp) lo = mid + 1; else hi = mid - 1;
            }
        }
        cp = 0xFFFD;        // missing: the replacement character
    }
    len = 0;
    wide = false;
    return nullptr;
}

bool chipguy_CYD_resistive_terminal::_isWide(uint16_t cp) const {
    uint32_t len; bool wide;
    _glyph(cp, len, wide);
    return wide;
}

// Decode pixel row `py` of a glyph `w` pixels wide into `out` through the
// 16-entry palette.  See make_terminal_fonts.py for the token format.
static void IRAM_ATTR decodeRow(const uint8_t *p, const uint8_t *end, int w, int py,
                                const uint16_t *pal, uint16_t *out) {
    int skip = py * w, n = w;
    while (n > 0) {
        if (p >= end) {                     // the rest is background
            while (n--) *out++ = pal[0];
            return;
        }
        uint8_t t = *p++;
        if (t < 0x80) {
            int len = (t & 0x3F) + 1;
            if (skip >= len) { skip -= len; continue; }
            len -= skip;
            skip = 0;
            if (len > n) len = n;
            n -= len;
            uint16_t c = pal[t < 0x40 ? 0 : 15];
            while (len--) *out++ = c;
        } else {
            int len = (t & 0x7F) + 1;
            const uint8_t *q = p;
            p += (len + 1) >> 1;
            if (skip >= len) { skip -= len; continue; }
            int i = skip;
            skip = 0;
            for (; i < len && n > 0; i++, n--) {
                uint8_t b = q[i >> 1];
                *out++ = pal[(i & 1) ? (b & 0x0F) : (b >> 4)];
            }
        }
    }
}

// 16 blends from bg (0) to fg (15), byte-swapped for the panel.
void chipguy_CYD_resistive_terminal::_palette(uint16_t fg, uint16_t bg) {
    if (_palValid && fg == _palFg && bg == _palBg) return;
    int fr = fg >> 11, fgn = (fg >> 5) & 0x3F, fb = fg & 0x1F;
    int br = bg >> 11, bgn = (bg >> 5) & 0x3F, bb = bg & 0x1F;
    for (int a = 0; a < 16; a++) {
        int r = (br * (15 - a) + fr * a + 7) / 15;
        int g = (bgn * (15 - a) + fgn * a + 7) / 15;
        int b = (bb * (15 - a) + fb * a + 7) / 15;
        _pal[a] = __builtin_bswap16((uint16_t)((r << 11) | (g << 5) | b));
    }
    _palFg = fg;
    _palBg = bg;
    _palValid = true;
}

// ── begin ──────────────────────────────────────────────────────────────────

bool chipguy_CYD_resistive_terminal::begin(chipguy_CYD_resistive_display &display,
                                           uint16_t rotation, const cydr_font_t *font) {
    if (_task) return true;
    uint8_t rot = cydr_normalize_rotation(rotation);
    if (rot == 1 || rot == 3) {
        // Hardware scrolling runs along the panel's long axis only.  Landscape
        // would scroll sideways: use LVGL for a rotated display.
        Serial.println("cydr terminal: landscape rotation not supported "
                       "(hardware scrolling is portrait-only); using portrait. "
                       "Use LVGL for a landscape display.");
        rot = 0;
    }
    if (!display._spi) {
        if (!display.begin(rot)) return false;
    } else {
        display.setRotation(rot);
    }
    _display = &display;
    _dev = display._spi;
    _W = display.width();
    _H = display.height();
    if (!font || font->cellW != CW || _W > CYDR_TERM_MAX_WIDTH) {
        Serial.println("cydr terminal: unsupported font or panel width");
        return false;
    }
    _font = font;
    _CH = font->cellH;
    _cols = _W / CW;
    _rot2 = rot == 2;

    _fixT = _fixB = 0;
    _VSA = _H;
    _vtop = _vtopT = _grid = 0;
    _lineTop = _curTop = 0;
    _curCol = 0;
    for (int c = 0; c < CYDR_TERM_MAX_COLS; c++) _cells[c] = {' ', _fg, _bg};
    _pending = 0;
    _blankFrom = _CH;
    _screenBg = _bg;
    _blinkAt = xTaskGetTickCount() + BLINK_TICKS;
    _cfgReq = true;
    _clearReq = true;
    _clearAll = true;
    _clearColor = _bg;

    _mutex = xSemaphoreCreateRecursiveMutexStatic(&_mutexBuf);
    _waitSem = xSemaphoreCreateBinaryStatic(&_waitBuf);
    _task = xTaskCreateStaticPinnedToCore(_taskEntry, "cydr_term",
                                          sizeof(_stack),   // bytes, in ESP-IDF's FreeRTOS
                                          this, CYDR_TERM_TASK_PRIORITY, _stack, &_tcb,
                                          CYDR_TERM_TASK_CORE);
    if (!_task) return false;
    flush();
    return true;
}

uint32_t chipguy_CYD_resistive_terminal::stackHighWater() const {
    return _task ? uxTaskGetStackHighWaterMark(_task) * sizeof(StackType_t) : 0;
}

bool chipguy_CYD_resistive_terminal::setFont(const cydr_font_t *font) {
    if (!font || font->cellW != CW || font->cellH != _CH) return false;
    if (!_task) { _font = font; return true; }
    xSemaphoreTakeRecursive(_mutex, portMAX_DELAY);
    flush();
    portENTER_CRITICAL(&_mux);
    _font = font;
    portEXIT_CRITICAL(&_mux);
    xSemaphoreGiveRecursive(_mutex);
    return true;
}

// ── Caller-side helpers ────────────────────────────────────────────────────

void chipguy_CYD_resistive_terminal::_kick() {
    if (_task) xTaskNotifyGive(_task);
}

// Wait for the task to finish a step.  The timeout only guards a missed
// wake-up; the caller re-checks its condition either way.
void chipguy_CYD_resistive_terminal::_waitTask() {
    _waiting = true;
    _kick();
    xSemaphoreTake(_waitSem, pdMS_TO_TICKS(20));
}

bool chipguy_CYD_resistive_terminal::_idleLocked() const {
    return !_pending && !_swPending && _vtop == _vtopT && !_clearReq && !_cfgReq &&
           !_blit && !_taskOnLine;
}

// The cell line has nothing left to draw: no changed cells, and every row of
// it that will be on screen once scrolling finishes is already on screen (the
// scroll draws rows from the cell array as they come into view).
bool chipguy_CYD_resistive_terminal::_lineDoneLocked() const {
    if (_pending || _clearReq || _cfgReq) return false;
    int32_t a = max(_lineTop, _vtopT), b = min(_lineTop + _CH, _vtopT + _VSA);
    if (a >= b) return true;
    return a >= _vtop && b <= _vtop + _VSA;
}

// Mark the cursor's cell for redraw, if the cell array holds the cursor's line.
void chipguy_CYD_resistive_terminal::_markCursorLocked() {
    if (_cursorStyle == CYDR_CURSOR_OFF || _curTop != _lineTop || _curCol >= _cols) return;
    if (_cells[_curCol].ch == CELL_KEEP) return;
    _pending |= 1u << _curCol;
}

void chipguy_CYD_resistive_terminal::_setCursorLocked(int32_t top, int16_t col) {
    if (top == _curTop && col == _curCol) return;
    _markCursorLocked();              // erase it where it was
    _curTop = top;
    _curCol = col;
    _blinkOn = true;
    _blinkAt = xTaskGetTickCount() + BLINK_TICKS;
    _markCursorLocked();              // and show it where it is
}

// Point the cell array at the line at `top` -- now if the old line is fully
// drawn, else by the task once it is.  fresh: the line is coming into view by
// scrolling and starts blank.
void chipguy_CYD_resistive_terminal::_requestSwitchLocked(int32_t top, bool fresh) {
    if (top == _lineTop && !fresh) return;
    _swTop = top;
    _swFresh = fresh;
    _swPending = true;
    if (_lineDoneLocked() && !_taskOnLine) _doSwitchLocked();
}

void chipguy_CYD_resistive_terminal::_doSwitchLocked() {
    int32_t top = _swTop;
    bool blank = _swFresh || top >= _blankFrom;
    _lineTop = top;
    _lineGen++;
    _swPending = false;
    _pending = 0;
    if (blank) {
        for (int c = 0; c < _cols; c++) _cells[c] = {' ', _fg, _screenBg};
        // Draw the blanks over any part already on screen that isn't known blank.
        bool visible = top < _vtop + _VSA && top + _CH > _vtop;
        if (_swFresh && top < _blankFrom && visible) _pending = (1u << _cols) - 1;
        if (_blankFrom < top + _CH) _blankFrom = top + _CH;
    } else {
        for (int c = 0; c < _cols; c++) _cells[c] = {CELL_KEEP, _fg, _bg};
    }
    _markCursorLocked();
}

// Wait until the cell array holds the cursor's line.
void chipguy_CYD_resistive_terminal::_waitSwitch() {
    for (;;) {
        portENTER_CRITICAL(&_mux);
        bool p = _swPending;
        portEXIT_CRITICAL(&_mux);
        if (!p) return;
        _waitTask();
    }
}

void chipguy_CYD_resistive_terminal::flush() {
    if (!_task) return;
    xSemaphoreTakeRecursive(_mutex, portMAX_DELAY);
    for (;;) {
        portENTER_CRITICAL(&_mux);
        bool idle = _idleLocked();
        portEXIT_CRITICAL(&_mux);
        if (idle) break;
        _waitTask();
    }
    xSemaphoreGiveRecursive(_mutex);
}

// ── Text input ─────────────────────────────────────────────────────────────

size_t chipguy_CYD_resistive_terminal::write(uint8_t b) {
    return write(&b, 1);
}

size_t chipguy_CYD_resistive_terminal::write(const uint8_t *buf, size_t n) {
    if (!_task) return 0;
    xSemaphoreTakeRecursive(_mutex, portMAX_DELAY);
    for (size_t i = 0; i < n; i++) _feedByte(buf[i]);
    _kick();
    xSemaphoreGiveRecursive(_mutex);
    return n;
}

void chipguy_CYD_resistive_terminal::putCodepoint(uint32_t cp) {
    if (!_task) return;
    xSemaphoreTakeRecursive(_mutex, portMAX_DELAY);
    _u8need = 0;
    _putCp(cp);
    _kick();
    xSemaphoreGiveRecursive(_mutex);
}

// UTF-8 decoder.  A malformed sequence prints U+FFFD.
void chipguy_CYD_resistive_terminal::_feedByte(uint8_t b) {
    if (_u8need) {
        if ((b & 0xC0) == 0x80) {
            _u8cp = (_u8cp << 6) | (b & 0x3F);
            if (--_u8need == 0) _putCp(_u8cp);
            return;
        }
        _u8need = 0;
        _putCp(0xFFFD);           // truncated sequence; b starts afresh
    }
    if (b < 0x80)                { _putCp(b); }
    else if ((b & 0xE0) == 0xC0) { _u8cp = b & 0x1F; _u8need = 1; }
    else if ((b & 0xF0) == 0xE0) { _u8cp = b & 0x0F; _u8need = 2; }
    else if ((b & 0xF8) == 0xF0) { _u8cp = b & 0x07; _u8need = 3; }
    else                         { _putCp(0xFFFD); }
}

void chipguy_CYD_resistive_terminal::_putCp(uint32_t cp) {
    switch (cp) {
        case '\n': _newline(); return;
        case '\r':
            portENTER_CRITICAL(&_mux);
            _setCursorLocked(_curTop, 0);
            portEXIT_CRITICAL(&_mux);
            return;
        case '\b':
            portENTER_CRITICAL(&_mux);
            if (_curCol > 0) _setCursorLocked(_curTop, min<int16_t>(_curCol, _cols) - 1);
            portEXIT_CRITICAL(&_mux);
            return;
        case '\t':
            // Destructive: spaces to the next multiple of 4, stopping at a wrap.
            if (_curCol >= _cols) { _newline(); return; }   // parked: column 0 is a stop
            do {
                _putGlyph(' ');
            } while (_curCol % 4 != 0 && _curCol < _cols);
            return;
        case '\v': clearToEndOfLine(); return;
        case '\f': clear(); return;
    }
    if (cp < 0x20 || cp == 0x7F) return;
    if (cp > 0xFFFD) cp = 0xFFFD;
    _putGlyph((uint16_t)cp);
}

void chipguy_CYD_resistive_terminal::_putGlyph(uint16_t cp) {
    bool wide = _isWide(cp) && _cols >= 2;
    int need = wide ? 2 : 1;
    if (_curCol >= _cols) _newline();                // parked past the edge
    if (_curCol + need > _cols) _newline();          // a wide glyph that won't fit
    _waitSwitch();

    portENTER_CRITICAL(&_mux);
    int c = _curCol;
    // Don't leave half of a wide character behind on either side.
    if (_cells[c].ch == CELL_WIDE_R && c > 0) {
        _cells[c - 1].ch = ' ';
        _pending |= 1u << (c - 1);
    }
    if (c + need < _cols && _cells[c + need].ch == CELL_WIDE_R) {
        _cells[c + need].ch = ' ';
        _pending |= 1u << (c + need);
    }
    _cells[c] = {cp, _fg, _bg};
    _pending |= 1u << c;
    if (wide) {
        _cells[c + 1] = {CELL_WIDE_R, _fg, _bg};
        _pending |= 1u << (c + 1);
    }
    int16_t next = c + need;
    bool wrap = false;
    if (next >= _cols) {
        // Last cell filled: park just past the edge, so a '\n' (or "\r\n")
        // that follows doesn't leave a blank line; the next printable
        // character wraps.  Or wrap now, if asked to.
        if (_immediateWrap) wrap = true;
        else _setCursorLocked(_curTop, _cols);
    } else {
        _setCursorLocked(_curTop, next);
    }
    portEXIT_CRITICAL(&_mux);
    if (wrap) _newline();
}

void chipguy_CYD_resistive_terminal::_newline() {
    _waitSwitch();
    portENTER_CRITICAL(&_mux);
    int32_t top = _curTop + _CH;
    bool fresh = false;
    if (top + _CH > _vtopT + _VSA) {
        _vtopT = top + _CH - _VSA;
        fresh = true;
        _scrollTicks[_scrollTickIdx++ & 3] = xTaskGetTickCount();
    }
    _setCursorLocked(top, 0);
    _requestSwitchLocked(top, fresh);
    portEXIT_CRITICAL(&_mux);
    _kick();
}

void chipguy_CYD_resistive_terminal::_clearEolLocked() {
    if (_curCol >= _cols) return;
    // A wide character whose right half is about to be cleared loses its left half too.
    if (_curCol > 0 && _cells[_curCol].ch == CELL_WIDE_R) {
        _cells[_curCol - 1].ch = ' ';
        _pending |= 1u << (_curCol - 1);
    }
    for (int c = _curCol; c < _cols; c++) {
        _cells[c] = {' ', _fg, _bg};
        _pending |= 1u << c;
    }
}

void chipguy_CYD_resistive_terminal::clearToEndOfLine() {
    if (!_task) return;
    xSemaphoreTakeRecursive(_mutex, portMAX_DELAY);
    _waitSwitch();
    portENTER_CRITICAL(&_mux);
    _clearEolLocked();
    portEXIT_CRITICAL(&_mux);
    _kick();
    xSemaphoreGiveRecursive(_mutex);
}

void chipguy_CYD_resistive_terminal::clear(bool includeFrozen) {
    if (!_task) return;
    xSemaphoreTakeRecursive(_mutex, portMAX_DELAY);
    portENTER_CRITICAL(&_mux);
    _clearReq = true;
    _clearAll = includeFrozen;
    _clearColor = _bg;
    _screenBg = _bg;
    _vtopT = 0;               // the task resets the panel's scroll to match
    _grid = 0;
    _curTop = 0;
    _curCol = 0;
    // The cell array moves to line 0 now, whatever it held: the clear covers it.
    _lineTop = 0;
    _lineGen++;
    _swPending = false;
    for (int c = 0; c < _cols; c++) _cells[c] = {' ', _fg, _bg};
    _pending = 0;
    _blankFrom = _CH;
    _blinkOn = true;
    _blinkAt = xTaskGetTickCount() + BLINK_TICKS;
    _markCursorLocked();
    portEXIT_CRITICAL(&_mux);
    _kick();
    xSemaphoreGiveRecursive(_mutex);
}

// ── Cursor ─────────────────────────────────────────────────────────────────

int16_t chipguy_CYD_resistive_terminal::rows() const {
    if (!_CH) return 0;
    int32_t r0 = _lineFloor(_vtopT);
    return (int16_t)((_vtopT + _VSA - r0 + _CH - 1) / _CH);
}

void chipguy_CYD_resistive_terminal::setCursor(int16_t col, int16_t row) {
    if (!_task) return;
    xSemaphoreTakeRecursive(_mutex, portMAX_DELAY);
    _waitSwitch();
    portENTER_CRITICAL(&_mux);
    int16_t nrows = rows();
    if (row < 0) row = 0;
    if (row >= nrows) row = nrows - 1;
    if (col < 0) col = 0;
    if (col >= _cols) col = _cols - 1;
    int32_t top = _lineFloor(_vtopT) + (int32_t)row * _CH;
    _setCursorLocked(top, col);
    _requestSwitchLocked(top, false);
    portEXIT_CRITICAL(&_mux);
    _kick();
    xSemaphoreGiveRecursive(_mutex);
}

int16_t chipguy_CYD_resistive_terminal::cursorCol() const { return _curCol; }

int16_t chipguy_CYD_resistive_terminal::cursorRow() const {
    return (int16_t)((_curTop - _lineFloor(_vtopT)) / _CH);
}

void chipguy_CYD_resistive_terminal::setCursorStyle(cydr_cursor_t style) {
    if (!_task) { _cursorStyle = style; return; }
    portENTER_CRITICAL(&_mux);
    _markCursorLocked();              // redraw the cell in the old style...
    _cursorStyle = style;
    _blinkOn = true;
    _blinkAt = xTaskGetTickCount() + BLINK_TICKS;
    _markCursorLocked();              // ...and the new (one of them is OFF-safe)
    portEXIT_CRITICAL(&_mux);
    _kick();
}

void chipguy_CYD_resistive_terminal::evictPartialLine(bool backward) {
    if (!_task) return;
    xSemaphoreTakeRecursive(_mutex, portMAX_DELAY);
    _waitSwitch();
    portENTER_CRITICAL(&_mux);
    if (!backward) {
        int32_t p = pmod(_vtopT - _grid, _CH);
        if (p) {
            _vtopT += _CH - p;
            if (_curTop < _vtopT) {               // the cursor's line went off the top
                _setCursorLocked(_vtopT, min<int16_t>(_curCol, _cols - 1));
                _requestSwitchLocked(_vtopT, false);
            }
        }
    } else {
        int32_t q = pmod(_vtopT + _VSA - _grid, _CH);
        if (q) {
            _vtopT -= q;
            if (_curTop + _CH > _vtopT + _VSA) {  // the cursor's line went off the bottom
                int32_t top = _curTop - _CH;
                _setCursorLocked(top, min<int16_t>(_curCol, _cols - 1));
                _requestSwitchLocked(top, false);
            }
            // Rows scrolled in at the top come in blank; nothing below the
            // cursor's new line is known any more.
            _blankFrom = INT32_MAX;
        }
    }
    portEXIT_CRITICAL(&_mux);
    _kick();
    xSemaphoreGiveRecursive(_mutex);
}

// ── Freeze ─────────────────────────────────────────────────────────────────

bool chipguy_CYD_resistive_terminal::freeze(uint16_t topPixels, uint16_t bottomPixels) {
    if (!_task) return false;
    if ((int)topPixels + bottomPixels + _CH > _H) return false;
    xSemaphoreTakeRecursive(_mutex, portMAX_DELAY);
    flush();
    portENTER_CRITICAL(&_mux);
    int16_t T2 = topPixels, B2 = bottomPixels, V2 = _H - T2 - B2;
    // Where the cursor's line is on screen, and in frame memory.
    int32_t yc = _fixT + (_curTop - _vtop);
    int32_t mc = _fixT + _ring(_curTop);
    bool keep = _ring(_curTop) + _CH <= _VSA;      // didn't straddle the wrap point
    int32_t off2 = 0;
    if (yc >= T2 && yc + _CH <= T2 + V2 && mc >= T2 && mc + _CH <= T2 + V2) {
        // Choose the new scroll offset that leaves the cursor's line -- and
        // whatever sits next to it in memory -- where it is on screen.
        off2 = pmod(mc - yc, V2);
    } else {
        keep = false;
    }
    int32_t yc2 = yc;
    if (yc2 < T2) yc2 = T2;
    if (yc2 > T2 + V2 - _CH) yc2 = T2 + V2 - _CH;
    if (yc2 != yc) keep = false;

    _fixT = T2; _fixB = B2; _VSA = V2;
    _vtop = _vtopT = off2;
    _curTop = off2 + (yc2 - T2);
    _grid = pmod(_curTop, _CH);
    _lineTop = _curTop;
    _lineGen++;
    _swPending = false;
    _pending = 0;
    if (!keep)
        for (int c = 0; c < _cols; c++) _cells[c] = {CELL_KEEP, _fg, _bg};
    if (_curCol > _cols - 1) _curCol = _cols - 1;
    _blankFrom = INT32_MAX;
    _markCursorLocked();
    _cfgReq = true;
    portEXIT_CRITICAL(&_mux);
    flush();
    xSemaphoreGiveRecursive(_mutex);
    return true;
}

// ── Bitmaps ────────────────────────────────────────────────────────────────

void chipguy_CYD_resistive_terminal::drawBitmap(int16_t x, int16_t y, int16_t w, int16_t h,
                                                const uint16_t *pixels) {
    if (!_task || !pixels || w <= 0 || h <= 0) return;
    BlitReq r;
    r.x0 = max<int16_t>(x, 0);
    r.y0 = max<int16_t>(y, 0);
    r.x1 = min<int>(x + w, _W);
    r.y1 = min<int>(y + h, _H);
    if (r.x0 >= r.x1 || r.y0 >= r.y1) return;
    r.ox = x; r.oy = y; r.w = w; r.px = pixels;

    xSemaphoreTakeRecursive(_mutex, portMAX_DELAY);
    bool scrolls = r.y0 < _fixT + _VSA && r.y1 > _fixT;
    if (scrolls) flush();       // land where the caller sees (x, y) now
    portENTER_CRITICAL(&_mux);
    _blit = &r;
    if (scrolls) {
        // Lines under the image are no longer known blank.
        int32_t vb = _vtop + (min<int>(r.y1, _fixT + _VSA) - _fixT);
        if (_blankFrom != INT32_MAX && _blankFrom < vb) _blankFrom = vb;
    }
    portEXIT_CRITICAL(&_mux);
    while (_blit) _waitTask();
    xSemaphoreGiveRecursive(_mutex);
}

// ── Touch ──────────────────────────────────────────────────────────────────

void chipguy_CYD_resistive_terminal::attachTouch(chipguy_CYD_resistive_touch &touch) {
    _touch = &touch;
    touch.setRotation(_rot2 ? 2 : 0);
}

bool chipguy_CYD_resistive_terminal::pollTouch(int16_t &x, int16_t &y, cydr_touch_units_t units) {
    if (!_touch) return false;
    uint16_t tx, ty;
    if (!_touch->read(tx, ty)) return false;
    if (units == CYDR_TOUCH_PIXELS) {
        x = tx;
        y = ty;
        return true;
    }
    if (ty < _fixT || ty >= _fixT + _VSA || tx >= _W) return false;
    portENTER_CRITICAL(&_mux);
    int32_t v = _vtopT + (ty - _fixT);
    int16_t row = (int16_t)((v - _lineFloor(_vtopT)) / _CH);
    portEXIT_CRITICAL(&_mux);
    x = min<int16_t>(tx / CW, _cols - 1);
    y = row;
    return true;
}

// ── printf ─────────────────────────────────────────────────────────────────

size_t chipguy_CYD_resistive_terminal::printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    size_t n = vprintf(fmt, ap);
    va_end(ap);
    return n;
}

size_t chipguy_CYD_resistive_terminal::vprintf(const char *fmt, va_list ap) {
    if (!_task || !fmt) return 0;
    xSemaphoreTakeRecursive(_mutex, portMAX_DELAY);
    using Self = chipguy_CYD_resistive_terminal;
    cydr_fmt_sink sink = {
        [](void *c, const char *s, size_t n) {
            for (size_t i = 0; i < n; i++) ((Self *)c)->_feedByte((uint8_t)s[i]);
        },
        [](void *c, uint32_t cp) { ((Self *)c)->_putCp(cp); },
        [](void *c, uint16_t cp) { return ((Self *)c)->_isWide(cp); },
        this,
    };
    size_t total = cydr_term_vformat(sink, fmt, ap);
    _kick();
    xSemaphoreGiveRecursive(_mutex);
    return total;
}

// Screen cells taken by UTF-8 text, reading at most maxBytes and stopping
// before a sequence that would be cut by that limit.  Returns the byte count
// used through `bytes`.
static int utf8Cells(const char *s, int maxBytes, int &bytes, const cydr_fmt_sink &sink) {
    int i = 0, cells = 0;
    while ((maxBytes < 0 || i < maxBytes) && s[i]) {
        uint8_t b = (uint8_t)s[i];
        int k = b < 0x80 ? 1 : (b & 0xE0) == 0xC0 ? 2 : (b & 0xF0) == 0xE0 ? 3
              : (b & 0xF8) == 0xF0 ? 4 : 1;
        if (maxBytes >= 0 && i + k > maxBytes) break;
        uint32_t cp = b;
        if (k > 1) {
            cp = b & (0x7F >> k);
            for (int j = 1; j < k; j++) {
                if (!s[i + j]) { k = j; break; }
                cp = (cp << 6) | ((uint8_t)s[i + j] & 0x3F);
            }
        }
        if (cp >= 0x20) cells += (cp <= 0xFFFF && sink.wide(sink.ctx, (uint16_t)cp)) ? 2 : 1;
        i += k;
    }
    bytes = i;
    return cells;
}

// The formatter behind printf().  Strings and characters go to the sink as
// they are; each number is rendered by snprintf, without its width, into a
// small buffer on this stack, and padded here.
size_t cydr_term_vformat(const cydr_fmt_sink &sink, const char *fmt, va_list ap) {
    size_t total = 0;
    auto emit = [&](const char *s, size_t n) { sink.bytes(sink.ctx, s, n); total += n; };
    auto pad = [&](int n, char c) {
        for (int i = 0; i < n; i++) sink.bytes(sink.ctx, &c, 1);
        if (n > 0) total += n;
    };

    while (*fmt) {
        if (*fmt != '%') {
            const char *s = fmt;
            while (*fmt && *fmt != '%') fmt++;
            emit(s, fmt - s);
            continue;
        }
        const char *spec = fmt++;
        if (*fmt == '%') { emit("%", 1); fmt++; continue; }

        // flags, width, precision, length
        bool left = false, zero = false;
        char flags[5]; int nf = 0;
        for (;; fmt++) {
            char c = *fmt;
            if (c == '-') left = true;
            else if (c == '0') zero = true;
            else if (c == '+' || c == ' ' || c == '#') { if (nf < 4) flags[nf++] = c; }
            else break;
        }
        flags[nf] = 0;
        int width = 0;
        if (*fmt == '*') {
            width = va_arg(ap, int);
            if (width < 0) { left = true; width = -width; }
            fmt++;
        } else {
            while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        }
        int prec = -1;
        if (*fmt == '.') {
            fmt++;
            prec = 0;
            if (*fmt == '*') { prec = va_arg(ap, int); if (prec < 0) prec = -1; fmt++; }
            else while (*fmt >= '0' && *fmt <= '9') prec = prec * 10 + (*fmt++ - '0');
        }
        char len[3] = {0, 0, 0};
        if (*fmt == 'h' || *fmt == 'l') {
            len[0] = *fmt++;
            if (*fmt == len[0]) len[1] = *fmt++;
        } else if (*fmt == 'j' || *fmt == 'z' || *fmt == 't' || *fmt == 'L') {
            len[0] = *fmt++;
        }
        char conv = *fmt;
        if (!conv) break;               // a '%' cut off by the end: dropped, as newlib does
        fmt++;

        if (conv == 's') {
            if (len[0] == 'l') {
                const wchar_t *ws = va_arg(ap, const wchar_t *);
                if (!ws) ws = L"(null)";
                int n = 0, cells = 0;
                while (ws[n] && (prec < 0 || n < prec)) {
                    uint32_t cp = (uint32_t)ws[n++];
                    cells += (cp <= 0xFFFF && sink.wide(sink.ctx, (uint16_t)cp)) ? 2 : 1;
                }
                if (!left) pad(width - cells, ' ');
                for (int i = 0; i < n; i++) { sink.codepoint(sink.ctx, (uint32_t)ws[i]); total++; }
                if (left) pad(width - cells, ' ');
            } else {
                const char *s = va_arg(ap, const char *);
                if (!s) s = "(null)";
                int bytes, cells = 0;
                if (width > 0 || prec >= 0) cells = utf8Cells(s, prec, bytes, sink);
                else bytes = (int)strlen(s);
                if (!left) pad(width - cells, ' ');
                emit(s, bytes);
                if (left) pad(width - cells, ' ');
            }
            continue;
        }
        if (conv == '%') {              // "%5%": a padded '%', as newlib does
            if (!left) pad(width - 1, ' ');
            emit("%", 1);
            if (left) pad(width - 1, ' ');
            continue;
        }
        if (conv == 'c') {
            int c = va_arg(ap, int);
            bool w2 = len[0] == 'l' && (uint32_t)c <= 0xFFFF && sink.wide(sink.ctx, (uint16_t)c);
            int cells = w2 ? 2 : 1;
            if (!left) pad(width - cells, ' ');
            if (len[0] == 'l') { sink.codepoint(sink.ctx, (uint32_t)c); total++; }
            else { char ch = (char)c; emit(&ch, 1); }
            if (left) pad(width - cells, ' ');
            continue;
        }
        if (conv == 'n') {             // accepted and ignored
            (void)va_arg(ap, void *);
            continue;
        }

        // Numbers: snprintf one value, without the width, then pad here.
        char sub[16];
        int k = 0;
        sub[k++] = '%';
        for (int i = 0; flags[i]; i++) sub[k++] = flags[i];
        if (prec >= 0) k += snprintf(sub + k, sizeof(sub) - k - 4, ".%d", prec > 99 ? 99 : prec);
        for (int i = 0; len[i]; i++) sub[k++] = len[i];
        sub[k++] = conv;
        sub[k] = 0;

        char nb[CYDR_TERM_PRINTF_NUMBUF];
        int n = -1;
        bool isInt = false;
        switch (conv) {
            case 'd': case 'i':
                isInt = true;
                if (len[0] == 'l' && len[1] == 'l') n = snprintf(nb, sizeof nb, sub, va_arg(ap, long long));
                else if (len[0] == 'l')             n = snprintf(nb, sizeof nb, sub, va_arg(ap, long));
                else if (len[0] == 'j')             n = snprintf(nb, sizeof nb, sub, va_arg(ap, intmax_t));
                else if (len[0] == 'z')             n = snprintf(nb, sizeof nb, sub, va_arg(ap, ssize_t));
                else if (len[0] == 't')             n = snprintf(nb, sizeof nb, sub, va_arg(ap, ptrdiff_t));
                else                                n = snprintf(nb, sizeof nb, sub, va_arg(ap, int));
                break;
            case 'u': case 'o': case 'x': case 'X':
                isInt = true;
                if (len[0] == 'l' && len[1] == 'l') n = snprintf(nb, sizeof nb, sub, va_arg(ap, unsigned long long));
                else if (len[0] == 'l')             n = snprintf(nb, sizeof nb, sub, va_arg(ap, unsigned long));
                else if (len[0] == 'j')             n = snprintf(nb, sizeof nb, sub, va_arg(ap, uintmax_t));
                else if (len[0] == 'z')             n = snprintf(nb, sizeof nb, sub, va_arg(ap, size_t));
                else if (len[0] == 't')             n = snprintf(nb, sizeof nb, sub, va_arg(ap, ptrdiff_t));
                else                                n = snprintf(nb, sizeof nb, sub, va_arg(ap, unsigned int));
                break;
            case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A':
                if (len[0] == 'L') n = snprintf(nb, sizeof nb, sub, va_arg(ap, long double));
                else               n = snprintf(nb, sizeof nb, sub, va_arg(ap, double));
                break;
            case 'p':
                n = snprintf(nb, sizeof nb, sub, va_arg(ap, void *));
                break;
            default:                    // unknown: print the spec as written
                emit(spec, fmt - spec);
                continue;
        }
        if (n < 0) continue;
        if (n > (int)sizeof nb - 1) n = sizeof nb - 1;   // cut short; see CYDR_TERM_PRINTF_NUMBUF

        int padn = width - n;
        if (padn <= 0) { emit(nb, n); continue; }
        if (left) { emit(nb, n); pad(padn, ' '); continue; }
        // Zero padding goes after any sign and 0x prefix, and not for
        // integers with a precision (as C does) or for inf / nan.
        int pre = 0;
        if (nb[0] == '+' || nb[0] == '-' || nb[0] == ' ') pre = 1;
        if (nb[pre] == '0' && (nb[pre + 1] == 'x' || nb[pre + 1] == 'X')) pre += 2;
        char c0 = nb[pre];
        bool special = c0 == 'i' || c0 == 'I' || c0 == 'n' || c0 == 'N';   // inf, nan
        if (zero && !special && !(isInt && prec >= 0)) {
            emit(nb, pre);
            pad(padn, '0');
            emit(nb + pre, n - pre);
        } else {
            pad(padn, ' ');
            emit(nb, n);
        }
    }
    return total;
}

// ═══════════════════════════════════════════════════════════════════════════
// Task side.  Everything below runs in the terminal's task, the only user of
// the panel's SPI device once begin() has returned.
// ═══════════════════════════════════════════════════════════════════════════

void chipguy_CYD_resistive_terminal::_taskEntry(void *arg) {
    ((chipguy_CYD_resistive_terminal *)arg)->_taskLoop();
}

void chipguy_CYD_resistive_terminal::_taskLoop() {
    for (;;) {
        bool did = _step();
        if (_waiting) {
            _waiting = false;
            xSemaphoreGive(_waitSem);
        }
        if (did) continue;
        TickType_t wait = portMAX_DELAY;
        if (_cursorStyle != CYDR_CURSOR_OFF) {
            int32_t d = (int32_t)(_blinkAt - xTaskGetTickCount());
            wait = d > 0 ? (TickType_t)d : 0;
        }
        ulTaskNotifyTake(pdTRUE, wait);
    }
}

bool chipguy_CYD_resistive_terminal::_step() {
    // Cursor blink: toggle and mark the cell; the render below draws it.
    TickType_t now = xTaskGetTickCount();
    portENTER_CRITICAL(&_mux);
    if (_cursorStyle != CYDR_CURSOR_OFF && (int32_t)(now - _blinkAt) >= 0) {
        _blinkOn = !_blinkOn;
        _blinkAt = now + BLINK_TICKS;
        _markCursorLocked();
    }
    // Keep virtual coordinates far from overflow: shift everything by a
    // multiple of both the scroll area and the line height.
    if (_vtop > (1 << 30) && !_taskOnLine) {
        int32_t m = (int32_t)_VSA * _CH;
        int32_t k = ((1 << 29) / m) * m;
        _vtop -= k; _vtopT -= k; _lineTop -= k; _curTop -= k; _swTop -= k;
        if (_blankFrom != INT32_MAX) _blankFrom -= k;
    }
    portEXIT_CRITICAL(&_mux);

    return _doConfig() || _doClear() || _doBlit() || _renderPending() ||
           _trySwitch() || _scrollStep();
}

// ── SPI plumbing ───────────────────────────────────────────────────────────
// The bus is held for each step, so a step's command / data sequence isn't
// interleaved with the touch controller (shared-touch boards); the touch
// driver's transfers simply wait for the step to end.

void chipguy_CYD_resistive_terminal::_busBegin() {
    spi_device_acquire_bus(_dev, portMAX_DELAY);
}

void chipguy_CYD_resistive_terminal::_busEnd() {
    _waitDma();
    spi_device_release_bus(_dev);
}

void chipguy_CYD_resistive_terminal::_waitDma() {
    if (_dmaBusy) {
        spi_transaction_t *r;
        spi_device_get_trans_result(_dev, &r, portMAX_DELAY);
        _dmaBusy = false;
    }
}

// Command plus parameter bytes, polled (DC: user 0 = command, 1 = data).
// Parameters go four at a time in the transaction itself, never by DMA.
void chipguy_CYD_resistive_terminal::_cmd(uint8_t cmd, const uint8_t *data, int n) {
    _waitDma();
    spi_transaction_t t = {};
    t.length = 8;
    t.flags = SPI_TRANS_USE_TXDATA;
    t.tx_data[0] = cmd;
    t.user = (void *)0;
    spi_device_polling_transmit(_dev, &t);
    for (int i = 0; i < n; i += 4) {
        int k = min(4, n - i);
        t = {};
        t.length = k * 8;
        t.flags = SPI_TRANS_USE_TXDATA;
        t.user = (void *)1;
        memcpy(t.tx_data, data + i, k);
        spi_device_polling_transmit(_dev, &t);
    }
}

// Address window in panel (MCU) coordinates, then RAMWR.  CASET is skipped
// when the columns haven't changed.
void chipguy_CYD_resistive_terminal::_window(int x0, int x1, int y0, int y1) {
    uint8_t d[4];
    if (x0 != _casetX0 || x1 != _casetX1) {
        d[0] = x0 >> 8; d[1] = x0; d[2] = x1 >> 8; d[3] = x1;
        _cmd(CMD_CASET, d, 4);
        _casetX0 = x0;
        _casetX1 = x1;
    }
    d[0] = y0 >> 8; d[1] = y0; d[2] = y1 >> 8; d[3] = y1;
    _cmd(CMD_RASET, d, 4);
    _cmd(CMD_RAMWR, nullptr, 0);
}

// Queue pixel data by DMA.  Waits for the previous transfer first, so the
// caller renders the next row into the other half of the buffer meanwhile.
void chipguy_CYD_resistive_terminal::_queue(const uint16_t *buf, int bytes) {
    _waitDma();
    _trans = {};
    _trans.length = bytes * 8;
    _trans.tx_buffer = buf;
    _trans.user = (void *)1;
    if (spi_device_queue_trans(_dev, &_trans, portMAX_DELAY) == ESP_OK) _dmaBusy = true;
}

// VSCRDEF.  The controllers count the fixed areas from the top of frame
// memory; with the panel upside down (rotation 2, MADCTL MY) frame memory runs
// bottom-to-top on screen, so the screen's top area is memory's bottom one.
void chipguy_CYD_resistive_terminal::_sendScrollDef(int16_t T, int16_t VSA, int16_t B) {
    int16_t tfa = _rot2 ? B : T, bfa = _rot2 ? T : B;
    uint8_t d[6] = {(uint8_t)(tfa >> 8), (uint8_t)tfa, (uint8_t)(VSA >> 8), (uint8_t)VSA,
                    (uint8_t)(bfa >> 8), (uint8_t)bfa};
    _cmd(CMD_VSCRDEF, d, 6);
}

// VSCRSADD: the frame-memory line shown at the top of the scroll area.  Screen
// row T + k shows panel row T + (off + k) mod VSA, where off = vtop mod VSA;
// upside down the same picture needs the offset mirrored (README-engineering.md).
void chipguy_CYD_resistive_terminal::_sendVsp(int32_t vtop, int16_t T, int16_t VSA, int16_t B) {
    int32_t off = pmod(vtop, VSA);
    int32_t vsp = _rot2 ? B + pmod(VSA - off, VSA) : T + off;
    uint8_t d[2] = {(uint8_t)(vsp >> 8), (uint8_t)vsp};
    _cmd(CMD_VSCRSADD, d, 2);
}

void chipguy_CYD_resistive_terminal::_fillBlank(uint16_t color) {
    if (_blankValid && _blankColor == color) return;
    uint16_t be = __builtin_bswap16(color);
    for (int i = 0; i < _W; i++) _half(1)[i] = be;
    _blankColor = color;
    _blankValid = true;
}

// ── Steps ──────────────────────────────────────────────────────────────────

bool chipguy_CYD_resistive_terminal::_doConfig() {
    portENTER_CRITICAL(&_mux);
    bool req = _cfgReq;
    int16_t T = _fixT, V = _VSA, B = _fixB;
    int32_t vtop = _vtop;
    portEXIT_CRITICAL(&_mux);
    if (!req) return false;
    _busBegin();
    _casetX0 = _casetX1 = -1;
    _sendScrollDef(T, V, B);
    _sendVsp(vtop, T, V, B);
    _busEnd();
    portENTER_CRITICAL(&_mux);
    _cfgReq = false;
    portEXIT_CRITICAL(&_mux);
    return true;
}

bool chipguy_CYD_resistive_terminal::_doClear() {
    portENTER_CRITICAL(&_mux);
    if (!_clearReq) { portEXIT_CRITICAL(&_mux); return false; }
    uint16_t color = _clearColor;
    bool all = _clearAll;
    _clearReq = false;
    _vtop = 0;
    int16_t T = _fixT, V = _VSA, B = _fixB;
    portEXIT_CRITICAL(&_mux);

    _busBegin();
    _sendVsp(0, T, V, B);
    uint16_t be = __builtin_bswap16(color);
    for (int i = 0; i < _W; i++) _half(0)[i] = _half(1)[i] = be;
    _blankColor = color;
    _blankValid = true;
    int y0 = all ? 0 : T, y1 = all ? _H : T + V;
    _window(0, _W - 1, y0, y1 - 1);
    for (int y = y0; y < y1; y += 2) {
        int rows = min(2, y1 - y);
        _queue(_half(0), rows * _W * 2);     // both halves: two rows per transfer
        if ((y & 15) == 0 && _clearReq) break;  // cleared again: start over
    }
    _busEnd();
    return true;
}

bool chipguy_CYD_resistive_terminal::_doBlit() {
    BlitReq *r = _blit;
    if (!r) return false;
    portENTER_CRITICAL(&_mux);
    int16_t T = _fixT, V = _VSA;
    int32_t vtop = _vtop;
    portEXIT_CRITICAL(&_mux);

    _busBegin();
    int bw = r->x1 - r->x0;
    int h = 0;
    int y = r->y0;
    while (y < r->y1) {
        // A run of screen rows that is contiguous in frame memory.
        int m, n;
        if (y < T)            { m = y; n = min<int>(r->y1, T) - y; }
        else if (y >= T + V)  { m = y; n = r->y1 - y; }
        else {
            m = T + pmod(vtop + (y - T), V);
            n = min<int>(r->y1, T + V) - y;
            n = min(n, T + V - m);          // up to the wrap point
        }
        _window(r->x0, r->x1 - 1, m, m + n - 1);
        for (int k = 0; k < n; k++, y++) {
            const uint16_t *src = r->px + (int32_t)(y - r->oy) * r->w + (r->x0 - r->ox);
            uint16_t *dst = _half(h);
            for (int i = 0; i < bw; i++) dst[i] = __builtin_bswap16(src[i]);
            _queue(dst, bw * 2);
            h ^= 1;
        }
    }
    _busEnd();
    _blankValid = false;
    portENTER_CRITICAL(&_mux);
    _blit = nullptr;
    portEXIT_CRITICAL(&_mux);
    return true;
}

bool chipguy_CYD_resistive_terminal::_trySwitch() {
    portENTER_CRITICAL(&_mux);
    bool did = false;
    if (_swPending && _lineDoneLocked() && !_taskOnLine) {
        _doSwitchLocked();
        did = true;
    }
    portEXIT_CRITICAL(&_mux);
    return did;
}

// Draw the changed cells of the line, in the rows of it now on screen.
bool chipguy_CYD_resistive_terminal::_renderPending() {
    portENTER_CRITICAL(&_mux);
    uint32_t bits = _pending;
    if (!bits) { portEXIT_CRITICAL(&_mux); return false; }
    _pending = 0;
    int32_t top = _lineTop;
    uint32_t gen = _lineGen;
    int32_t a = max(top, _vtop), b = min(top + _CH, _vtop + _VSA);
    if (a >= b) {                 // not on screen: scrolling draws it from the cells
        portEXIT_CRITICAL(&_mux);
        return true;
    }
    _taskOnLine = true;
    portEXIT_CRITICAL(&_mux);

    _busBegin();
    _renderRuns(bits, gen, top, a - top, b - top, true);
    _busEnd();

    portENTER_CRITICAL(&_mux);
    _taskOnLine = false;
    portEXIT_CRITICAL(&_mux);
    return true;
}

// Widen a span so it doesn't start or end in the middle of a wide character.
void chipguy_CYD_resistive_terminal::_alignSpan(int &c0, int &c1) const {
    if (c0 > 0 && _cells[c0].ch == CELL_WIDE_R) c0--;
    if (c1 + 1 < _cols && _cells[c1 + 1].ch == CELL_WIDE_R) c1++;
}

// Draw cells marked in `bits`, rows py0..py1-1, as rectangles.  A rectangle
// spans from one marked cell to the next, across unmarked cells whose
// contents are known (redrawing them is harmless), but never across a
// CELL_KEEP cell, whose pixels the terminal doesn't know.
void chipguy_CYD_resistive_terminal::_renderRuns(uint32_t bits, uint32_t gen, int32_t top,
                                                 int py0, int py1, bool pickup) {
    while (bits) {
        int c0 = __builtin_ctz(bits), c1 = c0;
        if (_cells[c0].ch == CELL_KEEP) { bits &= ~(1u << c0); continue; }
        for (int k = c0 + 1; k < _cols && _cells[k].ch != CELL_KEEP; k++)
            if (bits & (1u << k)) c1 = k;
        _alignSpan(c0, c1);
        bits &= ~(((2u << c1) - 1) & ~((1u << c0) - 1));
        _renderRect(c0, c1, gen, top, py0, py1, pickup);
    }
}

// Stream one rectangle of the line, row by row through the two halves of the
// DMA buffer.  With `pickup`, cells the caller changes meanwhile join in at
// the next row: the rectangle widens to take in new text beside it, and the
// rows already passed are redrawn for those cells afterwards.
void chipguy_CYD_resistive_terminal::_renderRect(int c0, int c1, uint32_t gen, int32_t top,
                                                 int py0, int py1, bool pickup) {
    uint32_t missMask = 0;
    int missRows = py0;
    int h = 0;
    int py = py0;
    while (py < py1) {
        int rr = _ring(top + py);
        int segEnd = min(py1, py + (_VSA - rr));       // stop at the wrap point
        _window(c0 * CW, (c1 + 1) * CW - 1, _fixT + rr, _fixT + rr + (segEnd - py) - 1);
        bool rewindow = false;
        for (; py < segEnd && !rewindow; py++) {
            int bytes = _renderRow(c0, c1, py, _half(h));
            _queue(_half(h), bytes);
            h ^= 1;
            if (!pickup || py + 1 >= py1) continue;

            portENTER_CRITICAL(&_mux);
            if (_lineGen != gen || _clearReq) {        // the line moved on: stop
                portEXIT_CRITICAL(&_mux);
                _blankValid = false;
                return;
            }
            uint32_t nb = _pending;
            if (nb) {
                // Cells changed inside the rectangle: the rows still to come
                // read them fresh; the rows passed need redrawing (below).
                // New text beside the rectangle: widen it over known cells.
                int n0 = c0, n1 = c1;
                for (int k = c1 + 1; k < _cols && _cells[k].ch != CELL_KEEP; k++)
                    if (nb & (1u << k)) n1 = k;
                for (int k = c0 - 1; k >= 0 && _cells[k].ch != CELL_KEEP; k--)
                    if (nb & (1u << k)) n0 = k;
                _alignSpan(n0, n1);
                uint32_t nspan = ((2u << n1) - 1) & ~((1u << n0) - 1);
                uint32_t taken = nb & nspan;
                _pending &= ~taken;     // inside the (widened) rectangle
                if (taken) {
                    missMask |= taken;
                    missRows = py + 1;
                }
                if (n0 != c0 || n1 != c1) {
                    c0 = n0;
                    c1 = n1;
                    rewindow = true;
                }
            }
            portEXIT_CRITICAL(&_mux);
        }
    }
    _blankValid = false;
    if (missMask && missRows > py0) _renderRuns(missMask, gen, top, py0, missRows, false);
}

// Render pixel row `py` of cells c0..c1 into `out`; returns the byte count.
int chipguy_CYD_resistive_terminal::_renderRow(int c0, int c1, int py, uint16_t *out) {
    uint16_t *p = out;
    for (int c = c0; c <= c1; c++) {
        if (_cells[c].ch == CELL_WIDE_R && c > c0) continue;   // drawn with its left half
        int w = _renderCell(c, py, p);
        p += w;
        if (w == 2 * CW) c++;
    }
    return (int)(p - out) * 2;
}

int chipguy_CYD_resistive_terminal::_renderCell(int c, int py, uint16_t *out) {
    Cell cell = _cells[c];
    if (cell.ch == CELL_KEEP || cell.ch == CELL_WIDE_R) {
        // Unknown contents (drawn only when scrolled into view, so blank), or
        // an orphaned right half.
        uint16_t be = __builtin_bswap16(cell.ch == CELL_KEEP ? _screenBg : cell.bg);
        for (int i = 0; i < CW; i++) out[i] = be;
        return CW;
    }
    uint16_t fg = cell.fg, bg = cell.bg;
    bool cur = _cursorStyle != CYDR_CURSOR_OFF && _blinkOn && _curTop == _lineTop &&
               _curCol == c;
    if (cur && _cursorStyle == CYDR_CURSOR_BLOCK) { uint16_t t = fg; fg = bg; bg = t; }
    uint32_t len;
    bool wide;
    const uint8_t *g = _glyph(cell.ch, len, wide);
    int w = CW;
    if (wide) {
        if (c + 1 < _cols && _cells[c + 1].ch == CELL_WIDE_R) w = 2 * CW;
        else g = nullptr;                    // half a pair: draw nothing
    }
    _palette(fg, bg);
    if (g) decodeRow(g, g + len, w, py, _pal, out);
    else for (int i = 0; i < w; i++) out[i] = _pal[0];
    if (cur && _cursorStyle == CYDR_CURSOR_UNDERLINE && py >= _CH - 3)
        for (int i = 0; i < CW; i++) out[i] = _pal[15];
    return w;
}

// Scroll by one or two pixel rows.  The panel rows that wrap round from one
// edge of the scroll area to the other are rewritten after the offset moves:
// from the cell array where they belong to the cell line, else blank.
//
// Until they are rewritten they still show what they held at the other edge.
// A fast step accepts that: two rows per step, one window, one DMA of both
// halves of the buffer.  A clean step avoids it: one row, blanked first while
// it is still at the edge being scrolled away (where a one-row blank is the
// price).  CYDR_SCROLL_AUTO steps fast while text is pouring in -- when the
// whole screen is in motion a one-row flash can't be seen -- and clean when a
// line scrolls on its own, when it could.
bool chipguy_CYD_resistive_terminal::_scrollStep() {
    portENTER_CRITICAL(&_mux);
    if (_vtop == _vtopT) { portEXIT_CRITICAL(&_mux); return false; }
    bool fwd = _vtopT > _vtop;
    int32_t v = _vtop;
    int32_t left = fwd ? _vtopT - v : v - _vtopT;
    int16_t T = _fixT, V = _VSA, B = _fixB;
    bool clean = _scrollMode == CYDR_SCROLL_CLEAN;
    if (_scrollMode == CYDR_SCROLL_AUTO) {
        TickType_t oldest = _scrollTicks[_scrollTickIdx & 3];   // four scrolls ago
        bool rapid = left > _CH || (oldest && xTaskGetTickCount() - oldest < RAPID_TICKS);
        clean = !rapid;
    }
    int k = clean ? 1 : min<int32_t>(2, left);
    int32_t a = fwd ? v + V : v - k;           // first virtual row coming into view
    if (pmod(a, V) + k > V) {                  // don't straddle the wrap point
        k = 1;
        a = fwd ? v + V : v - 1;
    }
    _vtop = fwd ? v + k : v - k;
    int32_t vnow = _vtop;
    int32_t lt = _lineTop;
    bool onLine = a + k > lt && a < lt + _CH;
    if (onLine) _taskOnLine = true;
    uint16_t bg = _screenBg;
    portEXIT_CRITICAL(&_mux);
    int m = T + pmod(a, V);

    _busBegin();
    if (clean) {                               // blank it while still at the far edge
        _fillBlank(bg);
        _window(0, _W - 1, m, m);
        _queue(_half(1), _W * 2);
    }
    _sendVsp(vnow, T, V, B);
    // Rows a .. a+k-1 into consecutive halves of the buffer (the halves are
    // contiguous, so two rows go out as one transfer).  A clean step's single
    // row goes in half 0, leaving the blank half alone.
    uint16_t be = __builtin_bswap16(bg);
    for (int i = 0; i < k; i++) {
        int32_t row = a + i;
        uint16_t *out = _half(i);
        int x = 0;
        if (row >= lt && row < lt + _CH) x = _renderRow(0, _cols - 1, row - lt, out) / 2;
        for (; x < _W; x++) out[x] = be;
    }
    if (k == 2) _blankValid = false;
    // VSCRSADD ended any memory write: open the window again.  (Without this,
    // the ST7789 / ST7796 take the pixels as more VSCRSADD parameters and the
    // scroll snaps back.)
    _window(0, _W - 1, m, m + k - 1);
    _queue(_half(0), _W * 2 * k);
    _busEnd();
    _scrolled += k;

    if (onLine) {
        portENTER_CRITICAL(&_mux);
        _taskOnLine = false;
        portEXIT_CRITICAL(&_mux);
    }
    return true;
}
