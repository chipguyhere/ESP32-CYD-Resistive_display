/*
 * XPT2046 resistive touch driver for the resistive-touch CYD family.
 * See chipguy_CYD_resistive_touch.h for wiring and usage.
 */

#include "chipguy_CYD_resistive_touch.h"

#include <Preferences.h>
#include <Wire.h>

// NVS namespaces + keys for the persisted calibration rectangle, one per
// wiring + panel size.  The strings predate the cydr_ names and are kept so
// saved calibrations survive; the 320x480 one is also the earlier
// chipguy_ESP32-32E_CYD_4inch_display's, so its saved calibration carries over.
#define CYDR_TOUCH_NVS_NS_OWN        "e32e28touch"   // own-touch, 240x320 (2432S028, 32E 2.8")
#define CYDR_TOUCH_NVS_NS_SHARED_480 "e32etouch"     // shared-touch, 320x480 (3.5", 4.0")
#define CYDR_TOUCH_NVS_NS_SHARED_320 "e32e32touch"   // shared-touch, 240x320 (3.2")
#define CYDR_TOUCH_NVS_NS_024        "e32024touch"   // shared-touch, 240x320, X/Y swapped (ESP32-024, ST7789)
#define CYDR_TOUCH_NVS_NS_024_ILI    "e32024itouch"  // shared-touch, 240x320, both axes mirrored (ESP32-024, ILI9341)
#define CYDR_TOUCH_NVS_NS_035        "e32035touch"   // shared-touch, 320x480, both axes mirrored (ESP32-035)
#define CYDR_TOUCH_NVS_NS_S032       "e2432s032touch" // shared-touch, 240x320 (ESP32-2432S032)
#define CYDR_TOUCH_NVS_NS_S032C      "e2432s032ctch"  // GT911; holds only filters (ESP32-2432S032C)
#define CYDR_TOUCH_NVS_NS_CAP        "ecaptouch"      // other capacitive boards; filters only
#define CYDR_TOUCH_NVS_NS_C5         "nmcydc5touch"   // NM-CYD-C5 (ESP32-C5)
#define CYDR_TOUCH_NVS_NS_ES3C28P    "es3c28ptouch"   // FT6336; holds only filters (ES3C28P)
#define CYDR_TOUCH_NVS_XMIN "xmin"
#define CYDR_TOUCH_NVS_XMAX "xmax"
#define CYDR_TOUCH_NVS_YMIN "ymin"
#define CYDR_TOUCH_NVS_YMAX "ymax"
#define CYDR_TOUCH_NVS_MINP "minp"   // minimum pressure filter
#define CYDR_TOUCH_NVS_JUMP "jump"   // max jump (px) filter

// ── Board pin assignments ──────────────────────────────────────────────────
#if CYDR_CHIP_C5
// NM-CYD-C5: on the display's bus only.
#define CYDR_TOUCH_CS     1
#define CYDR_TOUCH_IRQ    3   // PENIRQ, 10k pull-up on the board; LOW while touched
#elif CYDR_CHIP_S3
// ES3C28P: no XPT2046.  The FT6336 is on Wire (I2C0), SDA 16 / SCL 15,
// which the ES8311 codec shares -- a sketch's own Wire.begin(16, 15) for the
// codec then finds the bus already started.  INT (17) isn't used: polled.
#define CYDR_TOUCH_IRQ   -1
#define CYDR_TOUCH_CS    -1   // unused: the XPT2046 paths never run here
#define CYDR_CAP_SDA     16
#define CYDR_CAP_SCL     15
#define CYDR_CAP_WIRE    Wire
#else
// Own-touch boards: the XPT2046 has its own pins, bit-banged.
#define CYDR_TOUCH_CLK   25
#define CYDR_TOUCH_MOSI  32
#define CYDR_TOUCH_MISO  39
// Both wirings:
#define CYDR_TOUCH_CS    33
#define CYDR_TOUCH_IRQ   36   // active LOW while the panel is being touched
// Capacitive chips (GT911, CST820): Wire1 on SDA 33 / SCL 32.
#define CYDR_CAP_SDA     33
#define CYDR_CAP_SCL     32
#define CYDR_CAP_WIRE    Wire1
#endif
#ifndef CYDR_CAP_WIRE                 // the C5: no capacitive board, but keep it compiling
#define CYDR_CAP_SDA     -1
#define CYDR_CAP_SCL     -1
#define CYDR_CAP_WIRE    Wire1
#endif

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

