/*
 * XPT2046 resistive touch driver for the resistive-touch CYD family
 * (ESP32-2432S028; ESP32-32E 2.8", 3.2", 3.5", 4.0"; ESP32-024; ESP32-035;
 * ESP32-2432S032), the GT911 of the capacitive ESP32-2432S032C and
 * ESP32-3248S035C, and the CST820 of the capacitive ESP32-2432S024C and
 * JC2432W328.
 *
 * The XPT2046 is wired two different ways, and begin() picks the right one for
 * the board the display driver found:
 *   - OWN_TOUCH (2432S028, 32E 2.8"): on its OWN set of pins, separate from the display bus,
 *     so this driver bit-bangs it — no bus sharing, and it stays well under
 *     the XPT2046's ~2.5 MHz limit.
 *       XPT2046_CLK 25   XPT2046_MOSI 32   XPT2046_MISO 39
 *       XPT2046_CS  33   XPT2046_IRQ  36 (active LOW while touched)
 *   - SHARED_TOUCH (32E 3.2", 3.5", 4.0", ESP32-024): on the DISPLAY's SPI bus (SCLK 14, MOSI 13,
 *     MISO 12), added as a second ESP-IDF SPI device with its own chip-select
 *     and a slow clock.
 *       XPT2046_CS  33   XPT2046_IRQ  36 (active LOW while touched)
 *     Here begin() must come AFTER the display's begin(), which sets up that
 *     bus.  Reads come from samples taken between display writes; see
 *     "Sampling between display writes" below.
 *   - NM-CYD-C5 (ESP32-C5): the display's bus as well (SCLK 6, MOSI 7, MISO 2),
 *       XPT2046_CS  1    XPT2046_IRQ  3 (active LOW while touched)
 *
 * Capacitive boards have a GT911 (ESP32-2432S032C) or CST820 (ESP32-2432S024C,
 * JC2432W328) instead, on I2C (Wire1: SDA 33, SCL 32; RST 25 is left alone and
 * INT isn't used -- both chips are polled).  begin() picks whichever chip the
 * display found (display.touchChip()), and read() returns its points through
 * the same rotation mapping.
 *
 * The ESP32-S3 ES3C28P has an FT6336G at 0x38 on SDA 16 / SCL 15, read on
 * Wire (I2C0, shared with its ES8311 codec); the ES3N28P has no touch, and
 * read() never reports a press (chip() == CYDR_TOUCH_NONE).
 *
 * Capacitive chips need no calibration: setCalibration(),
 * the filters and pressure() don't apply, and loadCalibration() returns false.
 *
 * Resistive panels vary unit-to-unit, so raw ADC readings are mapped to screen
 * pixels through a calibration rectangle.  The defaults work on typical units;
 * call setCalibration() to tune, or readRaw() to gather values for your panel.
 * The calibration rectangle and the read() noise filters can be persisted to
 * NVS (Preferences) and reloaded at startup — see the TouchCalibrate example,
 * which produces and saves both for you.  Each wiring + panel size keeps its
 * own NVS namespace; the 320x480 one is the one the earlier
 * chipguy_ESP32-32E_CYD_4inch_display used, so its calibration carries over.
 */

#pragma once

#include <Arduino.h>
#include <driver/spi_master.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "chipguy_CYD_resistive_display.h"

// ── Sampling between display writes ────────────────────────────────────────
// Where the XPT2046 shares the display's SPI bus (every shared-touch board,
// and the NM-CYD-C5), begin(display) installs a hook the display runs while
// its bus is idle -- in flushRegion() and between the text terminal's steps.
// While the screen is pressed, the hook samples at most every
// CYDR_TOUCH_SAMPLE_MS into a cache; read() / readRaw() use a cached sample up
// to CYDR_TOUCH_CACHE_MS old and sample for themselves only when it is older
// (nothing being drawn, so the bus is free).  So a read never waits on a
// pixel DMA while the screen is busy.  PENIRQ is checked directly on every
// read, so press and release are immediate; coordinates can be up to one
// frame old.  setSampleCache(false) restores sampling on every read.
#ifndef CYDR_TOUCH_SAMPLE_MS
#define CYDR_TOUCH_SAMPLE_MS 8
#endif
#ifndef CYDR_TOUCH_CACHE_MS
#define CYDR_TOUCH_CACHE_MS 50     // > LVGL's 33 ms refresh period
#endif

// NM-CYD-C5 touch axes against the panel's (portrait, rotation 0): raw X
// runs opposite the panel's columns, raw Y with its rows, no swap (bench,
// 2026-11-09: corner taps, two runs alike).
#ifndef CYDR_C5_TOUCH_MX
#define CYDR_C5_TOUCH_MX true
#endif
#ifndef CYDR_C5_TOUCH_MY
#define CYDR_C5_TOUCH_MY false
#endif
#ifndef CYDR_C5_TOUCH_SWAP
#define CYDR_C5_TOUCH_SWAP false
#endif

