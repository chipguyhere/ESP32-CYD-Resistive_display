/*******************************************************************************
 * BoardDetect - which board is this?
 *
 * For the resistive-touch "Cheap Yellow Display" boards: ESP32-2432S028,
 * ESP32-32E 2.8", 3.2", 3.5", 4.0", ESP32-024 2.4" and ESP32-035 3.5" (XPT2046 touch; board
 * and panel detected at start-up).
 *
 * Runs the library's board detection and shows the result on the screen (and
 * on Serial at 115200): the board model and its panel, the touch chip, the
 * backlight and RGB LED pins, whether a saved touch calibration was found, the
 * SD card, and the board's battery support, light sensor and amplifier.  The
 * battery voltage and the light sensor are read live, once a second; the light
 * sensor only responds to near-darkness, so cover the board to see it change.
 *
 * Once the screen is drawn LVGL is stopped, as in TouchCalibrate's drawing
 * test: touches are blitted straight to the panel as dots, so you can draw all
 * over it to check that touch follows your finger.  BOOT clears the drawing.
 * After that LVGL redraws only the live lines.
 *
 * The detection calls themselves are just these, after lv_setup.begin():
 *   display.model() / modelName()   which board
 *   display.board()                 own-touch or shared-touch wiring
 *   display.panel() / panelName()   ILI9341, ST7789 or ST7796
 *   display.redLedPin()             4 or 22 (-1 if WiFi was on first)
 *   display.greenLedPin() / blueLedPin()   16 / 17 (17 / 16 on the ESP32-024 / -035)
 * See README-board-detection.md.
 *
 * Requires the following libraries:
 *   - chipguy_ESP32-CYD-Resistive_display  (display + touch driver, this library)
 *   - lvgl 9
 *
 * Arduino IDE Board Settings:
 *   - Board: "ESP32 Dev Module" (in Tools > Board > esp32).
 *   - The defaults work; the sketch fits the default partition scheme.
 *
 ******************************************************************************/

// The bundled lv_conf.h is based on LVGL 9.3 (LVGL 9 compatible).
#include "lv_conf.h"
#include "lvgl.h"
#include "lv_setup.hpp"
#include <SD.h>

#define BOOT_PIN 0                        // BOOT button: clears the drawing
static const uint16_t C_DOT = 0xFFE0;     // yellow (RGB565)
static const uint32_t C_TITLE = 0x40C0FF;

// The light sensor reads 0 (shown as the converter's ~142 mV floor) in
// anything but near-darkness, and rises only in the dark: about 1.67 V on the
// 2432S028 and ESP32-035, 0.64 V on the ESP32-024 (bench, 2026-10-18 and
// 2026-10-23).  300 mV splits them.  The default 11 dB attenuation spans both.
// The ESP32-035's sensor sees the board's own backlight: in a closed box with
// the screen lit it read only ~0.2 V.
static const uint32_t DARK_MV = 300;

// What each board has besides the screen (see README-board.md).  Every board
// has a speaker amplifier; the power switch is GPIO4 (LOW = on) where there is
// one.  The light sensor is on GPIO34; on the ESP32-32E boards GPIO34 is the
// battery reading instead (half the cell voltage).
struct BoardFacts {
    bool battery;          // battery connector and charger
    bool batteryReading;   // GPIO34 = cell / 2
    bool lightSensor;      // photoresistor on GPIO34
    bool ampSwitch;        // amplifier enable on GPIO4
};

static bool gKnown = true;   // false: model unknown, the facts are guesses

static BoardFacts factsFor(cydr_model_t m) {
    switch (m) {
        case CYDR_MODEL_2432S028:  return {false, false, true,  false};
        case CYDR_MODEL_32E_28:
        case CYDR_MODEL_32E_32:
        case CYDR_MODEL_32E_35_40: return {true,  true,  false, true};
        case CYDR_MODEL_024:
        case CYDR_MODEL_035:       return {true,  false, true,  false};
        default: gKnown = false;   return {false, false, false, display.redLedPin() == 22};
    }
}

static BoardFacts gFacts;
static const char *yesNo(bool b) { return gKnown ? (b ? "yes" : "no") : (b ? "yes?" : "no?"); }

static const char *cardTypeName(uint8_t t) {
    switch (t) {
        case CARD_MMC:  return "MMC";
        case CARD_SD:   return "SD";
        case CARD_SDHC: return "SDHC";
        default:        return "unknown";
    }
}

static String sdText() {
    // The card slot has the same pins on every board: VSPI (the default SPI)
    // with CS on GPIO5.  See README-sdcard.md.
    if (!SD.begin(5)) return "none / not FAT";
    String s = String(cardTypeName(SD.cardType())) + ", " +
               String((uint32_t)(SD.cardSize() >> 20)) + " MB";
    SD.end();
    return s;
}

// ── The live lines (GPIO34 is ADC1, so these reads work with WiFi on) ──────
static String batteryText() {
    String s = String("Battery support: ") + yesNo(gFacts.battery);
    if (gFacts.batteryReading)
        s += String(", ") + String(analogReadMilliVolts(34) * 2 / 1000.0f, 2) + " V";
    return s;
}

static String lightText() {
    String s = String("Light sensor: ") + yesNo(gFacts.lightSensor);
    if (gFacts.lightSensor) s += String(", ") + analogReadMilliVolts(34) + " mV";
    return s;
}

