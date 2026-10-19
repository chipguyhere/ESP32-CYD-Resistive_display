/*
 * XPT2046 resistive touch driver for the resistive-touch CYD family.
 * See chipguy_CYD_resistive_touch.h for wiring and usage.
 */

#include "chipguy_CYD_resistive_touch.h"

#include <Preferences.h>

// NVS namespaces + keys for the persisted calibration rectangle, one per
// wiring + panel size.  The strings predate the cydr_ names and are kept so
// saved calibrations survive; the 320x480 one is also the earlier
// chipguy_ESP32-32E_CYD_4inch_display's, so its saved calibration carries over.
#define CYDR_TOUCH_NVS_NS_OWN        "e32e28touch"   // own-touch, 240x320 (2432S028, 32E 2.8")
#define CYDR_TOUCH_NVS_NS_SHARED_480 "e32etouch"     // shared-touch, 320x480 (3.5", 4.0")
#define CYDR_TOUCH_NVS_NS_SHARED_320 "e32e32touch"   // shared-touch, 240x320 (3.2")
#define CYDR_TOUCH_NVS_NS_024        "e32024touch"   // shared-touch, 240x320, X/Y swapped (ESP32-024)
#define CYDR_TOUCH_NVS_XMIN "xmin"
#define CYDR_TOUCH_NVS_XMAX "xmax"
#define CYDR_TOUCH_NVS_YMIN "ymin"
#define CYDR_TOUCH_NVS_YMAX "ymax"
#define CYDR_TOUCH_NVS_MINP "minp"   // minimum pressure filter
#define CYDR_TOUCH_NVS_JUMP "jump"   // max jump (px) filter

// ── Board pin assignments ──────────────────────────────────────────────────
// Own-touch boards: the XPT2046 has its own pins, bit-banged.
#define CYDR_TOUCH_CLK   25
#define CYDR_TOUCH_MOSI  32
#define CYDR_TOUCH_MISO  39
// Both wirings:
#define CYDR_TOUCH_CS    33
#define CYDR_TOUCH_IRQ   36   // active LOW while the panel is being touched

// Shared-touch boards: the XPT2046 is a second device on the display's bus.  Its max
// SPI clock is ~2.5 MHz; stay comfortably under it.  The SPI master driver
// re-clocks the bus to this rate for touch and back to 40 MHz for the display.
#define CYDR_TOUCH_SPI_HZ  2000000

// XPT2046 control bytes: start bit + channel select, 12-bit, differential.
#define XPT2046_CMD_X   0xD0   // read X position
#define XPT2046_CMD_Y   0x90   // read Y position
#define XPT2046_CMD_Z1  0xB0   // read Z1 (pressure)
#define XPT2046_CMD_Z2  0xC0   // read Z2 (pressure)

// Scale for the position-normalized pressure metric (see _readRawSample).  It
// just sets the numeric range of pressure() / the minPressure slider; raise it
// if firm presses read too low, lower it if they saturate at 4095.
#define CYDR_TOUCH_P_SCALE  100.0f

// Default raw-ADC calibration rectangle (typical unit; tune with readRaw()).
#define CYDR_TOUCH_X_MIN  200
#define CYDR_TOUCH_X_MAX  3900
#define CYDR_TOUCH_Y_MIN  200
#define CYDR_TOUCH_Y_MAX  3900

// Samples taken per axis per read.  We return the MEDIAN of these (not the
// mean), which discards the occasional spike instead of blending it in.  Odd
// so the median is a single middle element.
static const int CYDR_TOUCH_SAMPLES = 7;

chipguy_CYD_resistive_touch::chipguy_CYD_resistive_touch()
    : _nvsNs(CYDR_TOUCH_NVS_NS_OWN),
      _xMin(CYDR_TOUCH_X_MIN), _xMax(CYDR_TOUCH_X_MAX),
      _yMin(CYDR_TOUCH_Y_MIN), _yMax(CYDR_TOUCH_Y_MAX) {}

