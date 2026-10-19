/*
 * TerminalMatrixRain - the green "digital rain" from The Matrix, on the text
 * terminal of a resistive-touch CYD (ESP32-2432S028; ESP32-32E 2.8", 3.2",
 * 3.5", 4.0"; ESP32-024).
 *
 * Columns of half-width katakana and digits fall at their own speeds, each
 * led by a bright, almost white glyph and trailing off through darker greens.
 * Glyphs in the trails flicker to new ones now and then.
 *
 * Half-width katakana (ｱ ｲ ｳ ...) are one cell wide, so they share the grid
 * with ordinary text.  They come with the opt-in Japanese fonts; this sketch
 * uses the one without kanji, cydr_font_mono16_ja_kana.h (about 66 KB of
 * flash), which fits the default partition scheme.  The full Japanese font,
 * cydr_font_mono16_ja.h, has them too.
 *
 * How it draws: the terminal only remembers the line the cursor is on, so the
 * sketch keeps its own picture of the screen -- a glyph and a shade per cell
 * -- and each frame redraws only the cells whose glyph or shade changed,
 * working down the screen a row at a time.  No scrolling is involved.
 */

#include <cydr_font_mono16_ja_kana.h>
#include <chipguy_CYD_resistive_terminal.h>

chipguy_CYD_resistive_display  display;
chipguy_CYD_resistive_terminal term;

// Types before the first function: Arduino puts its generated prototypes
// there, and newDrop(Drop &) needs Drop declared.
static const int kMaxCols = CYDR_TERM_MAX_COLS, kMaxRows = 15;
struct Cell { uint16_t glyph; uint8_t shade; };
static Cell want[kMaxRows][kMaxCols], shown[kMaxRows][kMaxCols];

struct Drop {
    int16_t head;       // row of the head; may be above or below the screen
    uint8_t length;     // trail length in cells
    uint8_t period;     // frames per row: 1 = fastest
    uint8_t wait;       // frames until the next row
};
static Drop drops[kMaxCols];
static int cols, rows;

// Shades, brightest first: the head, then the trail fading out.  0 = blank.
static const uint16_t kShade[] = {
    CYDR_BLACK,
    cydr_rgb565(200, 255, 200),     // 1: head, nearly white
    cydr_rgb565(40, 255, 80),
    cydr_rgb565(0, 210, 60),
    cydr_rgb565(0, 165, 45),
    cydr_rgb565(0, 125, 35),
    cydr_rgb565(0, 90, 25),
    cydr_rgb565(0, 55, 15),
};
static const int kShades = sizeof kShade / sizeof kShade[0];

// Half-width katakana U+FF66..U+FF9D, and digits as the film mixes in.
static uint16_t randomGlyph() {
    uint32_t r = esp_random();
    if ((r & 7) == 0) return '0' + (r >> 3) % 10;
    return 0xFF66 + (r >> 3) % (0xFF9D - 0xFF66 + 1);
}

static void newDrop(Drop &d) {
    d.length = 4 + esp_random() % (rows + 2);
    d.period = 1 + esp_random() % 3;
    d.head = -(int)(esp_random() % (rows * 2));     // start above the top, staggered
    d.wait = d.period;
}

// Advance the rain one frame and work out what every cell should show.
static void step() {
    for (int c = 0; c < cols; c++) {
        Drop &d = drops[c];
        if (--d.wait == 0) {
            d.wait = d.period;
            d.head++;
            if (d.head >= 0 && d.head < rows) want[d.head][c].glyph = randomGlyph();
            if (d.head - d.length >= rows) newDrop(d);
        }
        for (int r = 0; r < rows; r++) {
            int dist = d.head - r;                      // 0 = the head itself
            uint8_t shade = 0;
            if (dist == 0) shade = 1;
            else if (dist > 0 && dist < d.length)
                shade = 2 + (dist - 1) * (kShades - 2) / d.length;
            want[r][c].shade = shade;
        }
    }
    // A few trail glyphs change in place.
    for (int k = 0; k < cols / 3; k++) {
        int r = esp_random() % rows, c = esp_random() % cols;
        if (want[r][c].shade > 1) want[r][c].glyph = randomGlyph();
    }
}

// Draw the cells that differ from what's on screen, a row at a time.
static int draw() {
    int n = 0;
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            Cell &w = want[r][c], &s = shown[r][c];
            uint16_t g = w.shade ? w.glyph : ' ';
            if (g == (s.shade ? s.glyph : ' ') && w.shade == s.shade) continue;
            term.setCursor(c, r);
            term.setTextColor(kShade[w.shade], CYDR_BLACK);
            term.putCodepoint(g);
            s = w;
            n++;
        }
    }
    return n;
}

void setup() {
    Serial.begin(115200);
    if (!term.begin(display, 0, &cydr_font_mono16_ja_kana)) {
        Serial.println("Display init failed!");
        while (1) delay(100);
    }
    term.setCursorStyle(CYDR_CURSOR_OFF);
    cols = term.columns();
    rows = min<int>(term.rows(), kMaxRows);
    for (int c = 0; c < cols; c++) newDrop(drops[c]);
    for (int r = 0; r < rows; r++)
        for (int c = 0; c < cols; c++) want[r][c] = shown[r][c] = {randomGlyph(), 0};
}

void loop() {
    static uint32_t frames = 0, cells = 0, t0 = millis();
    uint32_t start = millis();
    step();
    cells += draw();
    frames++;
    if (millis() - t0 >= 5000) {
        Serial.printf("%.1f frames/s, %lu cells per frame\n", frames * 1000.0f / (millis() - t0),
                      (unsigned long)(cells / frames));
        frames = cells = 0;
        t0 = millis();
    }
    // About 16 frames a second, close to the film's pace.
    uint32_t spent = millis() - start;
    if (spent < 60) delay(60 - spent);
}
