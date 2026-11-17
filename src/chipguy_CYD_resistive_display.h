/*
 * ILI9341 / ST7789 / ST7796 SPI display driver for the resistive-touch
 * "Cheap Yellow Display" family:
 *   - ESP32-2432S028          2.8"  240x320  (the original CYD, micro-USB)
 *   - ESP32-32E 2.8"          2.8"  240x320  (USB-C, battery connector)
 *   - ESP32-32E 3.2"          3.2"  240x320
 *   - ESP32-32E 3.5" / 4.0"   320x480
 *   - ESP32-024               2.4"  240x320  (battery connector, FM5324GA)
 *   - ESP32-035               3.5"  320x480  (battery connector, light sensor)
 *   - ESP32-2432S032          3.2"  240x320  (and the capacitive -2432S032C)
 *   - ESP32-2432S024C         2.4"  240x320  (capacitive, CST820)
 *   - Guition JC2432W328      2.8"  240x320  (capacitive, CST820)
 *   - ESP32-3248S035C         3.5"  320x480  (capacitive, GT911, micro-USB)
 *   - NM-CYD-C5               2.8"  240x320  (ESP32-C5; see "Which chip" below)
 * The ESP32-32E boards are generic, sold by several vendors; their silkscreen
 * is three lines, e.g. "2.8" LCD Display / ESP32-32E 240x320 / Resistance
 * Touch" -- "3.2" ... 240x320", "3.5" ... 320x480", "4.0" ... 320x480" on
 * the others.
 * Capacitive touch is supported where the chip sits on I2C SDA 33 / SCL 32: a
 * GT911 (the ESP32-2432S032C, the resistive ESP32-2432S032's PCB with a GT911
 * fitted, and the ESP32-3248S035C) or a CST820 (the ESP32-2432S024C and JC2432W328).  begin() probes
 * for both on every board.
 *
 * Two things vary independently, and begin() detects both:
 *   - The WIRING (cydr_board_t), told by which backlight pin carries a
 *     pull-down:
 *       OWN_TOUCH    (2432S028, 32E 2.8"): backlight GPIO21, XPT2046 on its
 *                    own pins
 *       SHARED_TOUCH (32E 3.2", 3.5", 4.0", ESP32-024, ESP32-035): backlight GPIO27,
 *                    XPT2046 on this display's SPI bus
 *                    (ESP32-2432S032 too; its -C has a GT911 on I2C instead)
 *   - The PANEL (cydr_panel_t), told by its ID registers (SDO on GPIO12, or
 *     read back on SDA, GPIO13, on the ESP32-2432S032, which has no SDO):
 *       ILI9341 (BGR, columns mirrored), 240x320 — 2.8" and 2.4"
 *       ST7789  (RGB, columns not mirrored), 240x320 — 2.8" and 2.4"
 *               (inversion off), 3.2" and ESP32-2432S032 (inversion on)
 *       ST7796  (BGR), 320x480 — 3.5", 4.0", ESP32-035
 *     The panel alone decides the geometry.  See _detectPanel(); setPanel()
 *     overrides it.
 *   The red LED pin (GPIO4 or 22, by GPIO4's voltage) then tells the boards
 *   that share a wiring and panel apart: cydr_model_t.
 *
 * Drives the panel through the ESP-IDF SPI master driver, so the bus is shared
 * cleanly with other ESP-IDF SPI devices -- on the shared-touch boards the
 * XPT2046 is one.  Designed to feed an LVGL display in PARTIAL render mode:
 * LVGL hands us a rendered region and we push it with flushRegion().
 *
 * Classic (ESP32) board wiring (same on every board unless noted):
 *   TFT_MOSI 13   TFT_SCLK 14   TFT_CS 15   TFT_DC 2   TFT_MISO 12 (panel SDO)
 *   TFT_BL   21 (own-touch) or 27 (shared-touch); backlight, active HIGH
 *   Panel reset is not on a GPIO, so the driver issues a software reset.
 *   Touch (XPT2046), see chipguy_CYD_resistive_touch:
 *     own-touch:     CLK 25  MOSI 32  MISO 39  CS 33  IRQ 36
 *     shared-touch:  this display's bus (13/14/12), CS 33  IRQ 36
 *                    (the ST7789 ESP32-024's XPT2046 has its X and Y swapped)
 */

#pragma once

#include <Arduino.h>
#include <driver/spi_master.h>

// ── Which chip ─────────────────────────────────────────────────────────────
// Three chips, told apart at compile time:
//   - ESP32 (the classic CYDs above): detected at run time as described.
//   - ESP32-C5: the NM-CYD-C5 (RockBase IoT / NMTech), a 2.8" 240x320 CYD
//     with the same layout.  The build names the board; begin() still asks
//     the panel for its ID (ST7789 as shipped; an ILI9341 fits too).
//     Wiring, all on one SPI bus (SPI2):
//       TFT  MOSI 7  SCLK 6  MISO 2 (panel SDO)  CS 23  DC 24  BL 25
//       XPT2046  CS 1  IRQ 3 (PENIRQ, 10k pull-up)   TF card CS 10
//     Panel reset is on EN, as on the classic boards.
//   - ESP32-S3: the black 2.8" CYD-style board whose silkscreen reads
//     "2.8" LCD Display / ESP32-S3 240x320 / Capacitive Touch" (the vendor's
//     ES3C28P; the ES3N28P is the same board without touch).  begin() tells
//     S3 boards apart by what answers on I2C; see
//     chipguy_CYD_board_ESP32S3.cpp.  Wiring:
//       TFT  MOSI 11  SCLK 12  MISO 13 (panel SDO)  CS 10  DC 46  BL 45
//       FT6336G touch, I2C SDA 16 / SCL 15 (shared with the ES8311 codec),
//         INT 17, RST 18
//     Panel reset is on EN (CHIP_PU).
#if defined(CONFIG_IDF_TARGET_ESP32C5)
#define CYDR_CHIP_C5 1
#define CYDR_CHIP_S3 0
#elif defined(CONFIG_IDF_TARGET_ESP32S3)
#define CYDR_CHIP_C5 0
#define CYDR_CHIP_S3 1
#elif defined(CONFIG_IDF_TARGET_ESP32)
#define CYDR_CHIP_C5 0
#define CYDR_CHIP_S3 0
#else
#error "chipguy_ESP32-CYD-Resistive_display supports the ESP32 CYDs, the ESP32-C5 NM-CYD-C5 and the ESP32-S3 ES3C28P only."
#endif
// The classic ESP32 boards: run-time detection by backlight pull-downs and GPIO4.
#define CYDR_CHIP_CLASSIC (!CYDR_CHIP_C5 && !CYDR_CHIP_S3)