bool chipguy_CYD_resistive_touch::begin(chipguy_CYD_resistive_display &display) {
    _display = &display;
    return _setup(display.board(), display.panel());
}

bool chipguy_CYD_resistive_touch::begin() {
    cydr_board_t b = cydr_activeBoard();
    if (b == CYDR_BOARD_UNKNOWN) b = cydr_detectBoardPassive();
    return _setup(b, cydr_activePanel());
}

bool chipguy_CYD_resistive_touch::_setup(cydr_board_t board, cydr_panel_t panel) {
    // An unknown board is treated as own-pins wiring, so nothing shared gets
    // driven.
    _board = board == CYDR_BOARD_SHARED_TOUCH ? CYDR_BOARD_SHARED_TOUCH
                                                : CYDR_BOARD_OWN_TOUCH;
    // No display begun: assume the panel the wiring usually comes with.
    if (panel == CYDR_PANEL_AUTO)
        panel = _board == CYDR_BOARD_SHARED_TOUCH ? CYDR_PANEL_ST7796
                                                    : CYDR_PANEL_ILI9341;

    // IRQ is GPIO36, an input-only pin with NO internal pull resistor; the
    // XPT2046's PENIRQ drives it high when idle, low when touched.
    pinMode(CYDR_TOUCH_IRQ, INPUT);
    _swapXY = false;

    if (_board == CYDR_BOARD_SHARED_TOUCH) {
        if (panel == CYDR_PANEL_ST7796) {
            _nativeW = 320; _nativeH = 480;
            // On this panel raw Y runs opposite the ST7796's rows; raw X matches.
            // (Measured on hardware for chipguy_ESP32-32E_CYD_4inch_display.)
            _mirrorX = false; _mirrorY = true;
            _nvsNs = CYDR_TOUCH_NVS_NS_SHARED_480;
        } else if (cydr_activeModel() == CYDR_MODEL_024) {
            // ESP32-024: the XPT2046's X and Y run along the panel's rows and
            // columns, so the raw pair is swapped in _readRawSample().  After
            // the swap raw X runs opposite the panel's X and raw Y matches,
            // as on the own-touch boards (bench, 2026-10-18).
            _nativeW = 240; _nativeH = 320;
            _swapXY = true;
            _mirrorX = true; _mirrorY = false;
            _nvsNs = CYDR_TOUCH_NVS_NS_024;
        } else {
            // 3.2": 240x320 panel on the shared wiring.  Both raw axes run
            // opposite the panel's (bench, 2026-10-13).  Also the guess when
            // the model is unknown (touch begun without a display, or no
            // GPIO4 reading).
            _nativeW = 240; _nativeH = 320;
            _mirrorX = true; _mirrorY = true;
            _nvsNs = CYDR_TOUCH_NVS_NS_SHARED_320;
        }

        if (_spi) return true;
        // Add the XPT2046 to the display's already-initialized SPI bus as a
        // second device.  Hardware CS on GPIO33, slow clock, SPI mode 0.
        spi_device_interface_config_t devcfg = {};
        devcfg.clock_speed_hz = CYDR_TOUCH_SPI_HZ;
        devcfg.mode = 0;
        devcfg.spics_io_num = CYDR_TOUCH_CS;
        devcfg.queue_size = 1;
        esp_err_t err = spi_bus_add_device(CYDR_TFT_SPI_HOST, &devcfg, &_spi);
        if (err != ESP_OK) {
            Serial.printf("cydr touch: spi_bus_add_device failed: 0x%x "
                          "(begin the display first)\n", err);
            _spi = nullptr;
            return false;
        }
        return true;
    }

    _nativeW = 240; _nativeH = 320;
    // The XPT2046's raw X increases opposite the panel's X axis (same on the
    // ILI9341 and ST7789 boards); raw Y matches.
    _mirrorX = true; _mirrorY = false;
    _nvsNs = CYDR_TOUCH_NVS_NS_OWN;

    pinMode(CYDR_TOUCH_CLK, OUTPUT);
    pinMode(CYDR_TOUCH_MOSI, OUTPUT);
    pinMode(CYDR_TOUCH_MISO, INPUT);
    pinMode(CYDR_TOUCH_CS, OUTPUT);
    digitalWrite(CYDR_TOUCH_CS, HIGH);   // deselect
    digitalWrite(CYDR_TOUCH_CLK, LOW);
    return true;
}

