/*
 * TerminalSymbols - every LVGL symbol the text terminal can print, with its
 * name, on a resistive-touch CYD (ESP32-2432S028; ESP32-32E 2.8", 3.2",
 * 3.5", 4.0"; ESP32-024).
 *
 * The terminal's built-in font carries LVGL's symbol set at the same code
 * points LVGL uses, under the same LV_SYMBOL_* names, so a line like
 *
 *     term.print(LV_SYMBOL_WIFI " connected");
 *
 * shows the same icon here as it would in an LVGL label -- code moves between
 * the two without changes.  (The icons are Font Awesome Free, CC BY 4.0, by
 * Fonticons, Inc.; the names are LVGL's.)
 *
 * The list is longer than the screen: it scrolls through, pauses, and starts
 * over.  A few everyday Unicode symbols that are in the font too follow it.
 */

#include <chipguy_CYD_resistive_terminal.h>

chipguy_CYD_resistive_display  display;
chipguy_CYD_resistive_terminal term;

struct Sym { const char *glyph, *name; };
#define SYM(n) {LV_SYMBOL_##n, #n}
static const Sym kSymbols[] = {
    SYM(BULLET), SYM(AUDIO), SYM(VIDEO), SYM(LIST), SYM(OK), SYM(CLOSE),
    SYM(POWER), SYM(SETTINGS), SYM(HOME), SYM(DOWNLOAD), SYM(DRIVE),
    SYM(REFRESH), SYM(MUTE), SYM(VOLUME_MID), SYM(VOLUME_MAX), SYM(IMAGE),
    SYM(TINT), SYM(PREV), SYM(PLAY), SYM(PAUSE), SYM(STOP), SYM(NEXT),
    SYM(EJECT), SYM(LEFT), SYM(RIGHT), SYM(PLUS), SYM(MINUS), SYM(EYE_OPEN),
    SYM(EYE_CLOSE), SYM(WARNING), SYM(SHUFFLE), SYM(UP), SYM(DOWN), SYM(LOOP),
    SYM(DIRECTORY), SYM(UPLOAD), SYM(CALL), SYM(CUT), SYM(COPY), SYM(SAVE),
    SYM(BARS), SYM(ENVELOPE), SYM(CHARGE), SYM(PASTE), SYM(BELL),
    SYM(KEYBOARD), SYM(GPS), SYM(FILE), SYM(WIFI), SYM(BATTERY_FULL),
    SYM(BATTERY_3), SYM(BATTERY_2), SYM(BATTERY_1), SYM(BATTERY_EMPTY),
    SYM(USB), SYM(BLUETOOTH), SYM(TRASH), SYM(EDIT), SYM(BACKSPACE),
    SYM(SD_CARD), SYM(NEW_LINE),
};

// Unicode symbols handy in embedded projects, also in the built-in font.
static const Sym kUnicode[] = {
    {"°", "degree"}, {"℃", "celsius"}, {"µ", "micro"},
    {"Ω", "ohm"}, {"±", "plus-minus"}, {"≈", "approx"},
    {"≤", "less-equal"}, {"≥", "greater-eq"}, {"✓", "check"},
    {"✗", "cross"}, {"⚠", "warning"}, {"⚡", "voltage"},
    {"←", "arrow left"}, {"→", "arrow right"}, {"▲", "triangle up"},
    {"●", "circle"}, {"█", "full block"}, {"▒", "shade"},
};

static void listOut(const Sym *list, size_t n, uint16_t nameColor) {
    for (size_t i = 0; i < n; i++) {
        term.setTextColor(CYDR_WHITE);
        term.print(list[i].glyph);
        term.setTextColor(nameColor);
        term.printf(" %s\n", list[i].name);
        delay(120);
    }
}

void setup() {
    Serial.begin(115200);
    if (!term.begin(display)) {
        Serial.println("Display init failed!");
        while (1) delay(100);
    }
    term.setCursorStyle(CYDR_CURSOR_OFF);
}

void loop() {
    term.setTextColor(CYDR_YELLOW);
    term.println("LV_SYMBOL_...");
    listOut(kSymbols, sizeof kSymbols / sizeof kSymbols[0], CYDR_CYAN);
    term.setTextColor(CYDR_YELLOW);
    term.println("Unicode");
    listOut(kUnicode, sizeof kUnicode / sizeof kUnicode[0], CYDR_GREEN);
    delay(3000);
}