// ── Board-option guard ─────────────────────────────────────────────────────
// The Arduino IDE does not persist board options reliably.  So refuse, at
// compile time, the options that make a bad build for the chip.  ARDUINO_FQBN
// is defined by the core from the build's FQBN; option matching is exact: the
// name=value must be followed by a comma or the end of the string.
static constexpr bool cydr_fqbnHasOption(const char *s, const char *opt) {
    for (; *s; ++s) {
        const char *a = s; const char *b = opt;
        while (*a && *b && *a == *b) { ++a; ++b; }
        if (!*b && (*a == ',' || *a == 0)) return true;
    }
    return false;
}
#if CYDR_CHIP_C5
// The NM-CYD-C5's module (ESP32-C5-WROOM-1-N16R8) has 8 MB of PSRAM on pins
// inside the module, so enabling it costs no GPIO -- and the C5 has less
// internal SRAM to spare than the classic ESP32, so the examples put LVGL's
// heap in PSRAM (see their lv_conf.h).  The core's default is PSRAM disabled,
// and enabling it is what defines BOARD_HAS_PSRAM, so its absence catches an
// FQBN that leaves the option out as well as one that disables it.
#ifndef BOARD_HAS_PSRAM
#error "PSRAM is disabled. The NM-CYD-C5 has 8 MB of PSRAM and this library's LVGL set-up uses it: select Tools > PSRAM > Enabled (or PSRAM=enabled in the --fqbn)."
#endif
#elif CYDR_CHIP_S3
// The ES3C28P's chip is an ESP32-S3R8: 8 MB of octal (OPI) PSRAM inside the
// chip, and 16 MB of flash beside it.  "QSPI PSRAM" on an octal part fails
// at boot; OPI or Disabled both work (internal SRAM is plenty for LVGL at
// 240x320).  And the board has no USB-serial chip: the USB-C port is the
// S3's own USB, so with USB CDC On Boot disabled, Serial goes to UART0 on
// the 4-pin header and the Serial Monitor shows nothing.  A sketch that
// really wants Serial on UART0 can define CYDR_SERIAL_ON_UART0 before
// including this header.
#ifdef ARDUINO_FQBN
static_assert(!cydr_fqbnHasOption(ARDUINO_FQBN, "PSRAM=enabled"),
    "PSRAM is set to QSPI. This board's ESP32-S3R8 has octal PSRAM: select "
    "Tools > PSRAM > OPI PSRAM (or PSRAM=opi in the --fqbn), or Disabled.");
#endif
#if !defined(CYDR_SERIAL_ON_UART0) && (!defined(ARDUINO_USB_CDC_ON_BOOT) || !ARDUINO_USB_CDC_ON_BOOT)
#error "USB CDC On Boot is disabled, so Serial would go to the header's UART pins, not the USB-C port. Select Tools > USB CDC On Boot > Enabled (or CDCOnBoot=cdc in the --fqbn)."
#endif
#else
// Every classic board this library drives has 4 MB of flash and no PSRAM.
// "ESP32 Dev Module" offers larger flash sizes and PSRAM in its menus.  Either
// mistake is a bad time: a flash size larger than the chip produces an image
// the bootloader won't run, and PSRAM enabled on a board without it makes the
// core probe for it at boot, log about the PSRAM GPIOs (16/17 — this board's
// LED pins) being claimed by something else, and tie up those pins.  An FQBN
// that omits an option is left alone: it gets the board's default, which is
// right for both.
#ifdef ARDUINO_FQBN
static_assert(!cydr_fqbnHasOption(ARDUINO_FQBN, "FlashSize=8M") &&
              !cydr_fqbnHasOption(ARDUINO_FQBN, "FlashSize=16M") &&
              !cydr_fqbnHasOption(ARDUINO_FQBN, "FlashSize=32M"),
    "Flash size is set larger than 4 MB. This board has 4 MB of flash: select "
    "Tools > Flash Size > 4MB (or FlashSize=4M in the --fqbn).");
static_assert(!cydr_fqbnHasOption(ARDUINO_FQBN, "PSRAM=enabled"),
    "PSRAM is enabled. This board has no PSRAM, and enabling it claims GPIO16/17 and "
    "logs errors at boot: select Tools > PSRAM > Disabled (or PSRAM=disabled in the --fqbn).");
#endif
#ifdef BOARD_HAS_PSRAM
#error "BOARD_HAS_PSRAM is defined: this board has no PSRAM. Select Tools > PSRAM > Disabled."
#endif
#endif

// SPI host the display runs on.  On the shared-touch boards the touch driver adds the
// XPT2046 to this same host as a second device.
#define CYDR_TFT_SPI_HOST  SPI2_HOST   // HSPI