void chipguy_CYD_resistive_touch::setRotation(uint16_t rotation) {
    _rotation = cydr_normalize_rotation(rotation);
}

void chipguy_CYD_resistive_touch::setCalibration(uint16_t xMin, uint16_t xMax,
                                               uint16_t yMin, uint16_t yMax) {
    _xMin = xMin; _xMax = xMax;
    _yMin = yMin; _yMax = yMax;
}

void chipguy_CYD_resistive_touch::getCalibration(uint16_t &xMin, uint16_t &xMax,
                                               uint16_t &yMin, uint16_t &yMax) const {
    xMin = _xMin; xMax = _xMax;
    yMin = _yMin; yMax = _yMax;
}

bool chipguy_CYD_resistive_touch::loadCalibration() {
    Preferences prefs;
    if (!prefs.begin(_nvsNs, /*readOnly=*/true)) return false;
    // Treat the calibration as present only if all four keys exist.
    bool ok = prefs.isKey(CYDR_TOUCH_NVS_XMIN) && prefs.isKey(CYDR_TOUCH_NVS_XMAX) &&
              prefs.isKey(CYDR_TOUCH_NVS_YMIN) && prefs.isKey(CYDR_TOUCH_NVS_YMAX);
    if (ok) {
        _xMin = prefs.getUShort(CYDR_TOUCH_NVS_XMIN, _xMin);
        _xMax = prefs.getUShort(CYDR_TOUCH_NVS_XMAX, _xMax);
        _yMin = prefs.getUShort(CYDR_TOUCH_NVS_YMIN, _yMin);
        _yMax = prefs.getUShort(CYDR_TOUCH_NVS_YMAX, _yMax);
    }
    prefs.end();
    return ok;
}

void chipguy_CYD_resistive_touch::saveCalibration() {
    Preferences prefs;
    if (!prefs.begin(_nvsNs, /*readOnly=*/false)) return;
    prefs.putUShort(CYDR_TOUCH_NVS_XMIN, _xMin);
    prefs.putUShort(CYDR_TOUCH_NVS_XMAX, _xMax);
    prefs.putUShort(CYDR_TOUCH_NVS_YMIN, _yMin);
    prefs.putUShort(CYDR_TOUCH_NVS_YMAX, _yMax);
    prefs.end();
}

void chipguy_CYD_resistive_touch::setFilters(uint16_t minPressure, uint16_t maxJump) {
    _minPressure = minPressure;
    _maxJump = maxJump;
}

void chipguy_CYD_resistive_touch::getFilters(uint16_t &minPressure, uint16_t &maxJump) const {
    minPressure = _minPressure;
    maxJump = _maxJump;
}

bool chipguy_CYD_resistive_touch::loadFilters() {
    Preferences prefs;
    if (!prefs.begin(_nvsNs, /*readOnly=*/true)) return false;
    bool ok = prefs.isKey(CYDR_TOUCH_NVS_MINP) && prefs.isKey(CYDR_TOUCH_NVS_JUMP);
    if (ok) {
        _minPressure = prefs.getUShort(CYDR_TOUCH_NVS_MINP, _minPressure);
        _maxJump     = prefs.getUShort(CYDR_TOUCH_NVS_JUMP, _maxJump);
    }
    prefs.end();
    return ok;
}