class chipguy_CYD_resistive_touch {
public:
    chipguy_CYD_resistive_touch();

    // Configure the touch controller for the board `display` found.  Call
    // after display.begin().  Returns false if the shared-bus device could not
    // be added (shared-touch boards only).
    bool begin(chipguy_CYD_resistive_display &display);

    // As above, without a display object: uses the board found by the most
    // recent display begin(), or, failing that, checks the board itself.  On
    // shared-touch boards the display must still have been begun first (it owns the
    // bus), and reads don't wait for display DMA — prefer begin(display).
    bool begin();

    // The board this driver was set up for, after begin().
    cydr_board_t board() const { return _board; }

    // True when the touch controller is capacitive (GT911 or CST820), after
    // begin().
    bool capacitive() const { return _chip != CYDR_TOUCH_XPT2046; }
    // The touch controller in use, after begin(), and its name.
    cydr_touch_chip_t chip() const { return _chip; }
    const char *chipName() const;

    // Before begin(): whether to apply the touch overrides saved in NVS
    // (default true; see cydr_overrides_t).  The touch bus override is the
    // display's: begin(display) follows display.touchBus().
    void useOverrides(bool on) { _useOverrides = on; }
    // After begin(): what detection chose for the touch fields of
    // cydr_overrides_t (1 off / 2 on); the other fields are left alone.
    void autoValues(cydr_overrides_t &o) const;
    // After begin(), change the axis handling live (not saved); for
    // previewing overrides.  Also what the overrides set at begin().
    void setAxes(bool mirrorX, bool mirrorY, bool swapXY);

    // Unrotated panel size the touch maps onto (240x320 or 320x480).
    int16_t nativeWidth()  const { return _nativeW; }
    int16_t nativeHeight() const { return _nativeH; }

    // Whether read() mirrors raw X / raw Y to match the panel (own-touch:
    // X; shared-touch 240x320 (3.2"): both; 320x480: Y).  TouchCalibrate uses these to fit the calibration.
    bool mirrorsX() const { return _mirrorX; }
    bool mirrorsY() const { return _mirrorY; }
    // True on the ST7789 ESP32-024, whose XPT2046 X and Y channels are
    // swapped relative to the panel (the ILI9341 one's aren't).  readRaw() has already swapped them back, so
    // calibration code needs no special case.
    bool swapsXY() const { return _swapXY; }

    // Match the touch coordinate mapping to the display rotation, using the
    // same convention as chipguy_CYD_resistive_display: an index 0..3 or
    // degrees 0/90/180/270.
    void setRotation(uint16_t rotation);

    // Override the raw-ADC calibration rectangle.  xMin/xMax map across the
    // panel's physical X axis (nativeWidth()), yMin/yMax across the physical
    // Y axis (nativeHeight()), both before rotation is applied.  Usually set
    // via loadCalibration() or produced by the TouchCalibrate example.
    void setCalibration(uint16_t xMin, uint16_t xMax, uint16_t yMin, uint16_t yMax);

    // Read the current calibration rectangle (e.g. to display or re-save it).
    void getCalibration(uint16_t &xMin, uint16_t &xMax,
                        uint16_t &yMin, uint16_t &yMax) const;

    // Load the calibration rectangle from NVS (Preferences).  Returns true and
    // applies it if a saved calibration exists; returns false and leaves the
    // current (default) calibration in place otherwise.  Call after begin(),
    // which picks the board's NVS namespace.
    bool loadCalibration();

    // Persist the current calibration rectangle to NVS (Preferences) so future
    // boots can loadCalibration() it.
    void saveCalibration();

    // Noise filters applied by read() (not readRaw()):
    //   minPressure - reject touches lighter than this (0 = off).  Compare
    //                 against pressure() values gathered on your panel.
    //   maxJump     - reject a reading that teleports more than this many pixels
    //                 from the previous accepted point (0 = off); brief outliers
    //                 are dropped, a sustained move is still let through.
    void setFilters(uint16_t minPressure, uint16_t maxJump);
    void getFilters(uint16_t &minPressure, uint16_t &maxJump) const;

    // Load/save the filter settings in NVS (Preferences), same store as the
    // calibration.  loadFilters() returns true if saved settings were found.
    bool loadFilters();
    void saveFilters();