// Default native geometry (240x320 portrait), used before begin().  The
// panel actually fitted is reported at run time by nativeWidth() /
// nativeHeight(), and width()/height() reflect the active rotation.
#define CYDR_TFT_NATIVE_WIDTH   240
#define CYDR_TFT_NATIVE_HEIGHT  320

// Normalize a rotation argument to an index 0..3.  Accepts either the index
// directly (0..3) or degrees (0/90/180/270) as aliases for 0/1/2/3.
static inline uint8_t cydr_normalize_rotation(uint16_t rotation) {
    switch (rotation) {
        case 90:  return 1;
        case 180: return 2;
        case 270: return 3;
        default:  return (uint8_t)(rotation & 0x03);
    }
}

// Which panel controller is fitted.  PANEL_AUTO (the default) detects it in
// begin().
enum cydr_panel_t : uint8_t {
    CYDR_PANEL_AUTO = 0,
    CYDR_PANEL_ILI9341,      // 240x320 (2.8")
    CYDR_PANEL_ST7789,       // 240x320 (2.8", 3.2")
    CYDR_PANEL_ST7796,       // 320x480 (3.5", 4.0", ESP32-035)
};

// How the board is wired, which decides the backlight pin and the touch bus.
// Independent of the panel: the 3.2" has the shared wiring with a 240x320
// ST7789.  The panel alone decides the geometry.
enum cydr_board_t : uint8_t {
    CYDR_BOARD_UNKNOWN = 0,
    CYDR_BOARD_OWN_TOUCH,    // backlight 21, XPT2046 on its own pins:
                               //   ESP32-2432S028, ESP32-32E 2.8"
    CYDR_BOARD_SHARED_TOUCH, // backlight 27, XPT2046 on the display bus:
                               //   ESP32-32E 3.2", 3.5", 4.0", ESP32-024, ESP32-035,
                               //   ESP32-2432S032 / -C
};

// The board as told by the backlight pins' pull-downs, without driving
// anything: GPIO21 held LOW against the internal pull-up means OWN_TOUCH,
// GPIO27 held LOW means SHARED_TOUCH.  UNKNOWN if neither (or both).  Leaves both
// pins as reset-state inputs.  Used by the display's begin() and by the touch
// driver when it is started without a display.
cydr_board_t cydr_detectBoardPassive();

// Which board, as told by the wiring, the panel and the red LED pin together.
// The 3.5" and 4.0" are wired and built alike and are not told apart.
enum cydr_model_t : uint8_t {
    CYDR_MODEL_UNKNOWN = 0,  // red LED pin unmeasurable (WiFi held ADC2), or no begin()
    CYDR_MODEL_2432S028,     // own touch, red LED on GPIO4
    CYDR_MODEL_32E_28,       // own touch, amplifier enable on GPIO4
    CYDR_MODEL_32E_32,       // shared touch, ST7789, amplifier enable on GPIO4
    CYDR_MODEL_32E_35_40,    // shared touch, ST7796, amplifier enable on GPIO4
                             //   (also the guess when GPIO4 couldn't be measured)
    CYDR_MODEL_024,          // ESP32-024 2.4": shared touch, ST7789 or ILI9341, red LED on GPIO4
    CYDR_MODEL_035,          // ESP32-035 3.5": shared touch, ST7796, red LED on GPIO4
    CYDR_MODEL_2432S032,     // ESP32-2432S032 3.2": shared touch, ST7789 with no SDO
                             //   (answers only on SDA), red LED on GPIO4
    CYDR_MODEL_2432S032C,    // ESP32-2432S032C: the same PCB with GT911 capacitive
                             //   touch (I2C SDA 33 / SCL 32) instead of the XPT2046
    CYDR_MODEL_2432S024C,    // ESP32-2432S024C 2.4": ILI9341, backlight 27, CST820
                             //   capacitive touch (I2C SDA 33 / SCL 32; INT on GPIO21)
    CYDR_MODEL_JC2432W328,   // Guition JC2432W328 2.8": ST7789, backlight 27, CST820
                             //   capacitive touch (I2C SDA 33 / SCL 32)
    CYDR_MODEL_3248S035C,    // ESP32-3248S035C 3.5": ST7796 that answers no ID
                             //   reads, backlight 27, GT911 capacitive touch
    CYDR_MODEL_NM_CYD_C5,    // NM-CYD-C5 2.8" (ESP32-C5): ST7789, backlight 25,
                             //   XPT2046 on the display bus; named by the build
    CYDR_MODEL_ES3C28P,      // ES3C28P 2.8" (ESP32-S3, black PCB): ILI9341V IPS,
                             //   backlight 45, FT6336G capacitive touch (I2C
                             //   SDA 16 / SCL 15), ES8311 codec on the same bus
    CYDR_MODEL_ES3N28P,      // ES3N28P: the ES3C28P without touch
    CYDR_MODEL_COUNT         // not a model: one past the last
};
// Values are stored by number in the model override, so new models go at the end.

// Which chip a model's board carries: 0 the classic ESP32, 1 the ESP32-C5,
// 2 the ESP32-S3.  A model can only be named (overridden) on its own chip.
static inline uint8_t cydr_modelChip(cydr_model_t m) {
    return m == CYDR_MODEL_NM_CYD_C5 ? 1 :
           m == CYDR_MODEL_ES3C28P || m == CYDR_MODEL_ES3N28P ? 2 : 0;
}
#define CYDR_THIS_CHIP (CYDR_CHIP_C5 ? 1 : CYDR_CHIP_S3 ? 2 : 0)

