/*
 * TerminalCJK - Chinese, Japanese and Korean on the text terminal of a
 * resistive-touch CYD (ESP32-2432S028; ESP32-32E 2.8", 3.2", 3.5", 4.0";
 * ESP32-024).
 *
 * CJK characters take two cells each; half-width katakana (ｶﾀｶﾅ), one.  A
 * double-width character that won't fit at the end of a line moves to the
 * next one whole, scrolling if needed.
 *
 * ── FLASH: THIS EXAMPLE IS BIG ON PURPOSE ────────────────────────────────
 * The CJK glyphs are an OPT-IN font, and a large one.  Pick ONE of the
 * includes below -- only the language(s) your project prints -- and pass
 * the matching font to setFont().  Each falls back to the built-in font for
 * Latin, symbols and box drawing, so it replaces cydr_font_mono16 entirely.
 *
 * Don't copy this sketch as the start of a project that won't print CJK:
 * the font include alone adds the flash shown below.  Start from
 * TerminalHello instead.
 *
 * Anything but the Korean or the kana-only Japanese font needs a partition
 * scheme with a 3 MB app slot on these 4 MB boards: Tools > Partition Scheme >
 * "Huge APP (3MB No OTA/1MB SPIFFS)", or PartitionScheme=huge_app in the
 * --fqbn.  Korean alone (~0.6 MB) and kana-only Japanese (~66 KB) fit the
 * default scheme's 1.2 MB slot.
 */

// All three languages, ~2.2 MB.  Han characters in Simplified Chinese forms,
// so Japanese kanji look Chinese -- use the Japanese font for Japanese text.
#include <cydr_font_mono16_cjk.h>
#define CJK_FONT cydr_font_mono16_cjk

// Simplified Chinese only (GB2312 level-1 hanzi), ~1.2 MB:
// #include <cydr_font_mono16_zh.h>
// #define CJK_FONT cydr_font_mono16_zh

// Japanese only (JIS level-1 kanji, hiragana, katakana; Japanese glyph
// forms), ~1.0 MB:
// #include <cydr_font_mono16_ja.h>
// #define CJK_FONT cydr_font_mono16_ja

// Japanese without kanji (hiragana, katakana, half-width katakana), ~66 KB;
// fits the default partition scheme.  The kanji in the text below would
// print as replacement characters:
// #include <cydr_font_mono16_ja_kana.h>
// #define CJK_FONT cydr_font_mono16_ja_kana

// Korean only (the 2350 everyday Hangul syllables), ~0.6 MB:
// #include <cydr_font_mono16_ko.h>
// #define CJK_FONT cydr_font_mono16_ko

#include <chipguy_CYD_resistive_terminal.h>

chipguy_CYD_resistive_display  display;
chipguy_CYD_resistive_terminal term;

struct Greeting { const char *lang, *text; };
static const Greeting kText[] = {
    {"中文",   "你好，世界！今天天气很好。"},
    {"日本語", "こんにちは、世界。カタカナとひらがな、漢字。"},
    {"한국어", "안녕하세요, 세계! 오늘 날씨가 좋네요."},
    // Half-width katakana, one cell each (in the Japanese and all-three fonts).
    {"半角", "ﾊﾝｶｸ ｶﾀｶﾅ ﾓ ﾂｶｴﾏｽ｡"},
};

void setup() {
    Serial.begin(115200);
    if (!term.begin(display, 0, &CJK_FONT)) {
        Serial.println("Display init failed!");
        while (1) delay(100);
    }
    term.setCursorStyle(CYDR_CURSOR_OFF);
}

void loop() {
    for (const Greeting &g : kText) {
        term.setTextColor(CYDR_YELLOW);
        term.printf("[%s]\n", g.lang);
        term.setTextColor(CYDR_WHITE);
        term.println(g.text);
        delay(1500);
    }
    // Mixed text, and printf padding counted in cells: each CJK
    // character is two.
    term.setTextColor(CYDR_CYAN);
    term.printf("|%-8s|\n|%8s|\n", "温度", "22.5°C");
    term.setTextColor(CYDR_WHITE);
    term.println();
    delay(2500);
}
