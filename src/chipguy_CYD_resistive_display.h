/*
 * ILI9341 / ST7789 / ST7796 SPI display driver for the resistive-touch
 * "Cheap Yellow Display" family:
 *   - ESP32-2432S028          2.8"  240x320  (the original CYD, micro-USB)
 *   - ESP32-32E 2.8"          2.8"  240x320  (USB-C, battery connector)
 *   - ESP32-32E 3.2"          3.2"  240x320
 *   - ESP32-32E 3.5" / 4.0"   320x480
 * The ESP32-32E boards are generic, sold by several vendors; their silkscreen
 * reads e.g. "2.8" LCD Display, ESP32-32E 240x320, Resistance Touch".
 * Capacitive-touch CYDs (e.g. ESP32-2432S032C, GT911) are not supported.
 *
 * Two things vary independently, and begin() detects both:
 *   - The WIRING (cydr_board_t), told by which backlight pin carries a
 *     pull-down:
 *       OWN_TOUCH    (2432S028, 32E 2.8"): backlight GPIO21, XPT2046 on its
 *                    own pins
 *       SHARED_TOUCH (32E 3.2", 3.5", 4.0"): backlight GPIO27, XPT2046 on
 *                    this display's SPI bus
 *   - The PANEL (cydr_panel_t), told by its ID registers (SDO on GPIO12):
 *       ILI9341 (BGR, columns mirrored), 240x320 — 2.8"
 *       ST7789  (RGB, columns not mirrored), 240x320 — 2.8" (inversion off),
 *               3.2" (inversion on)
 *       ST7796  (BGR), 320x480 — 3.5", 4.0"
 *     The panel alone decides the geometry.  See _detectPanel(); setPanel()
 *     overrides it.
 *
 * Drives the panel through the ESP-IDF SPI master driver, so the bus is shared
 * cleanly with other ESP-IDF SPI devices -- on the shared-touch boards the
 * XPT2046 is one.  Designed to feed an LVGL display in PARTIAL render mode:
 * LVGL hands us a rendered region and we push it with flushRegion().
 *
 * Board wiring (same on every board unless noted):
 *   TFT_MOSI 13   TFT_SCLK 14   TFT_CS 15   TFT_DC 2   TFT_MISO 12 (panel SDO)
 *   TFT_BL   21 (own-touch) or 27 (shared-touch); backlight, active HIGH
 *   Panel reset is not on a GPIO, so the driver issues a software reset.
 *   Touch (XPT2046), see chipguy_CYD_resistive_touch:
 *     own-touch:     CLK 25  MOSI 32  MISO 39  CS 33  IRQ 36
 *     shared-touch:  this display's bus (13/14/12), CS 33  IRQ 36
 */

#pragma once

#include <Arduino.h>
#include <driver/spi_master.h>

// ── Board-option guard ─────────────────────────────────────────────────────
// Every board this library drives has 4 MB of flash and no PSRAM.  The Arduino
// IDE does not persist board options reliably, and "ESP32 Dev Module" offers
// larger flash sizes and PSRAM in its menus.  Either mistake is a bad time: a
// flash size larger than the chip produces an image the bootloader won't run,
// and PSRAM enabled on a board without it makes the core probe for it at boot,
// log about the PSRAM GPIOs (16/17 — this board's LED pins) being claimed by
// something else, and tie up those pins.  So refuse at compile time when
// ARDUINO_FQBN — which the core defines from the build's FQBN — names a wrong
// value.  An FQBN that omits an option is left alone: it gets the board's
// default, which is right for both.  Option matching is exact: the name=value
// must be followed by a comma or the end of the string.
static constexpr bool cydr_fqbnHasOption(const char *s, const char *opt) {
    for (; *s; ++s) {
        const char *a = s; const char *b = opt;
        while (*a && *b && *a == *b) { ++a; ++b; }
        if (!*b && (*a == ',' || *a == 0)) return true;
    }
    return false;
}
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
    CYDR_PANEL_ST7796,       // 320x480 (3.5", 4.0")
};

// How the board is wired, which decides the backlight pin and the touch bus.
// Independent of the panel: the 3.2" has the shared wiring with a 240x320
// ST7789.  The panel alone decides the geometry.
enum cydr_board_t : uint8_t {
    CYDR_BOARD_UNKNOWN = 0,
    CYDR_BOARD_OWN_TOUCH,    // backlight 21, XPT2046 on its own pins:
                               //   ESP32-2432S028, ESP32-32E 2.8"
    CYDR_BOARD_SHARED_TOUCH, // backlight 27, XPT2046 on the display bus:
                               //   ESP32-32E 3.2", 3.5", 4.0"
};

// The board as told by the backlight pins' pull-downs, without driving
// anything: GPIO21 held LOW against the internal pull-up means OWN_TOUCH,
// GPIO27 held LOW means SHARED_TOUCH.  UNKNOWN if neither (or both).  Leaves both
// pins as reset-state inputs.  Used by the display's begin() and by the touch
// driver when it is started without a display.
cydr_board_t cydr_detectBoardPassive();

// The board and panel found by the most recent display begin() (UNKNOWN /
// AUTO before that).
cydr_board_t cydr_activeBoard();
cydr_panel_t cydr_activePanel();

// The RGB LED's red pin (active LOW): 4 on the ESP32-2432S028, 22 on the
// ESP32-32E 2.8" / 3.2" / 3.5" / 4.0", where GPIO4 is the speaker amplifier's
// shutdown pin instead.  Told apart by GPIO4's voltage against the internal
// pull-down (see README-board-detection.md).  GPIO4 is on ADC2, which WiFi
// takes over, so the answer is measured once and latched: display.begin()
// does it, and any later call returns the latched value.  Call it (or
// begin()) before WiFi starts and before anything drives GPIO4.  Returns -1
// (not latched) if the first measurement finds ADC2 already taken by WiFi.
int8_t cydr_redLedPin();

// The GPIO4 reading behind cydr_redLedPin(), in millivolts: 0 if not yet
// measured, or on a SHARED_TOUCH board, where the answer needs no measurement.
// For diagnostics.
uint32_t cydr_redLedProbeMillivolts();

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

    // The red LED's GPIO (4 or 22, -1 if unmeasurable); see cydr_redLedPin().  Latched in begin().
    int8_t redLedPin() const { return cydr_redLedPin(); }

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

private:
    spi_device_handle_t _spi = nullptr;
    cydr_panel_t _panel = CYDR_PANEL_AUTO;
    cydr_board_t _board = CYDR_BOARD_UNKNOWN;
    int16_t _nativeW = CYDR_TFT_NATIVE_WIDTH;
    int16_t _nativeH = CYDR_TFT_NATIVE_HEIGHT;
    int16_t _w = CYDR_TFT_NATIVE_WIDTH;
    int16_t _h = CYDR_TFT_NATIVE_HEIGHT;
    uint8_t _rotation = 0;
    int8_t  _blPin = 21;

    spi_transaction_t _pixelTrans = {};   // persistent: queued DMA reads it async
    bool _pixelBusy = false;              // true while a queued flush is in flight

    void _cmd(uint8_t cmd);
    void _data(const uint8_t *data, size_t len);
    void _data8(uint8_t val);
    cydr_panel_t _detectPanel(cydr_board_t boardHint);
    void _initILI9341();
    void _initST7789();
    void _initST7796();
    void _setAddrWindow(int x0, int y0, int x1, int y1);
    void _pushPixels(const uint8_t *data, size_t bytes);
    void _waitPixelDone();
};