// The touch controller: the XPT2046 (resistive) unless a capacitive chip
// answers on I2C (SDA 33 / SCL 32 on the classic boards; SDA 16 / SCL 15 on
// the ESP32-S3 board).
enum cydr_touch_chip_t : uint8_t {
    CYDR_TOUCH_XPT2046 = 0,
    CYDR_TOUCH_GT911,        // 0x5D or 0x14 (ESP32-2432S032C, ESP32-3248S035C)
    CYDR_TOUCH_CST820,       // 0x15, Hynitron CST816 / CST820 family (2432S024C, JC2432W328)
    CYDR_TOUCH_FT6336,       // 0x38, FocalTech FT6336G (ES3C28P)
    CYDR_TOUCH_NONE,         // no touch fitted (ES3N28P); read() never reports a press
};

// Probe the board's touch I2C bus for a capacitive touch chip.  Classic
// boards: SDA 33 / SCL 32, CST820 at 0x15, then GT911 at 0x5D / 0x14, using
// Wire1.  ESP32-S3: SDA 16 / SCL 15, FT6336 at 0x38, using Wire (the codec
// shares the bus).  XPT2046 (0) if none answers -- CYDR_TOUCH_NONE on the S3,
// which has no XPT2046; the address in *addr.  Starts and ends the Wire
// object and leaves the pins reset.  display.begin() runs it.
cydr_touch_chip_t cydr_probeI2cTouch(uint8_t *addr = nullptr);
// The touch chip and its I2C address chosen by the most recent display
// begin() (XPT2046 / 0 before that); the touch driver follows them.
cydr_touch_chip_t cydr_activeTouchChip();
uint8_t cydr_activeTouchAddress();

// Held for each capacitive touch chip I2C transaction (start to stop), and by
// a panel readback on GPIO12 on a capacitive board.  Some capacitive boards
// keep the XPT2046 fitted with its CS on GPIO33 -- the touch chip's SDA -- and
// its DOUT on GPIO12, the panel's SDO: an I2C transfer during a readback
// selects it, and it can drive GPIO12 against the panel.  Between transfers
// SDA idles high, deselecting it.  See README-engineering.md, "Readback".
void cydr_touchI2cLock();
void cydr_touchI2cUnlock();
// True when a panel readback has to take the lock: capacitive touch, and
// readback on GPIO12.
bool cydr_readbackNeedsTouchLock();

// The battery chip's power key (SW1 on the boards that have one), and how it
// turns the board off on battery.  It always turns the board on with one press.
// The key isn't wired to the ESP32: a sketch can't see or press it, only tell
// the user about it.
enum cydr_power_key_t : uint8_t {
    CYDR_PWRKEY_UNKNOWN = 0,     // model unknown
    CYDR_PWRKEY_NONE,            // no power key -- or one the power-key override
                                 //   reports absent (e.g. hidden by an enclosure)
    CYDR_PWRKEY_DOUBLE_PRESS,    // two quick presses (FM5324GA; some IP5306s)
    CYDR_PWRKEY_LONG_HOLD,       // a ~10-second hold (other IP5306s)
    CYDR_PWRKEY_EITHER,          // IP5306 boards seen both ways: the chip variant decides
};

// How the panel's frame memory can be read back (display.readback()), as
// tested by begin(): four pixels written and read back exactly.
enum cydr_readback_t : uint8_t {
    CYDR_READBACK_NONE = 0,   // unavailable: no read line answers (ESP32-3248S035C)
    CYDR_READBACK_MISO,       // on MISO, the panel's SDO: GPIO12 (most boards),
                              //   GPIO2 (NM-CYD-C5), GPIO13 (ES3C28P)
    CYDR_READBACK_SDA,        // on GPIO13, shared with MOSI: the panel's
                              //   bidirectional SDA (ESP32-2432S032 / C).  The
                              //   display's SPI device is 3-wire half-duplex.
};

// Screen diagonal sizes the size override can name, in tenths of an inch.
// Index 0 is "unknown"; the override stores 1..5.
static const uint8_t cydr_sizeTenths[] = {0, 24, 28, 32, 35, 40};

// The board and panel found by the most recent display begin() (UNKNOWN /
// AUTO before that).
cydr_board_t cydr_activeBoard();
cydr_panel_t cydr_activePanel();
cydr_model_t cydr_activeModel();
// Where the most recent display begin() put the XPT2046 (the wiring, unless
// overridden); the touch driver's begin() without a display uses it.
cydr_board_t cydr_activeTouchBus();

// The RGB LED's red pin (active LOW): 4 on the ESP32-2432S028 and ESP32-024,
// 22 on the ESP32-32E 2.8" / 3.2" / 3.5" / 4.0", where GPIO4 is the speaker
// amplifier's shutdown pin instead.  Told apart by GPIO4's voltage against
// the internal pull-down (see README-board-detection-ESP32.md).  GPIO4 is on ADC2,
// which WiFi takes over, so the answer is measured once and latched:
// display.begin() does it, and any later call returns the latched value.
// Call it (or begin()) before WiFi starts and before anything drives GPIO4.
// Returns -1 (not latched) if the first measurement finds ADC2 already taken
// by WiFi.
int8_t cydr_redLedPin();

// The GPIO4 reading behind cydr_redLedPin(), in millivolts: 0 if not yet
// measured.  For diagnostics.
uint32_t cydr_redLedProbeMillivolts();

