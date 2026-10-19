/*
 * TerminalTouchButtons - touch buttons made of inverse text, on the text
 * terminal of a resistive-touch CYD (ESP32-2432S028; ESP32-32E 2.8", 3.2",
 * 3.5", 4.0"; ESP32-024).
 *
 * A "button" here is just a few character cells printed with the foreground
 * and background colors swapped.  Half-block characters at each end -- a
 * right half block before the label and a left half block after it, in the
 * button's color -- trim half a cell off each side, so a three-letter label
 * sits exactly centered in a button four cells wide.  pollTouch() reports a touch in the same
 * column / row terms setCursor() uses, so finding which button was pressed is
 * a comparison, not a pixel calculation.  The three buttons switch the
 * board's RGB LED; a pressed button shows lit while your finger is on it.
 *
 * Run TouchCalibrate once first if touches land in the wrong place: this
 * sketch loads the calibration it saves.
 *
 * Nothing here scrolls: status text is rewritten in place with setCursor()
 * and '\v' (clear to end of line).  Printing past the bottom line would
 * scroll the buttons away with everything else -- for buttons that stay put
 * while text scrolls, put them in a frozen area (see TerminalHeaderFooter)
 * and test touches with CYDR_TOUCH_PIXELS.
 */

#include <chipguy_CYD_resistive_terminal.h>

chipguy_CYD_resistive_display  display;
chipguy_CYD_resistive_touch    touch;
chipguy_CYD_resistive_terminal term;

struct Button {
    int16_t col, row, width;
    const char *label;
    uint16_t color;
    int8_t pin;        // LED pin (active LOW), -1 if this board doesn't say
    bool on;
};

static Button buttons[3];

static void drawButton(const Button &b, bool pressed) {
    // Off: the color's label on grey.  On: black on the color.  Pressed:
    // white on the color, so a press shows before you let go.
    uint16_t fill = (pressed || b.on) ? b.color : CYDR_GREY;
    uint16_t text = pressed ? CYDR_WHITE : b.on ? CYDR_BLACK : b.color;
    term.setCursor(b.col, b.row);
    term.setTextColor(fill, CYDR_BLACK);
    term.print("\u2590");                    // ▐ right half block: the left end, half a cell in
    term.setTextColor(text, fill);
    term.print(b.label);
    term.setTextColor(fill, CYDR_BLACK);
    term.print("\u258C");                    // ▌ left half block: the right end
    term.setTextColor(CYDR_WHITE, CYDR_BLACK);
}

static void status(int16_t row, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    term.setCursor(0, row);
    term.vprintf(fmt, ap);
    term.print(CYDR_CLEAR_EOL_STR);
    va_end(ap);
}

static int hit(int16_t col, int16_t row) {
    for (int i = 0; i < 3; i++) {
        const Button &b = buttons[i];
        if (row == b.row && col >= b.col && col < b.col + b.width) return i;
    }
    return -1;
}

void setup() {
    Serial.begin(115200);
    if (!term.begin(display)) {
        Serial.println("Display init failed!");
        while (1) delay(100);
    }
    touch.begin(display);
    touch.loadCalibration();     // saved by TouchCalibrate, if it has run
    touch.loadFilters();
    term.attachTouch(touch);
    term.setCursorStyle(CYDR_CURSOR_OFF);

    // Three buttons of five cells (half block, three letters, half block)
    // spread across the screen: columns 0, 5, 10 on a 15-column screen,
    // 0, 7, 15 on a 20-column one.
    int16_t gap = (term.columns() - 5) / 2;
    buttons[0] = {0,                2, 5, "RED", CYDR_RED,   display.redLedPin(),   false};
    buttons[1] = {gap,              2, 5, "GRN", CYDR_GREEN, display.greenLedPin(), false};
    buttons[2] = {(int16_t)(2 * gap), 2, 5, "BLU", CYDR_BLUE,  display.blueLedPin(),  false};
    for (Button &b : buttons) {
        if (b.pin >= 0) { pinMode(b.pin, OUTPUT); digitalWrite(b.pin, HIGH); }   // off
        drawButton(b, false);
    }
    term.setTextColor(CYDR_YELLOW);
    term.setCursor(0, 0);
    term.print("RGB LED");
    term.setTextColor(CYDR_WHITE);
    status(4, "Tap a button");
}

void loop() {
    static int pressed = -1;
    int16_t col, row, px, py;
    bool down = term.pollTouch(col, row);
    if (down) {
        term.pollTouch(px, py, CYDR_TOUCH_PIXELS);
        status(6, "cell %d,%d", col, row);
        status(7, "px %d,%d", px, py);
    }
    int now = down ? hit(col, row) : -1;
    if (now != pressed) {
        if (pressed >= 0 && !down) {
            // Lifted off the button (not slid off it): toggle it.
            Button &b = buttons[pressed];
            b.on = !b.on;
            if (b.pin >= 0) digitalWrite(b.pin, b.on ? LOW : HIGH);
            status(4, "%s %s", b.label, b.on ? "on" : "off");
            if (b.pin < 0) status(5, "(LED pin unknown)");
        }
        if (pressed >= 0) drawButton(buttons[pressed], false);
        if (now >= 0) drawButton(buttons[now], true);
        pressed = now;
    }
    delay(20);
}