// The screen-pressed test: PENIRQ, LOW while touched.  A GPIO read; costs no
// bus time.
static inline bool _cydr_irqDown() { return CYDR_TOUCH_IRQ >= 0 && digitalRead(CYDR_TOUCH_IRQ) == LOW; }

chipguy_CYD_resistive_touch::chipguy_CYD_resistive_touch()
    : _nvsNs(CYDR_TOUCH_NVS_NS_OWN),
      _xMin(CYDR_TOUCH_X_MIN), _xMax(CYDR_TOUCH_X_MAX),
      _yMin(CYDR_TOUCH_Y_MIN), _yMax(CYDR_TOUCH_Y_MAX) {}

bool chipguy_CYD_resistive_touch::begin(chipguy_CYD_resistive_display &display) {
    _display = &display;
    return _begin(display.touchBus(), display.panel());
}

bool chipguy_CYD_resistive_touch::begin() {
    cydr_board_t b = cydr_activeTouchBus();
    if (b == CYDR_BOARD_UNKNOWN) b = cydr_detectBoardPassive();
    return _begin(b, cydr_activePanel());
}

// Set up for the board, note what detection chose, then apply any saved
// axis overrides on top.
bool chipguy_CYD_resistive_touch::_begin(cydr_board_t board, cydr_panel_t panel) {
    bool ok = _setup(board, panel);
    _autoMx = _mirrorX; _autoMy = _mirrorY; _autoSwap = _swapXY;
    if (ok && _useOverrides) {
        cydr_overrides_t o;
        cydr_loadOverrides(o);
        if (o.touchMirrorX) _mirrorX = o.touchMirrorX == 2;
        if (o.touchMirrorY) _mirrorY = o.touchMirrorY == 2;
        if (o.touchSwapXY)  _swapXY  = o.touchSwapXY == 2;
    }
    return ok;
}

void chipguy_CYD_resistive_touch::autoValues(cydr_overrides_t &o) const {
    o.touchMirrorX = _autoMx ? 2 : 1;
    o.touchMirrorY = _autoMy ? 2 : 1;
    o.touchSwapXY  = _autoSwap ? 2 : 1;
}