// ── Overrides ──────────────────────────────────────────────────────────────
// Per-board settings, saved in NVS (namespace "cydrforce"), that replace what
// begin() detects -- for a board variant the detection gets wrong.  Each field
// is CYDR_OVR_AUTO (0, use detection) or one of two values, 1 or 2, as listed.
// The TouchCalibrate example edits them; see README-overrides.md.  They are
// read by display.begin() and touch.begin(), so they take effect on the next
// start.  "Raw" mirror settings are the panel's MADCTL MX / MY bits in
// rotation 0, the other rotations following from them.
#define CYDR_OVR_AUTO 0
struct cydr_overrides_t {
    uint8_t panel;         // only if the panel didn't identify itself:
                           //   1 ILI9341, 2 ST7789, 3 ST7796 (cydr_panel_t values)
    uint8_t colorOrder;    // 1 RGB, 2 BGR
    uint8_t inversion;     // 1 off, 2 on
    uint8_t mirrorX;       // MADCTL MX in rotation 0: 1 clear, 2 set
    uint8_t mirrorY;       // MADCTL MY in rotation 0: 1 clear, 2 set
    uint8_t spiClock;      // 1 27 MHz, 2 20 MHz, 3 80 MHz (auto: 40 MHz).  3 is
                           //   set by TouchCalibrate's speed test when the board
                           //   passes it; see stressTest().
    uint8_t backlight;     // 1 GPIO21, 2 GPIO27
    uint8_t touchBus;      // XPT2046: 1 own pins (25/32/39), 2 the display's bus
    uint8_t touchMirrorX;  // 1 off, 2 on
    uint8_t touchMirrorY;  // 1 off, 2 on
    uint8_t touchSwapXY;   // 1 off, 2 on
    uint8_t redLed;        // 1 GPIO4 (no amplifier switch), 2 GPIO22 (GPIO4 switches the amplifier)
    uint8_t greenBlue;     // 1 green 16 / blue 17, 2 green 17 / blue 16
    uint8_t gpio34;        // 1 light sensor, 2 battery (cell / 2)
    // Added later; a blob saved before them reads them as AUTO.
    uint8_t model;         // 1..CYDR_MODEL_COUNT-1 (cydr_model_t values).  The
                           //   wiring, backlight, red LED, touch chip, inversion,
                           //   touch axes and size then follow the named model;
                           //   the panel is still detected.
    uint8_t size;          // screen diagonal: 1 2.4", 2 2.8", 3 3.2", 4 3.5", 5 4.0"
                           //   (index into cydr_sizeTenths); names the 3.5"
                           //   vs 4.0" 32E, which detection can't tell apart
    uint8_t powerKey;      // 1 absent / hidden, 2 press twice, 3 hold 10 s,
                           //   4 either (cydr_power_key_t values)
};

// Read the saved overrides (all AUTO if none); true if any is set.
bool cydr_loadOverrides(cydr_overrides_t &o);
void cydr_saveOverrides(const cydr_overrides_t &o);
void cydr_clearOverrides();
// True if any field is set.
bool cydr_anyOverride(const cydr_overrides_t &o);

// The last result of the 80 MHz speed test (display.stressTest()) on this
// board, saved in NVS beside the overrides.  TouchCalibrate and
// RasterBouncingBalls write it; a sketch reads it to know whether 80 MHz has
// been checked here.  "Clear all" in TouchCalibrate leaves it: it records the
// hardware, not a setting.
enum cydr_speed_result_t : uint8_t {
    CYDR_SPEED_UNTESTED = 0,
    CYDR_SPEED_PASSED,       // every window read back right
    CYDR_SPEED_FAILED,       // `bad` of `windows` came back wrong
};
struct cydr_speed_test_t {
    uint8_t  result;         // cydr_speed_result_t
    uint32_t hz;             // the clock tested
    int32_t  windows;        // windows sent and read back
    int32_t  bad;            // of those, wrong
    uint8_t  version;        // the test's version; cydr_saveSpeedTest() fills it in
};
// The current test.  A record from an earlier version (a weaker test) loads
// as untested, so sketches run the test again.  2: bulk windows and UART
// noise.  3: LED PWM noise.
#define CYDR_SPEED_TEST_VERSION 3
// False (and result UNTESTED) if no test has been recorded.
bool cydr_loadSpeedTest(cydr_speed_test_t &r);
void cydr_saveSpeedTest(const cydr_speed_test_t &r);

// ── Panel refresh rate ─────────────────────────────────────────────────────
// The panel redraws itself from its own memory on its own oscillator (about
// 94 Hz on the ILI9341 measured).  Its controller can slow that down, which
// is what lets a sketch send whole frames without tearing at 40 MHz (see
// the RasterBouncingBalls example).  A slower refresh costs picture quality:
// 50 % greys, thin stripes and checkerboards start to flicker or shimmer
// first.  So the library never changes the rate by itself: display.begin()
// leaves the controller's stock rate, and only a sketch that calls
// setRefreshStep() or applyRefreshChoice() gets another.
//
// Nothing below this is offered: below it each pixel's charge is held long
// enough that flicker and DC stress on the liquid crystal grow.  A
// conservative floor chosen for this library, not a datasheet limit.
#define CYDR_MIN_REFRESH_HZ 30.0f

// A slower rate the user accepted, saved in NVS: the step (see
// setRefreshStep()), the stock rate it was measured against, and the rate
// it gave.  Only applyRefreshChoice() uses it.
struct cydr_refresh_choice_t {
    uint8_t panel;           // cydr_panel_t it was chosen on (ignored on another)
    uint8_t step;
    float   stockHz;
    float   hz;
};
bool cydr_loadRefreshChoice(cydr_refresh_choice_t &c);   // false if none
void cydr_saveRefreshChoice(const cydr_refresh_choice_t &c);
void cydr_clearRefreshChoice();

// The RGB LED's green and blue pins (active LOW): 16 and 17 on every board
// except the ESP32-024 and ESP32-035, where they are the other way round
// (17 and 16).
// Valid after display.begin(), since they follow from the model.  -1 before
// that, or when the model is unknown and the board could be either a 32E 3.2"
// or an ESP32-024 (shared-touch 240x320 with no GPIO4 reading).
int8_t cydr_greenLedPin();
int8_t cydr_blueLedPin();
// The addressable (WS2812) RGB LED's data pin, for the core's rgbLedWrite():
// 27 on the NM-CYD-C5, 42 on the ES3C28P / ES3N28P, -1 on the classic boards
// (they have a plain RGB LED; see cydr_redLedPin()).
int8_t cydr_rgbLedPin();