void chipguy_CYD_resistive_touch::saveFilters() {
    Preferences prefs;
    if (!prefs.begin(_nvsNs, /*readOnly=*/false)) return;
    prefs.putUShort(CYDR_TOUCH_NVS_MINP, _minPressure);
    prefs.putUShort(CYDR_TOUCH_NVS_JUMP, _maxJump);
    prefs.end();
}

// Bit-bang one XPT2046 conversion (own-touch boards): send the 8-bit command, then clock
// out the 12-bit result (MSB first).  SPI mode 0, sampled on the rising edge.
// CS is handled by the caller.
uint16_t chipguy_CYD_resistive_touch::_transferByteAndRead12(uint8_t cmd) {
    // Send command byte.
    for (int i = 7; i >= 0; i--) {
        digitalWrite(CYDR_TOUCH_MOSI, (cmd >> i) & 1);
        digitalWrite(CYDR_TOUCH_CLK, HIGH);
        digitalWrite(CYDR_TOUCH_CLK, LOW);
    }

    // One busy clock between command and result.
    digitalWrite(CYDR_TOUCH_CLK, HIGH);
    digitalWrite(CYDR_TOUCH_CLK, LOW);

    // Clock in 12 result bits, MSB first.
    uint16_t value = 0;
    for (int i = 0; i < 12; i++) {
        digitalWrite(CYDR_TOUCH_CLK, HIGH);
        value = (value << 1) | (digitalRead(CYDR_TOUCH_MISO) & 1);
        digitalWrite(CYDR_TOUCH_CLK, LOW);
    }
    return value;
}

// One conversion on whichever bus this board uses.  On the shared bus a
// 3-byte transaction (command + 2 read bytes) covers the command, the one busy
// clock, and the 12 result bits MSB-first; the result lands in bits 14..3 of
// the last two bytes, and the SPI driver drives CS.
uint16_t chipguy_CYD_resistive_touch::_readChannel(uint8_t cmd) {
    if (!_spi) return _transferByteAndRead12(cmd);
    spi_transaction_t t = {};
    t.flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA;
    t.length = 24;               // 3 bytes on the wire
    t.tx_data[0] = cmd;
    if (spi_device_polling_transmit(_spi, &t) != ESP_OK) return 0;
    return (uint16_t)(((t.rx_data[1] << 8) | t.rx_data[2]) >> 3) & 0x0FFF;
}

// Read one channel CYDR_TOUCH_SAMPLES times and return the MEDIAN.  The first
// conversion right after switching channels is the least settled, so we take a
// throwaway read before sampling.  The median rejects outliers (the spikes that
// make a plain average jump around) far better than a mean.  On own-touch boards,
// call with CS already asserted (LOW) — _readRawSample holds it for the whole block.
uint16_t chipguy_CYD_resistive_touch::_readChannelMedian(uint8_t cmd) {
    uint16_t buf[CYDR_TOUCH_SAMPLES];
    _readChannel(cmd);                                        // throwaway: let it settle
    for (int i = 0; i < CYDR_TOUCH_SAMPLES; i++) buf[i] = _readChannel(cmd);

    // Insertion sort (tiny fixed array), then take the middle element.
    for (int i = 1; i < CYDR_TOUCH_SAMPLES; i++) {
        uint16_t v = buf[i];
        int j = i - 1;
        while (j >= 0 && buf[j] > v) { buf[j + 1] = buf[j]; j--; }
        buf[j + 1] = v;
    }
    return buf[CYDR_TOUCH_SAMPLES / 2];
}

