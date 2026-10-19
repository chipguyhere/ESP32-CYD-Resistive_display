/*
 * TerminalScrollSpeed - how fast the text terminal prints and scrolls, on a
 * resistive-touch CYD (ESP32-2432S028; ESP32-32E 2.8", 3.2", 3.5", 4.0";
 * ESP32-024).
 *
 * Plays a pretend boot log: bursts of a few hundred lines as fast as the
 * terminal will take them, then a pause "waiting" for something, with a
 * spinner that rewrites one line in place.  Too fast to read -- that's the
 * point.  The panel's hardware scrolls the text one pixel row at a time; the
 * sketch only ever prints.  Screen lines per second for each burst go to the screen
 * and to Serial.  Bursts alternate between the two scroll modes,
 * CYDR_SCROLL_FAST and CYDR_SCROLL_CLEAN (see setScrollMode()).
 */

#include <chipguy_CYD_resistive_terminal.h>

chipguy_CYD_resistive_display  display;
chipguy_CYD_resistive_terminal term;

static const char *const kWords[] = {
    "init", "probe", "ok", "mount", "sync", "link up", "retry", "cache",
    "dma", "irq", "spi", "i2c", "uart", "adc", "rtc", "nvs",
};

// One burst of `lines` log lines; returns pixel rows scrolled per second.
// A line either fits the screen or runs well onto a second line -- never by
// just a character or two, which looks like a mistake.
static float burst(int lines) {
    term.flush();
    uint32_t t0 = millis(), px0 = term.pixelsScrolled();
    for (int i = 0; i < lines; i++) {
        uint32_t ms = millis();
        uint32_t r = esp_random();
        char line[48];
        int n = snprintf(line, sizeof line, "%s %02lX", kWords[r % 16],
                         (unsigned long)((r >> 8) & 0xFF));
        int cells = 11 + n;                       // after the "[ssss.mmm] " stamp
        if ((r & 0x30) == 0) cells += 2;          // " \u2713" is two cells
        int over = cells - term.columns();
        if (over == 1 || over == 2) n += snprintf(line + n, sizeof line - n, " (again)");
        term.setTextColor(CYDR_GREY);
        term.printf("[%4lu.%03lu] ", (unsigned long)(ms / 1000), (unsigned long)(ms % 1000));
        term.setTextColor((r & 7) == 0 ? CYDR_YELLOW : CYDR_WHITE);
        term.printf("%s%s\n", line, (r & 0x30) == 0 ? " \u2713" : "");
    }
    term.flush();
    return (term.pixelsScrolled() - px0) * 1000.0f / (millis() - t0);
}

// Pretend to wait on something for `ms`, spinning a cursor in place.
static void pretendWait(const char *what, uint32_t ms) {
    static const char *const spin = "|/-\\";
    term.setTextColor(CYDR_CYAN);
    uint32_t t0 = millis();
    for (int k = 0; millis() - t0 < ms; k++) {
        term.printf("\r%s %c" CYDR_CLEAR_EOL_STR, what, spin[k & 3]);
        delay(120);
    }
    term.printf("\r%s done" CYDR_CLEAR_EOL_STR "\n", what);
}

void setup() {
    Serial.begin(115200);
    if (!term.begin(display)) {
        Serial.println("Display init failed!");
        while (1) delay(100);
    }
    term.setCursorStyle(CYDR_CURSOR_OFF);
    Serial.printf("TerminalScrollSpeed on %s (%s): %d x %d cells\n",
                  display.modelName(), display.panelName(), term.columns(), term.rows());
}

void loop() {
    static const char *const kWaits[] = {"dhcp", "sensors", "flash", "ntp"};
    static int round = 0;
    // Alternate the scroll modes, to compare.
    term.setScrollMode(round & 1 ? CYDR_SCROLL_CLEAN : CYDR_SCROLL_FAST);
    float pps = burst(200 + 100 * (round % 3));
    float lps = pps / term.cellHeight();
    term.setTextColor(CYDR_GREEN);
    // "115 lines per second" is 20 cells: one line on a 20-column screen,
    // broken between words on a 15-column one.
    term.printf("%s scrolling:\n", round & 1 ? "clean" : "fast");
    term.printf("%.0f lines%sper second\n", lps, term.columns() >= 20 ? " " : "\n");
    Serial.printf("%s burst: %.0f px/s = %.1f screen lines/s, stack never used %lu bytes\n",
                  round & 1 ? "clean" : "fast", pps, lps, (unsigned long)term.stackHighWater());
    pretendWait(kWaits[round++ % 4], 1500);
}