// The TF / microSD card's chip-select pin: 5 on the classic boards (the
// card on its own VSPI bus: SD.begin(cydr_sdCsPin())), 10 on the NM-CYD-C5
// (the card on the display's SPI bus, so not Arduino's SD -- see
// README-sdcard-ESP32-C5.md), -1 on the ESP32-S3 ES3C28P (the card is
// SDMMC, no CS: README-sdcard-ESP32-S3.md).  Fixed per chip; needs no
// begin().
int8_t cydr_sdCsPin();

// The GT911's I2C address (0x5D or 0x14) on SDA 33 / SCL 32, or 0 if none
// answers.  display.begin() uses it to tell the ESP32-2432S032C from the
// ESP32-2432S032; it starts and ends Wire1.
uint8_t cydr_gt911Address();

class chipguy_CYD_resistive_display {
public:
    chipguy_CYD_resistive_display();

    // Force the panel type instead of detecting it.  Call before begin().
    // The geometry follows from the panel (ST7796 = 320x480, others =
    // 240x320); the board wiring is still detected.
    void setPanel(cydr_panel_t panel) { _panel = panel; }

    // The panel in use: after begin(), the detected (or forced) type.
    cydr_panel_t panel() const { return _panel; }
    const char *panelName() const;

    // The board in use, after begin().
    cydr_board_t board() const { return _board; }

    // The board model, after begin(), and its name ("ESP32-024 2.4\"", ...).
    // The name of the 32E 3.5" / 4.0" names one size when the size override
    // does.
    cydr_model_t model() const { return _model; }
    const char *modelName() const;
    static const char *modelName(cydr_model_t m, uint8_t sizeTenths = 0);

    // The screen diagonal in tenths of an inch (24, 28, 32, 35, 40), after
    // begin(): from the model, or the size override.  0 when unknown -- which
    // includes the 32E 3.5" / 4.0", whose two sizes are built alike.
    uint8_t diagonalTenths() const { return _sizeTenths; }

    // The power key and how it turns the board off, after begin(): from the
    // model, or the power-key override.  powerOffHint() is text for telling a
    // user, e.g. "hold SW1 for 10 s"; nullptr when there is no key (or it is
    // reported absent) or the model is unknown.  It applies on battery: with
    // USB connected the board stays powered.
    cydr_power_key_t powerKey() const { return _powerKey; }
    const char *powerOffHint() const;

    // The touch controller found (or implied by a model override), after
    // begin(), and its I2C address (0 for the XPT2046).
    cydr_touch_chip_t touchChip() const { return _touchChip; }
    uint8_t touchAddress() const { return _touchAddr; }

    // The RGB LED's pins, all active LOW; see cydr_redLedPin() and
    // cydr_greenLedPin().  Red is 4 or 22, green / blue 16 / 17 (17 / 16 on
    // the ESP32-024); -1 if unknown.
    int8_t redLedPin()   const { return cydr_redLedPin(); }
    int8_t greenLedPin() const { return cydr_greenLedPin(); }
    int8_t blueLedPin()  const { return cydr_blueLedPin(); }
    // The WS2812 RGB LED's pin (NM-CYD-C5: 27), or -1 on the classic boards.
    int8_t rgbLedPin()   const { return cydr_rgbLedPin(); }
    // The card slot's CS pin (5, 10, or -1 for SDMMC); see cydr_sdCsPin().
    int8_t sdCsPin()     const { return cydr_sdCsPin(); }

    // Unrotated physical dimensions of the fitted panel (240x320 or 320x480),
    // valid after begin().
    int16_t nativeWidth()  const { return _nativeW; }
    int16_t nativeHeight() const { return _nativeH; }

    // Initialize the SPI bus, detect and initialize the panel, and turn on
    // the backlight.
    // rotation: 0 = portrait (native), 1 = landscape, 2 = portrait flipped,
    //           3 = landscape flipped.  Degrees (0/90/180/270) are accepted as
    //           aliases for 0/1/2/3.
    // Returns false if the SPI bus/device could not be set up.
    // The bus is set up with MISO (GPIO12): the shared-touch boards' XPT2046
    // shares it.  Call this before the touch driver's begin().
    bool begin(uint16_t rotation = 0);

    // Before begin(): whether to apply the overrides saved in NVS (default
    // true).  TouchCalibrate turns them off for its recovery start.
    void useOverrides(bool on) { _useOverrides = on; }
    // Before begin(): the panel's SPI clock, overriding both the default
    // 40 MHz and any saved override (0 = no change).  TouchCalibrate runs at
    // 20 MHz so it stays usable whatever the clock override says.  80 MHz
    // (80000000) passed stressTest() on the boards tried, but isn't the
    // default; stressTest() tells for a given board.  See
    // README-engineering.md, "The SPI clock".
    void setSpiClock(uint32_t hz) { _spiHzForced = hz; }

    // After begin(): the overrides in effect (all AUTO if none, or if
    // useOverrides(false)), and what detection chose for each display-side
    // field, in the same 1 / 2 encoding (touch fields left 0; see the touch
    // driver's autoValues()).  panel is 0 when the panel identified itself.
    const cydr_overrides_t &overrides() const { return _ovr; }
    void autoValues(cydr_overrides_t &o) const;
    // True if no panel ID was recognised and the type was guessed from the
    // wiring -- the only case the panel override applies to.
    bool panelGuessed() const { return _panelGuessed; }
    // Whether, and on which pin, pixels can be read back from the panel
    // (tested in begin()); readbackName(): "GPIO12", "shared GPIO13" or
    // "unavailable".  The text terminal's cursor uses it over old text.
    cydr_readback_t readback() const { return _readback; }
    const char *readbackName() const;

