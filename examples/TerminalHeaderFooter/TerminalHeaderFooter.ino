/*
 * TerminalHeaderFooter - a fixed header and footer with text scrolling
 * between them, on the text terminal of a resistive-touch CYD
 * (ESP32-2432S028; ESP32-32E 2.8", 3.2", 3.5", 4.0"; ESP32-024).
 *
 * The panel's hardware can leave strips at the top and bottom of the screen
 * out of the scroll ("frozen").  This sketch freezes 48 pixels at the top and
 * 32 at the bottom, draws a pre-rendered header and footer image into them
 * with drawBitmap(), and prints a log in between.  While the text scrolls,
 * it keeps updating an "activity" bar inside the footer -- drawing into a
 * frozen area never disturbs the scrolling text.
 *
 * The images are RGB565 arrays in bitmaps.h, made from PNG-like drawings by
 * extras/bitmaps/make_header_footer.py.  Make your own the same way: any
 * w x h array of uint16_t RGB565 pixels, row by row, will do.
 *
 * Between them, header and footer leave 240 pixels (on a 240x320 panel):
 * seven and a half 32-pixel text lines.  So once the text scrolls, the top
 * line is usually cut in half -- evictPartialLine() scrolls just enough to
 * push it off, and this sketch shows that too.
 */

#include <chipguy_CYD_resistive_terminal.h>
#include "bitmaps.h"

chipguy_CYD_resistive_display  display;
chipguy_CYD_resistive_terminal term;

// Fill part of the footer's activity bar: `fill` of BAR_W pixels lit.
static void drawBar(int fill, uint16_t color) {
    uint16_t row[BAR_W];
    for (int x = 0; x < BAR_W; x++) row[x] = x < fill ? color : 0x2104;
    int16_t x0 = (term.width() - FOOTER_W) / 2 + BAR_X;
    int16_t y0 = term.height() - FOOTER_H + BAR_Y;
    for (int y = 0; y < BAR_H; y++) term.drawBitmap(x0, y0 + y, BAR_W, 1, row);
}

void setup() {
    Serial.begin(115200);
    if (!term.begin(display)) {
        Serial.println("Display init failed!");
        while (1) delay(100);
    }
    term.setCursorStyle(CYDR_CURSOR_UNDERLINE);
    term.setScrollMode(CYDR_SCROLL_FAST);    // full speed throughout

    // Freeze the strips, then draw into them.  Centered: on a 240-wide panel
    // the 320-wide images lose 40 pixels at each side.
    term.freeze(HEADER_H, FOOTER_H);
    int16_t x = (term.width() - HEADER_W) / 2;
    term.drawBitmap(x, 0, HEADER_W, HEADER_H, kHeader);
    term.drawBitmap(x, term.height() - FOOTER_H, FOOTER_W, FOOTER_H, kFooter);
    drawBar(0, CYDR_GREEN);

    term.printf("%d x %d cells\nbetween the\nheader and footer\n", term.columns(), term.rows());
}

void loop() {
    static uint32_t n = 0;

    // A long burst of log lines as fast as the terminal takes them, with the
    // footer bar tracking progress.
    const int kLines = 400;
    term.flush();
    uint32_t t0 = millis(), px0 = term.pixelsScrolled();
    for (int i = 0; i < kLines; i++) {
        term.setTextColor(i % 7 ? CYDR_WHITE : CYDR_YELLOW);
        term.printf("line %lu\n", (unsigned long)n++);
        if (i % 10 == 9) drawBar((i + 1) * BAR_W / kLines, CYDR_GREEN);
    }
    term.flush();
    float lps = (term.pixelsScrolled() - px0) * 1000.0f / (millis() - t0) / term.cellHeight();

    // Scrolling speed: "91 lines per second" fits a 20-column screen; break
    // it between words on a 15-column one.
    term.setTextColor(CYDR_GREEN);
    term.printf("%.0f lines%sper second\n", lps, term.columns() >= 20 ? " " : "\n");
    delay(2000);

    // The top line is now probably cut in half by the header.
    term.setTextColor(CYDR_CYAN);
    term.print("top line partial?");
    delay(1500);
    term.evictPartialLine();          // scroll on until it's gone
    term.print("\revicted" CYDR_CLEAR_EOL_STR);
    delay(1500);
    term.evictPartialLine(true);      // or back: blank space comes in at the top
    term.print("\rscrolled back" CYDR_CLEAR_EOL_STR);
    delay(1500);
    term.setTextColor(CYDR_WHITE);
    term.println();
    drawBar(0, CYDR_GREEN);
}
