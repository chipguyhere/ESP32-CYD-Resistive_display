/*
 * XPT2046 resistive touch driver for the resistive-touch CYD family
 * (ESP32-2432S028; ESP32-32E 2.8", 3.2", 3.5", 4.0"; ESP32-024).
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
 *     bus, and a read waits for any in-flight display DMA first.
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
#include "chipguy_CYD_resistive_display.h"

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

    // Unrotated panel size the touch maps onto (240x320 or 320x480).
    int16_t nativeWidth()  const { return _nativeW; }
    int16_t nativeHeight() const { return _nativeH; }

    // Whether read() mirrors raw X / raw Y to match the panel (own-touch:
    // X; shared-touch 240x320 (3.2"): both; 320x480: Y).  TouchCalibrate uses these to fit the calibration.
    bool mirrorsX() const { return _mirrorX; }
    bool mirrorsY() const { return _mirrorY; }
    // True on the ESP32-024, whose XPT2046 X and Y channels are swapped
    // relative to the panel.  readRaw() has already swapped them back, so
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

private:
    chipguy_CYD_resistive_display *_display = nullptr;
    spi_device_handle_t _spi = nullptr;   // shared-bus device (shared-touch boards only)
    cydr_board_t _board = CYDR_BOARD_OWN_TOUCH;
    int16_t  _nativeW = CYDR_TFT_NATIVE_WIDTH;
    int16_t  _nativeH = CYDR_TFT_NATIVE_HEIGHT;
    bool     _mirrorX = true, _mirrorY = false;
    bool     _swapXY = false;
    const char *_nvsNs;

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
    uint16_t _readChannel(uint8_t cmd);
    uint16_t _transferByteAndRead12(uint8_t cmd);
    uint16_t _readChannelMedian(uint8_t cmd);
    bool     _readRawSample(uint16_t &rawX, uint16_t &rawY);
};