    // After begin(), change these live (not saved); for previewing overrides.
    void setColorOrder(bool bgr);
    void setInversion(bool on);
    void setMirror(bool mx, bool my);   // MADCTL MX / MY in rotation 0
    bool colorBGR()  const { return _bgr; }
    bool inversion() const { return _invert; }
    bool mirrorX()   const { return _mx0; }
    bool mirrorY()   const { return _my0; }
    // Whether the current rotation's MADCTL has MY set (frame memory rows run
    // bottom-to-top on screen).  The text terminal's scrolling needs it.
    bool rowsReversed() const { return _madctl & 0x80; }
    uint32_t spiClock() const { return _spiHz; }
    // The core the SPI bus's interrupt runs on (the one begin() ran on).  A
    // task that holds the bus with spi_device_acquire_bus() and queues
    // transfers should run on this core: from the other core the text
    // terminal crashed in spi_bus_lock_bg_exit() on an ESP32-2432S032
    // (2026-11-02); see README-engineering.md, "Resources".
    int spiCore() const { return _spiCore; }
    int8_t backlightPin() const { return _blPin; }

    // Where the XPT2046 is wired: CYDR_BOARD_OWN_TOUCH or _SHARED_TOUCH.
    // Follows the board's wiring unless overridden.
    cydr_board_t touchBus() const { return _touchBus; }

    // What GPIO34 carries: 1 the battery (cell / 2, the ESP32-32E boards),
    // 0 a light sensor, -1 unknown (model unknown and not overridden).
    int8_t gpio34IsBattery() const;

    // Change rotation after begin().  Updates width()/height() and the
    // panel's MADCTL.  Accepts an index 0..3 or degrees 0/90/180/270.
    void setRotation(uint16_t rotation);

    // Logical dimensions for the current rotation.
    int16_t width()  const { return _w; }
    int16_t height() const { return _h; }

    // Current rotation (0..3).
    uint8_t rotation() const { return _rotation; }

    // Backlight brightness, 0..100 (active HIGH on every board).
    void setBacklight(int percentage);

    // Push a rectangular region of RGB565 pixels to the panel.  Coordinates
    // are inclusive.  `pixels` is in LVGL's native little-endian byte order;
    // this call byte-swaps it in place to the big-endian order the panel
    // expects, so the buffer is modified.
    //
    // Asynchronous: it byte-swaps, waits for the PREVIOUS flush's DMA to finish,
    // sends the address window, then QUEUES this region's pixel DMA and returns
    // before it completes.  With double buffering, the caller can render the
    // next region into the other buffer while this one transmits.  `pixels`
    // must stay valid and untouched until the next flushRegion()/waitFlush().
    void flushRegion(int x1, int y1, int x2, int y2, uint16_t *pixels);

    // Block until the most recently queued flush has finished transmitting.
    // Call before tearing down or doing direct drawing after a flush.  The
    // touch driver calls it before using the shared bus.
    void waitFlush();

    // Fill the whole screen with one RGB565 color (handy for bring-up tests).
    void fillScreen(uint16_t color);

    // SPI stress test.  Fills the screen with random pixels, then for `ms`
    // milliseconds sends windows of random pixels with the bus clocked at
    // `hz`, reading each back from the panel at 2 MHz and comparing.  Two
    // kinds, alternating:
    //   - small windows, random size up to the full width and 40 rows,
    //     through flushRegion(): the first, last and one middle row of
    //     every window checked, and every row of every fourth;
    //   - bulk windows, full width and up to the whole screen tall, streamed
    //     as back-to-back queued DMAs the way a frame-streaming sketch sends
    //     them: first, last and two random rows checked.  Rare byte losses
    //     only show up in a stream like this.
    // Where Serial is a UART (the classic and C5 boards), the test also keeps
    // its TX line busy with high-edge-density characters during the DMAs, as
    // a working sketch's logging would: a line of "UUUU****????pppp~~~~"
    // junk appears on the Serial Monitor meanwhile.  And it flashes the RGB
    // LED, every 50 ms switching between equal brightness on all three colors
    // (every PWM edge together: the biggest current spikes) and three
    // different ones (the edges spread out); a WS2812 (C5, S3) between white
    // and a mixed color.  The LED is left off afterwards.
    // Returns the number of windows with any pixel wrong (0 = passed), or -1
    // if this board can't read pixels back (readback() == CYDR_READBACK_NONE)
    // or a buffer couldn't be allocated; *windows, if given, gets the number
    // tested.  The screen is noise afterwards: redraw it (LVGL:
    // lv_obj_invalidate(lv_screen_active())).  progress, if given, is called
    // after each window with the elapsed and total milliseconds.
    // TouchCalibrate's speed test runs it at 80 MHz.
    int stressTest(uint32_t hz, uint32_t ms = 20000,
                   void (*progress)(uint32_t elapsedMs, uint32_t totalMs) = nullptr,
                   int *windows = nullptr);

    // A hook that samples touch between display writes, while the bus is
    // idle.  The touch driver installs it on the boards where the XPT2046
    // shares this bus (see chipguy_CYD_resistive_touch.h, "Sampling between
    // display writes"); flushRegion() runs it before queueing each region, and
    // the text terminal's task between its steps.  It returns at once unless
    // the screen is pressed and the last sample is old.  Run it only while
    // holding nothing on the bus.
    void setTouchSampler(void (*fn)(void *), void *arg) { _samplerArg = arg; _sampler = fn; }
    void runTouchSampler() { if (_sampler) _sampler(_samplerArg); }

