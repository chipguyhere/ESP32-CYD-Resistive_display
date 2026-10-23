/*
 * TerminalHello - the text terminal's basics on a resistive-touch CYD
 * (ESP32-2432S028; ESP32-32E 2.8", 3.2", 3.5", 4.0"; ESP32-024).
 *
 * No LVGL: print() and printf() write straight onto a grid of character
 * cells, and the panel's own hardware scrolls the text.  This sketch shows
 * colors, printf padding, tabs, clear-to-end-of-line, symbols, box drawing
 * and the four cursor styles, then counts upward forever so you can watch
 * it scroll.
 *
 * Hardware scrolling only works in portrait, the panel's long axis.  For a
 * landscape display, use LVGL instead (see the Lvgl* examples).
 */

#include <chipguy_CYD_resistive_terminal.h>

chipguy_CYD_resistive_display  display;
chipguy_CYD_resistive_terminal term;

void setup() {
    Serial.begin(115200);
    if (!term.begin(display)) {
        Serial.println("Display init failed!");
        while (1) delay(100);
    }
    Serial.printf("Terminal: %s, %d x %d cells, readback %s\n", display.modelName(),
                  term.columns(), term.rows(), term.readbackAvailable() ? "yes" : "no");

    term.setTextColor(CYDR_YELLOW);
    term.println("Hello, CYD!");
    term.setTextColor(CYDR_WHITE);
    term.printf("%d x %d cells\n", term.columns(), term.rows());

    // printf pads to a width: numbers, and strings (counted in cells).
    term.printf("[%5d][%-4s]\n", 42, "ab");
    term.printf("[%05.1f%s]\n", 21.5, "°C");      // 021.5°C

    // Tab moves to the next multiple of 4 columns.
    term.print("a\tbc\td\n");

    // Background colors, and '\v' (CYDR_CLEAR_EOL) to clear the rest of a line.
    term.setTextColor(CYDR_BLACK, CYDR_CYAN);
    term.print(" inverse ");
    term.print(CYDR_CLEAR_EOL_STR "\n");                // the cyan runs to the edge
    term.setTextColor(CYDR_WHITE, CYDR_BLACK);

    // Symbols: Unicode, and LVGL's set under the same names.
    term.print("± µ Ω ✓ ⚠ ");
    term.print(LV_SYMBOL_WIFI " " LV_SYMBOL_BATTERY_3 " " LV_SYMBOL_OK "\n");

    // Box drawing meets at the cell edges.
    term.setTextColor(CYDR_GREEN);
    term.print("┌──┬──┐\n");
    term.print("│OK│  │\n");
    term.print("└──┴──┘\n");
    term.setTextColor(CYDR_WHITE);

    // The cursor can sit on text printed earlier: here, on the "O" of "OK".
    int16_t row = term.cursorRow();
    term.setCursor(1, row - 2);
    delay(2500);
    term.setCursor(0, row);

    // The four cursor styles, two seconds each.
    term.print("block ");
    term.setCursorStyle(CYDR_CURSOR_BLOCK);
    delay(2000);
    term.print("\rline  ");
    term.setCursorStyle(CYDR_CURSOR_UNDERLINE);
    delay(2000);
    term.print("\rbar   ");
    term.setCursorStyle(CYDR_CURSOR_BAR);
    delay(2000);
    term.print("\roff   ");
    term.setCursorStyle(CYDR_CURSOR_OFF);
    delay(2000);
    term.setCursorStyle(CYDR_CURSOR_BLOCK);
    term.println();
}

// Count upward, slowly at first and then with no delay at all: from there on
// the terminal's scrolling is the only thing setting the pace.
void loop() {
    static uint32_t n = 0;
    term.printf("count %lu\n", (unsigned long)n++);
    if (n % 100 == 0)
        Serial.printf("count %lu, terminal task stack never used: %lu bytes\n",
                      (unsigned long)n, (unsigned long)term.stackHighWater());
    if (n < 200) delay(200 - n);
}