    // Pressure of the most recent sample: a position-normalized touch-resistance
    // metric (XPT2046 Z and X), higher = firmer press, and roughly uniform
    // across the panel.  Watch it live (e.g. the readout in TouchCalibrate) to
    // choose a minPressure threshold.
    uint16_t pressure() const { return _lastPressure; }

    // Read the touch point.  Returns true if the screen is currently pressed,
    // with x,y mapped to screen pixels for the active rotation.
    bool read(uint16_t &x, uint16_t &y);

    // Read median-filtered raw 12-bit ADC values (no calibration/rotation
    // applied).  Returns true if pressed.  Useful for gathering calibration
    // numbers.
    bool readRaw(uint16_t &rawX, uint16_t &rawY);

    // Sampling between display writes (shared bus; see the top of this file).
    // On by default; off makes every read sample the bus itself, as before.
    void setSampleCache(bool on) { _cacheOn = on; }
    bool sampleCache() const { return _cacheOn; }
    // Cache only: read() never samples the shared bus itself, however old
    // the cache is; it reports whatever the display side's last sample was
    // (released if there is none since the press began).  For a sketch whose
    // own task owns the bus and calls display.runTouchSampler() whenever it
    // has a gap (RasterBouncingBalls), so a touch read can never delay a
    // transfer.  No effect on own-pins or capacitive boards.  Off by default.
    void setCacheOnly(bool on) { _cacheOnly = on; }
    bool cacheOnly() const { return _cacheOnly; }
    // Diagnostics: bus samples taken so far (by either side), and how many
    // reads were answered by a sample they hadn't seen yet.
    uint32_t busSamples() const;
    uint32_t freshReads() const { return _hits; }

private:
    chipguy_CYD_resistive_display *_display = nullptr;
    spi_device_handle_t _spi = nullptr;   // shared-bus device (shared-touch boards only)
    cydr_board_t _board = CYDR_BOARD_OWN_TOUCH;
    int16_t  _nativeW = CYDR_TFT_NATIVE_WIDTH;
    int16_t  _nativeH = CYDR_TFT_NATIVE_HEIGHT;
    bool     _mirrorX = true, _mirrorY = false;
    bool     _swapXY = false;
    const char *_nvsNs;
    bool     _useOverrides = true;
    bool     _autoMx = false, _autoMy = false, _autoSwap = false;
    cydr_touch_chip_t _chip = CYDR_TOUCH_XPT2046;
    uint8_t  _capAddr = 0;                // capacitive chip's I2C address
    bool     _gtDown = false;             // capacitive: last reported state
    uint16_t _gtX = 0, _gtY = 0;

    uint8_t  _rotation = 0;
    uint16_t _xMin, _xMax, _yMin, _yMax;

    // Noise-filter settings and state (see setFilters()).
    uint16_t _minPressure = 0;
    uint16_t _maxJump = 0;
    uint16_t _lastPressure = 0;
    uint16_t _lastX = 0, _lastY = 0;
    bool     _hasLast = false;
    uint8_t  _jumpCount = 0;

    bool     _setup(cydr_board_t board, cydr_panel_t panel);
    bool     _begin(cydr_board_t board, cydr_panel_t panel);
    uint16_t _readChannel(uint8_t cmd);
    uint16_t _transferByteAndRead12(uint8_t cmd);
    uint16_t _readChannelMedian(uint8_t cmd);
    bool     _readRawSample(uint16_t &rawX, uint16_t &rawY);
    bool     _sampleBus(uint16_t &rawX, uint16_t &rawY, uint16_t &pressure);
    bool     _sampleToCache();
    void     _installSampler();
    static void _samplerHook(void *arg);

    struct _cache_t {
        uint16_t x = 0, y = 0, p = 0;
        bool     down = false, valid = false;
        uint32_t ms = 0;
    };
    _cache_t _cache;
    portMUX_TYPE _cacheMux = portMUX_INITIALIZER_UNLOCKED;
    StaticSemaphore_t _sampleMutexBuf;
    SemaphoreHandle_t _sampleMutex = nullptr;   // one bus sample at a time
    bool     _cacheOn = true;
    bool     _cacheOnly = false;
    uint32_t _samples = 0, _hits = 0, _lastSeenMs = 0xFFFFFFFF;
    bool     _gt911Read(uint16_t &rawX, uint16_t &rawY);
    bool     _gtReg(uint16_t reg, uint8_t *buf, int n);       // takes cydr_touchI2cLock()
    bool     _gtRegLocked(uint16_t reg, uint8_t *buf, int n);
    bool     _cst820Read(uint16_t &rawX, uint16_t &rawY);
    bool     _cstReg(uint8_t reg, uint8_t *buf, int n);       // takes cydr_touchI2cLock()
    bool     _cstRegLocked(uint8_t reg, uint8_t *buf, int n);
};