    // ── Driving the panel directly ─────────────────────────────────────────
    // For a sketch that sends pixels from its own task, timed against the
    // panel's scan (the RasterBouncingBalls example).  Don't mix with
    // flushRegion(): call waitFlush() before taking the bus over, and send
    // nothing of your own while a flushRegion() may be in flight.
    //
    // The panel's SPI device.  Its pre-transaction callback sets DC from each
    // transaction's `user` field: (void *)0 a command, (void *)1 data.  Above
    // 60 MHz the classic ESP32 garbles a DMA whose length is 2 mod 4 bytes
    // (see flushRegion()), so keep data transfers a multiple of 4 bytes.
    spi_device_handle_t spiDevice() const { return _spi; }
    // CASET / RASET / RAMWR for a window (inclusive), polled.  Pixel data
    // then goes out as any number of data transactions, big-endian RGB565;
    // the panel's memory write carries on across CS between them.  Nothing
    // of yours may be queued on the device when this is called.
    void setWindow(int x0, int y0, int x1, int y1) { _setAddrWindow(x0, y0, x1, y1); }
    // Change the write clock now, e.g. to 80 MHz once stressTest() has
    // passed.  Not saved (the SPI-clock override is the saved form).
    void changeSpiClock(uint32_t hz);
    // Send command `cmd` and read n (1..4) bytes back on the readback line at
    // `hz`, exactly as clocked: any dummy clock or byte the command has is in
    // them.  E.g. 0x45, the line the panel is scanning.  Polled; takes the
    // bus and, where a readback needs it, the touch I2C lock.  False if the
    // board can't read back (readback() == CYDR_READBACK_NONE).
    bool readCommand(uint8_t cmd, uint8_t *buf, size_t n, uint32_t hz = 4000000);

    // The panel's refresh rate, as the controller's own steps.  Step 0 is
    // the stock rate (what begin() leaves); higher steps are slower.  The
    // controller can't say its rate in hertz -- its oscillator varies from
    // panel to panel -- so each step is given as a ratio to the stock rate,
    // and the stock rate has to be measured (scan line, command 0x45).
    //   ILI9341: FRMCTR1 (0xB1): oscillator / 1, 2, 4 or 8, and 16..31
    //            clocks a line.
    //   ST7789:  FRCTRL2 (0xC6): a line of 250 + 16 * RTNA clocks.
    //   ST7796:  not offered yet (0 steps).
    // refreshStepCount() is 0 when the rate can't be changed.
    int   refreshStepCount() const;
    float refreshStepRatio(int step) const;      // rate / stock rate; 1 for step 0
    // Apply a step now (not saved).  stockHz: the stock rate measured on this
    // panel.  Refused (false) if stockHz * ratio would be under
    // CYDR_MIN_REFRESH_HZ, or stockHz is 0.
    bool  setRefreshStep(int step, float stockHz);
    int   refreshStep() const { return _refreshStep; }
    // Apply the choice saved with cydr_saveRefreshChoice(), if there is one
    // for this panel type.  The only way a saved rate takes effect: call it
    // from a sketch that wants it.
    bool  applyRefreshChoice();

private:
    // The text terminal drives the panel through this object's SPI device.
    friend class chipguy_CYD_resistive_terminal;

    spi_device_handle_t _spi = nullptr;
    cydr_panel_t _panel = CYDR_PANEL_AUTO;
    cydr_board_t _board = CYDR_BOARD_UNKNOWN;
    cydr_model_t _model = CYDR_MODEL_UNKNOWN;
    int16_t _nativeW = CYDR_TFT_NATIVE_WIDTH;
    int16_t _nativeH = CYDR_TFT_NATIVE_HEIGHT;
    int16_t _w = CYDR_TFT_NATIVE_WIDTH;
    int16_t _h = CYDR_TFT_NATIVE_HEIGHT;
    uint8_t _rotation = 0;
    int8_t  _blPin = 21;
    cydr_board_t _touchBus = CYDR_BOARD_UNKNOWN;
    cydr_touch_chip_t _touchChip = CYDR_TOUCH_XPT2046;
    uint8_t _touchAddr = 0;
    uint8_t _sizeTenths = 0;
    cydr_power_key_t _powerKey = CYDR_PWRKEY_UNKNOWN;
    bool    _bgr = true, _invert = false, _mx0 = true, _my0 = false;
    uint8_t _madctl = 0;
    bool    _useOverrides = true, _panelGuessed = false;
    cydr_readback_t _readback = CYDR_READBACK_NONE;
    int     _refreshStep = 0;
    uint8_t _frmStockClocks = 24;         // ILI9341 RTNA as the init sets it
    bool    _refreshStepRegs(int step, uint8_t &a, uint8_t &b, float &ratio) const;
    uint32_t _spiHzForced = 0, _spiHz = 40000000;
    int      _spiCore = 1;
    cydr_overrides_t _ovr = {};
    cydr_overrides_t _auto = {};

    void (*_sampler)(void *) = nullptr;   // see setTouchSampler()
    void *_samplerArg = nullptr;

    spi_transaction_t _pixelTrans = {};   // persistent: queued DMA reads it async
    spi_transaction_t _tailTrans = {};    // an odd-length flush's last pixel (see flushRegion)
    uint8_t _pixelBusy = 0;               // queued flush transactions still in flight

    void _cmd(uint8_t cmd);
    void _data(const uint8_t *data, size_t len);
    void _data8(uint8_t val);
    cydr_panel_t _detectPanel(cydr_board_t boardHint);
    void _initILI9341();
    void _initST7789();
    void _initST7796();
    cydr_readback_t _probeReadback();
    bool _readRow(int x0, int x1, int y, uint8_t *buf, size_t bytes);
    void _setAddrWindow(int x0, int y0, int x1, int y1);
    void _pushPixels(const uint8_t *data, size_t bytes);
    void _waitPixelDone();
};