void chipguy_CYD_resistive_touch::setAxes(bool mirrorX, bool mirrorY, bool swapXY) {
    _mirrorX = mirrorX; _mirrorY = mirrorY; _swapXY = swapXY;
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
#if CYDR_CHIP_S3
    if (cydr_activeTouchChip() == CYDR_TOUCH_NONE ||
        cydr_activeTouchChip() == CYDR_TOUCH_XPT2046) {
        // No touch fitted (ES3N28P, or an S3 board whose chip didn't answer):
        // nothing to set up; read() reports no press.
        _chip = CYDR_TOUCH_NONE;
        _nativeW = panel == CYDR_PANEL_ST7796 ? 320 : 240;
        _nativeH = panel == CYDR_PANEL_ST7796 ? 480 : 320;
        _mirrorX = false; _mirrorY = false;
        _nvsNs = CYDR_TOUCH_NVS_NS_ES3C28P;
        if (cydr_activeModel() != CYDR_MODEL_ES3N28P)
            Serial.printf("cydr touch: no touch chip answered on SDA %d / SCL %d\n",
                          CYDR_CAP_SDA, CYDR_CAP_SCL);
        return true;
    }
#else
    pinMode(CYDR_TOUCH_IRQ, INPUT);
#endif
#if CYDR_CHIP_C5
    pinMode(CYDR_TOUCH_CS, OUTPUT);       // deselected until the SPI driver takes it
    digitalWrite(CYDR_TOUCH_CS, HIGH);
#endif
    _swapXY = false;

    if (cydr_activeTouchChip() != CYDR_TOUCH_XPT2046) {
        // GT911 or CST820 on Wire1, reporting the panel's portrait pixels
        // (the GT911's configured resolution is 240x320 on the 2432S032C;
        // the CST820 reports 0..239 x 0..319).  No mirroring: both run with
        // the panel's rows and columns (bench: GT911 2026-10-29, CST820 on
        // the 2432S024C and JC2432W328 2026-10-31).
        _nativeW = panel == CYDR_PANEL_ST7796 ? 320 : 240;
        _nativeH = panel == CYDR_PANEL_ST7796 ? 480 : 320;
        _mirrorX = false; _mirrorY = false;
        _nvsNs = cydr_activeModel() == CYDR_MODEL_2432S032C ? CYDR_TOUCH_NVS_NS_S032C :
                 cydr_activeModel() == CYDR_MODEL_ES3C28P   ? CYDR_TOUCH_NVS_NS_ES3C28P
                                                            : CYDR_TOUCH_NVS_NS_CAP;
        if (_chip != CYDR_TOUCH_XPT2046) return true;     // begun before
        cydr_touch_chip_t want = cydr_activeTouchChip();
        uint8_t addr = cydr_activeTouchAddress();
        if (!addr) {
            // A forced model names a chip the display's probe didn't find
            // at once: look again.
            if (cydr_probeI2cTouch(&addr) != want) addr = 0;
        }
        if (!addr) {
            Serial.printf("cydr touch: %s not answering on SDA %d / SCL %d\n",
                          want == CYDR_TOUCH_GT911 ? "GT911" :
                          want == CYDR_TOUCH_FT6336 ? "FT6336" : "CST820",
                          CYDR_CAP_SDA, CYDR_CAP_SCL);
            return false;
        }
        _chip = want;
        _capAddr = addr;
#if CYDR_CHIP_C5
        return false;                     // unreachable: no capacitive C5 board
#endif
        CYDR_CAP_WIRE.begin(CYDR_CAP_SDA, CYDR_CAP_SCL, 400000);
        if (_chip == CYDR_TOUCH_CST820) {
            // Keep it awake: the CST816 family sleeps after a few seconds
            // without a touch and then stops answering on I2C until touched.
            // 0xFE (DisAutoSleep) = 1.
            cydr_touchI2cLock();
            CYDR_CAP_WIRE.beginTransmission(_capAddr);
            CYDR_CAP_WIRE.write(0xFE); CYDR_CAP_WIRE.write(0x01);
            CYDR_CAP_WIRE.endTransmission();
            cydr_touchI2cUnlock();
        } else if (_chip == CYDR_TOUCH_FT6336) {
            // Keep it in active mode: by default it drops to its slower-
            // scanning monitor mode a few seconds after the last touch.
            // 0x86 (G_CTRL) = 0.
            cydr_touchI2cLock();
            CYDR_CAP_WIRE.beginTransmission(_capAddr);
            CYDR_CAP_WIRE.write(0x86); CYDR_CAP_WIRE.write(0x00);
            CYDR_CAP_WIRE.endTransmission();
            cydr_touchI2cUnlock();
        }
        return true;
    }

    if (_board == CYDR_BOARD_SHARED_TOUCH) {
        if (CYDR_CHIP_C5) {
            // NM-CYD-C5: see CYDR_C5_TOUCH_* in the header.
            _nativeW = panel == CYDR_PANEL_ST7796 ? 320 : 240;
            _nativeH = panel == CYDR_PANEL_ST7796 ? 480 : 320;
            _mirrorX = CYDR_C5_TOUCH_MX; _mirrorY = CYDR_C5_TOUCH_MY;
            _swapXY = CYDR_C5_TOUCH_SWAP;
            _nvsNs = CYDR_TOUCH_NVS_NS_C5;
        } else if (panel == CYDR_PANEL_ST7796 && cydr_activeModel() == CYDR_MODEL_035) {
            // ESP32-035: raw X runs opposite the panel's columns too, unlike
            // the 32E 3.5"/4.0" (bench, 2026-10-23).
            _nativeW = 320; _nativeH = 480;
            _mirrorX = true; _mirrorY = true;
            _nvsNs = CYDR_TOUCH_NVS_NS_035;
        } else if (panel == CYDR_PANEL_ST7796) {
            _nativeW = 320; _nativeH = 480;
            // On this panel raw Y runs opposite the ST7796's rows; raw X matches.
            // (Measured on hardware for chipguy_ESP32-32E_CYD_4inch_display.)
            _mirrorX = false; _mirrorY = true;
            _nvsNs = CYDR_TOUCH_NVS_NS_SHARED_480;
        } else if (cydr_activeModel() == CYDR_MODEL_024 && panel == CYDR_PANEL_ILI9341) {
            // ESP32-024 with the ILI9341 panel: its touch film is laid the
            // other way from the ST7789 module's -- no swap, and both raw axes
            // run opposite the panel's, as on the 3.2" (bench, 2026-10-23).
            // Its own namespace, so a calibration saved under the swapped
            // layout is never applied to this one.
            _nativeW = 240; _nativeH = 320;
            _mirrorX = true; _mirrorY = true;
            _nvsNs = CYDR_TOUCH_NVS_NS_024_ILI;
        } else if (cydr_activeModel() == CYDR_MODEL_024) {
            // ESP32-024 (ST7789 panel): the XPT2046's X and Y run along the panel's rows and
            // columns, so the raw pair is swapped in _readRawSample().  After
            // the swap raw X runs opposite the panel's X and raw Y matches,
            // as on the own-touch boards (bench, 2026-10-18).
            _nativeW = 240; _nativeH = 320;
            _swapXY = true;
            _mirrorX = true; _mirrorY = false;
            _nvsNs = CYDR_TOUCH_NVS_NS_024;
        } else if (cydr_activeModel() == CYDR_MODEL_2432S032) {
            // ESP32-2432S032: both raw axes run opposite the panel's, as on
            // the 3.2" (bench, 2026-10-29).
            _nativeW = 240; _nativeH = 320;
            _mirrorX = true; _mirrorY = true;
            _nvsNs = CYDR_TOUCH_NVS_NS_S032;
        } else {
            // 3.2": 240x320 panel on the shared wiring.  Both raw axes run
            // opposite the panel's (bench, 2026-10-13).  Also the guess when
            // the model is unknown (touch begun without a display, or no
            // GPIO4 reading).
            _nativeW = 240; _nativeH = 320;
            _mirrorX = true; _mirrorY = true;
            _nvsNs = CYDR_TOUCH_NVS_NS_SHARED_320;
        }

        if (_spi) { _installSampler(); return true; }
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
        _installSampler();
        // Arm PENIRQ: the XPT2046 drives it only while its last command's
        // power-down bits were 00, and nothing is sent to it until PENIRQ
        // shows a press -- so a chip that came up (or was left) with them
        // set would never report one.  One conversion command with PD = 00
        // puts it in the state every later read leaves it in.
        if (_display) _display->waitFlush();
        _readChannel(XPT2046_CMD_Z1);
        return true;
    }

#if !CYDR_CHIP_CLASSIC
    return false;                         // no own-pins wiring on the C5 or S3
#else
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
#endif
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
    if (capacitive()) return false;         // the chip reports pixels
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
#if !CYDR_CHIP_CLASSIC
    return 0;                             // own-pins wiring only; none on the C5
#else
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
#endif
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

// ── Sampling between display writes (shared bus) ───────────────────────────
// On the shared-bus boards a read used to wait for the display's in-flight
// pixel DMA before it could use the bus -- up to a whole LVGL region (~4 ms
// at 40 MHz), stalling the render loop exactly while a finger drags.  Instead
// the display runs _samplerHook() at the moments its bus is idle anyway (in
// flushRegion(), and between the text terminal's steps); the sample lands in
// a small cache, and read() takes it from there without touching the bus.
// Only when the cache is old (nothing is being drawn, so the bus is free)
// does read() sample for itself.
//
// Timing: the writer samples only while PENIRQ shows a press and its last
// sample is CYDR_TOUCH_SAMPLE_MS old; a read accepts a cached sample up to
// CYDR_TOUCH_CACHE_MS old.  PENIRQ is read directly every time, so press and
// release are seen at once; only the coordinates can be up to one frame old.

void chipguy_CYD_resistive_touch::_installSampler() {
    if (!_display || !_spi) return;
    if (!_sampleMutex) _sampleMutex = xSemaphoreCreateMutexStatic(&_sampleMutexBuf);
    _display->setTouchSampler(_samplerHook, this);
}

void chipguy_CYD_resistive_touch::_samplerHook(void *arg) {
    auto *self = (chipguy_CYD_resistive_touch *)arg;
    if (!self->_cacheOn || !_cydr_irqDown()) return;
    uint32_t now = millis();
    portENTER_CRITICAL(&self->_cacheMux);
    uint32_t age = now - self->_cache.ms;
    bool valid = self->_cache.valid;
    portEXIT_CRITICAL(&self->_cacheMux);
    if (valid && age < CYDR_TOUCH_SAMPLE_MS) return;
    // Never block the writer: if a reader is sampling right now, skip.
    if (xSemaphoreTake(self->_sampleMutex, 0) != pdTRUE) return;
    self->_sampleToCache();
    xSemaphoreGive(self->_sampleMutex);
}

// One sample over the bus into the cache.  Caller holds _sampleMutex and
// knows the bus is idle.
bool chipguy_CYD_resistive_touch::_sampleToCache() {
    uint16_t x, y, p;
    bool down = _sampleBus(x, y, p);
    portENTER_CRITICAL(&_cacheMux);
    _cache.x = x; _cache.y = y; _cache.p = p;
    _cache.down = down;
    _cache.valid = true;
    _cache.ms = millis();
    portEXIT_CRITICAL(&_cacheMux);
    _samples++;
    return down;
}

uint32_t chipguy_CYD_resistive_touch::busSamples() const { return _samples; }

bool chipguy_CYD_resistive_touch::_readRawSample(uint16_t &rawX, uint16_t &rawY) {
    // IRQ is pulled low by the controller only while the panel is pressed.
    if (!_cydr_irqDown()) {
        // Released: a cached sample belongs to that press, not the next one.
        portENTER_CRITICAL(&_cacheMux);
        _cache.valid = false;
        portEXIT_CRITICAL(&_cacheMux);
        return false;
    }

    if (_spi && _cacheOn && _sampleMutex) {
        uint32_t now = millis();
        portENTER_CRITICAL(&_cacheMux);
        auto c = _cache;
        portEXIT_CRITICAL(&_cacheMux);
        if (!_cacheOnly && (!c.valid || now - c.ms > CYDR_TOUCH_CACHE_MS)) {
            // Nothing recent from the writer: nothing is being drawn, so the
            // bus is (nearly) free.  Wait out a pixel DMA if one is still
            // going, and a writer-side sample if one is under way.
            xSemaphoreTake(_sampleMutex, portMAX_DELAY);
            if (_display) _display->waitFlush();
            _sampleToCache();
            xSemaphoreGive(_sampleMutex);
            portENTER_CRITICAL(&_cacheMux);
            c = _cache;
            portEXIT_CRITICAL(&_cacheMux);
        }
        if (!c.valid) return false;        // cache only, and nothing sampled yet
        _hits += c.ms != _lastSeenMs;
        _lastSeenMs = c.ms;
        if (!c.down) return false;
        rawX = c.x; rawY = c.y;
        _lastPressure = c.p;
        return true;
    }

    // Own-pins boards, or the cache turned off: sample now.  On the shared
    // bus, let any in-flight display DMA finish first.
    if (_spi && _display) _display->waitFlush();
    uint16_t p;
    bool down = _sampleBus(rawX, rawY, p);
    _lastPressure = p;
    return down;
}

// One full sample on whichever bus this board uses: medians of X and Y, the
// pressure, and PENIRQ again at the end.  Returns false if the press ended
// during sampling (a spurious single-edge IRQ, or a lift).
bool chipguy_CYD_resistive_touch::_sampleBus(uint16_t &rawX, uint16_t &rawY, uint16_t &pressure) {
    pressure = 0;
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
    pressure = (uint16_t)(p > 4095.0f ? 4095.0f : p);

    // Still pressed after sampling?  Rejects spurious single-edge IRQs.
    return _cydr_irqDown();
}

// GT911 registers: 0x814E status (bit 7 = new data, low nibble = points),
// 0x8150 first point (X lo/hi, Y lo/hi, size lo/hi).  The status must be
// cleared after each read.  Between reports the last state is held, so a
// press doesn't flicker between the controller's ~100 Hz updates.
bool chipguy_CYD_resistive_touch::_gtReg(uint16_t reg, uint8_t *buf, int n) {
    cydr_touchI2cLock();      // the transfer, start to stop: see cydr_touchI2cLock()
    bool ok = _gtRegLocked(reg, buf, n);
    cydr_touchI2cUnlock();
    return ok;
}

bool chipguy_CYD_resistive_touch::_gtRegLocked(uint16_t reg, uint8_t *buf, int n) {
    CYDR_CAP_WIRE.beginTransmission(_capAddr);
    CYDR_CAP_WIRE.write(reg >> 8);
    CYDR_CAP_WIRE.write(reg & 0xFF);
    if (CYDR_CAP_WIRE.endTransmission(false) != 0) return false;
    if (CYDR_CAP_WIRE.requestFrom((int)_capAddr, n) != n) return false;
    for (int i = 0; i < n; i++) buf[i] = CYDR_CAP_WIRE.read();
    return true;
}

bool chipguy_CYD_resistive_touch::_gt911Read(uint16_t &rawX, uint16_t &rawY) {
    uint8_t st;
    if (_gtReg(0x814E, &st, 1) && (st & 0x80)) {
        uint8_t p[6];
        _gtDown = (st & 0x0F) > 0 && _gtReg(0x8150, p, 6);
        if (_gtDown) {
            _gtX = p[0] | p[1] << 8;
            _gtY = p[2] | p[3] << 8;
            _lastPressure = p[4] | p[5] << 8;
        }
        cydr_touchI2cLock();
        CYDR_CAP_WIRE.beginTransmission(_capAddr);
        CYDR_CAP_WIRE.write(0x81); CYDR_CAP_WIRE.write(0x4E); CYDR_CAP_WIRE.write(0);
        CYDR_CAP_WIRE.endTransmission();
        cydr_touchI2cUnlock();
    }
    rawX = _swapXY ? _gtY : _gtX;
    rawY = _swapXY ? _gtX : _gtY;
    return _gtDown;
}

// CST820 (CST816 family) registers, one-byte addressed: 0x02 finger count,
// 0x03 X high nibble (bits 7..6 the event), 0x04 X low, 0x05 Y high nibble,
// 0x06 Y low.  Read on every poll; no status to clear.  A failed read keeps
// the last state, as the GT911 path does between reports.
bool chipguy_CYD_resistive_touch::_cstReg(uint8_t reg, uint8_t *buf, int n) {
    cydr_touchI2cLock();      // the transfer, start to stop: see cydr_touchI2cLock()
    bool ok = _cstRegLocked(reg, buf, n);
    cydr_touchI2cUnlock();
    return ok;
}

bool chipguy_CYD_resistive_touch::_cstRegLocked(uint8_t reg, uint8_t *buf, int n) {
    CYDR_CAP_WIRE.beginTransmission(_capAddr);
    CYDR_CAP_WIRE.write(reg);
    if (CYDR_CAP_WIRE.endTransmission(false) != 0) return false;
    if (CYDR_CAP_WIRE.requestFrom((int)_capAddr, n) != n) return false;
    for (int i = 0; i < n; i++) buf[i] = CYDR_CAP_WIRE.read();
    return true;
}

bool chipguy_CYD_resistive_touch::_cst820Read(uint16_t &rawX, uint16_t &rawY) {
    uint8_t p[5];
    if (_cstReg(0x02, p, 5)) {
        _gtDown = (p[0] & 0x0F) > 0;
        if (_gtDown) {
            _gtX = (uint16_t)(p[1] & 0x0F) << 8 | p[2];
            _gtY = (uint16_t)(p[3] & 0x0F) << 8 | p[4];
        }
        _lastPressure = 0;
    }
    rawX = _swapXY ? _gtY : _gtX;
    rawY = _swapXY ? _gtX : _gtY;
    return _gtDown;
}

const char *chipguy_CYD_resistive_touch::chipName() const {
    switch (_chip) {
        case CYDR_TOUCH_GT911:  return "GT911";
        case CYDR_TOUCH_CST820: return "CST820";
        case CYDR_TOUCH_FT6336: return "FT6336";
        case CYDR_TOUCH_NONE:   return "none";
        default:                return "XPT2046";
    }
}

bool chipguy_CYD_resistive_touch::readRaw(uint16_t &rawX, uint16_t &rawY) {
    if (_chip == CYDR_TOUCH_GT911)  return _gt911Read(rawX, rawY);
    // The FT6336's point registers (0x02 count, 0x03..0x06 X / Y) are laid out
    // as the CST820's.
    if (_chip == CYDR_TOUCH_CST820 || _chip == CYDR_TOUCH_FT6336) return _cst820Read(rawX, rawY);
    if (_chip == CYDR_TOUCH_NONE) return false;
    return _readRawSample(rawX, rawY);
}

bool chipguy_CYD_resistive_touch::read(uint16_t &x, uint16_t &y) {
    uint16_t rawX, rawY;
    if (!readRaw(rawX, rawY)) {
        _hasLast = false;       // released: forget the last point for jump filter
        _jumpCount = 0;
        return false;
    }

    const long W = _nativeW - 1;    // 239 or 319
    const long H = _nativeH - 1;    // 319 or 479
    long px, py;
    if (capacitive()) {
        // Already in panel pixels.
        px = constrain((long)rawX, 0L, W);
        py = constrain((long)rawY, 0L, H);
    } else {
        // Pressure filter: reject touches lighter than the configured threshold.
        if (_minPressure > 0 && _lastPressure < _minPressure) return false;
        // Clamp to the calibration rectangle, then map to physical pixels
        // (before rotation).
        px = map((long)constrain(rawX, _xMin, _xMax), _xMin, _xMax, 0, W);
        py = map((long)constrain(rawY, _yMin, _yMax), _yMin, _yMax, 0, H);
    }

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