bool chipguy_CYD_resistive_touch::_readRawSample(uint16_t &rawX, uint16_t &rawY) {
    // IRQ is pulled low by the controller only while the panel is pressed.
    if (digitalRead(CYDR_TOUCH_IRQ) != LOW) return false;

    // On the shared bus, let any in-flight display DMA finish first.
    if (_spi && _display) _display->waitFlush();

    if (!_spi) digitalWrite(CYDR_TOUCH_CS, LOW);
    // Median of several conversions per axis rejects the occasional spike far
    // better than a mean (which blends it in) — resistive jitter is mostly
    // impulse noise.  See _readChannelMedian.
    rawX = _readChannelMedian(XPT2046_CMD_X);
    rawY = _readChannelMedian(XPT2046_CMD_Y);

    // Pressure via the XPT2046 datasheet touch-resistance formula:
    //   R_touch ∝ (xpos / 4096) * (z2/z1 - 1)
    // The (xpos/4096) term divides out the touch position, so the result is
    // roughly uniform across the panel — unlike the raw z1+4096-z2 proxy, which
    // reads much lighter near the low-rawX edge.  We report its inverse
    // (conductance), scaled, so higher = firmer press (matching minPressure).
    uint16_t z1 = _readChannel(XPT2046_CMD_Z1);
    uint16_t z2 = _readChannel(XPT2046_CMD_Z2);
    if (!_spi) digitalWrite(CYDR_TOUCH_CS, HIGH);

    uint16_t xpos = rawX;                  // median X position for the formula
    if (_swapXY) { rawX = rawY; rawY = xpos; }   // ESP32-024: report in panel orientation
    float p = 0.0f;
    if (z1 > 0 && xpos > 0) {
        if (z2 <= z1) {
            p = 4095.0f;                       // resistance ~0 (or noise): treat as firmest
        } else {
            float r = ((float)xpos / 4096.0f) * ((float)z2 / (float)z1 - 1.0f);
            p = (r > 0.0f) ? CYDR_TOUCH_P_SCALE / r : 4095.0f;
        }
    }
    _lastPressure = (uint16_t)(p > 4095.0f ? 4095.0f : p);

    // Still pressed after sampling?  Rejects spurious single-edge IRQs.
    if (digitalRead(CYDR_TOUCH_IRQ) != LOW) return false;

    return true;
}

bool chipguy_CYD_resistive_touch::readRaw(uint16_t &rawX, uint16_t &rawY) {
    return _readRawSample(rawX, rawY);
}

bool chipguy_CYD_resistive_touch::read(uint16_t &x, uint16_t &y) {
    uint16_t rawX, rawY;
    if (!_readRawSample(rawX, rawY)) {
        _hasLast = false;       // released: forget the last point for jump filter
        _jumpCount = 0;
        return false;
    }

    // Pressure filter: reject touches lighter than the configured threshold.
    if (_minPressure > 0 && _lastPressure < _minPressure) return false;

    const long W = _nativeW - 1;    // 239 or 319
    const long H = _nativeH - 1;    // 319 or 479

    // Clamp to the calibration rectangle, then map to physical pixels
    // (before rotation).
    long px = map((long)constrain(rawX, _xMin, _xMax), _xMin, _xMax, 0, W);
    long py = map((long)constrain(rawY, _yMin, _yMax), _yMin, _yMax, 0, H);

    // Mirror whichever raw axis runs opposite the panel on this board.  (Done
    // in physical space so every rotation inherits the correction.)
    if (_mirrorX) px = W - px;
    if (_mirrorY) py = H - py;

    switch (_rotation) {
        case 0:  x = px;          y = py;          break;  // portrait
        case 1:  x = py;          y = W - px;      break;  // landscape
        case 2:  x = W - px;      y = H - py;      break;  // portrait flipped
        case 3:  x = H - py;      y = px;          break;  // landscape flipped
    }

    // Jump filter: drop a reading that teleports more than _maxJump pixels from
    // the last accepted point.  A real fast drag survives because we let the
    // reading through after a couple of consecutive "jumps".
    if (_maxJump > 0 && _hasLast) {
        long d = labs((long)x - _lastX) + labs((long)y - _lastY);
        if (d > _maxJump && ++_jumpCount < 3) return false;
    }
    _jumpCount = 0;
    _lastX = x; _lastY = y; _hasLast = true;
    return true;
}