static String darkText() {
    return String("Darkness detector: ") +
           (analogReadMilliVolts(34) > DARK_MV ? "dark" : "not dark");
}

// ── Screen ─────────────────────────────────────────────────────────────────
static lv_obj_t *gList;                  // the column of report lines
static lv_obj_t *lblBattery, *lblLight, *lblDark;

static lv_obj_t *addLine(const String &text) {
    lv_obj_t *l = lv_label_create(gList);
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(l, text.c_str());
    Serial.println(text);
    return l;
}

static lv_obj_t *addHeading(lv_obj_t *parent, const char *text, const lv_font_t *font) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(C_TITLE), 0);
    lv_label_set_text(l, text);
    return l;
}

static void buildScreen() {
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    char panel[32];
    snprintf(panel, sizeof panel, "%s %dx%d", display.panelName(),
             display.nativeWidth(), display.nativeHeight());
    Serial.printf("\n%s\n%s\n", display.modelName(), panel);
    lv_obj_t *title = addHeading(scr, display.modelName(), &lv_font_montserrat_20);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);
    lv_obj_t *sub = addHeading(scr, panel, &lv_font_montserrat_20);
    lv_obj_align_to(sub, title, LV_ALIGN_OUT_BOTTOM_MID, 0, 2);

    gList = lv_obj_create(scr);
    lv_obj_set_size(gList, display.width() - 16, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(gList, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(gList, 0, 0);
    lv_obj_set_style_pad_all(gList, 0, 0);
    lv_obj_set_style_pad_row(gList, 2, 0);
    lv_obj_set_flex_flow(gList, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(gList, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align_to(gList, sub, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);
    lv_obj_set_x(gList, 8);

    bool shared = display.board() == CYDR_BOARD_SHARED_TOUCH;
    addLine(shared ? "Touch: XPT2046 on display SPI" : "Touch: XPT2046");
    addLine(String("Calibration: ") + (lv_setup.touch().loadCalibration() ? "saved" : "defaults"));
    addLine(String("Backlight: GPIO") + (shared ? 27 : 21));
    auto pin = [](int8_t p) { return p < 0 ? String("?") : String(p); };
    addLine("R/G/B LED: GPIO" + pin(display.redLedPin()) + "/" +
            pin(display.greenLedPin()) + "/" + pin(display.blueLedPin()));
    addLine("SD card: " + sdText());
    lblBattery = addLine(batteryText());
    lblLight = addLine(lightText());
    lblDark = gFacts.lightSensor ? addLine(darkText()) : nullptr;
    addLine("Amplifier: yes");
    addLine(String("Amp power switch: ") +
            (gFacts.ampSwitch ? (gKnown ? "GPIO4" : "GPIO4?") : yesNo(false)));

    lv_obj_t *hint = lv_label_create(scr);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x909090), 0);
    lv_label_set_text(hint, "Draw anywhere.  BOOT clears.");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -6);
}

// Render whatever LVGL has marked dirty, then wait for the last region to go
// out so direct draws can't collide with it.
static void renderNow() {
    lv_refr_now(NULL);
    display.waitFlush();
}

// A small square straight to the panel, through one of LVGL's render buffers
// (DMA-capable, and idle while LVGL isn't rendering).
static void drawDot(int x, int y) {
    const int r = 2;
    int x1 = max(0, x - r), y1 = max(0, y - r);
    int x2 = min((int)display.width() - 1, x + r), y2 = min((int)display.height() - 1, y + r);
    uint16_t *b = lv_setup.drawBuffer(0);
    for (int i = 0; i < (x2 - x1 + 1) * (y2 - y1 + 1); i++) b[i] = C_DOT;
    display.flushRegion(x1, y1, x2, y2, b);
    display.waitFlush();
}

// Set a label's text only when it changed, so LVGL repaints (and wipes the
// drawing under) as little as possible.
static void updateLabel(lv_obj_t *l, const String &text) {
    if (l && text != lv_label_get_text(l)) lv_label_set_text(l, text.c_str());
}

void setup() {
    Serial.begin(115200);

    // Detection happens here, in display.begin().  Start WiFi (if a sketch
    // uses it) only after this: the red-LED check needs ADC2.
    lv_setup.begin();
    pinMode(BOOT_PIN, INPUT_PULLUP);
    gFacts = factsFor(display.model());

    buildScreen();
    renderNow();          // LVGL draws the screen once; from here loop() draws dots
}

void loop() {
    uint16_t x, y;
    if (lv_setup.touch().read(x, y)) drawDot(x, y);

    // BOOT clears the drawing by re-rendering the whole LVGL screen.
    if (digitalRead(BOOT_PIN) == LOW) {
        lv_obj_invalidate(lv_screen_active());
        renderNow();
        while (digitalRead(BOOT_PIN) == LOW) delay(10);
    }

    // Once a second, refresh the live lines (LVGL repaints only what changed).
    static uint32_t lastLive = 0;
    if (millis() - lastLive > 1000) {
        lastLive = millis();
        updateLabel(lblBattery, batteryText());
        updateLabel(lblLight, lightText());
        if (lblDark) updateLabel(lblDark, darkText());
        renderNow();
    }
}
