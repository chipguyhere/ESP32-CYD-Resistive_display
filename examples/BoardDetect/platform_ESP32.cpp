/*
 * platform_ESP32.cpp - THIS FILE ONLY APPLIES TO THE CLASSIC ESP32 CYDs
 * (ESP32-2432S028, ESP32-32E, ESP32-024, ESP32-035, ESP32-2432S032, ...).
 * On any other chip it compiles to nothing; see platform.h.
 */

#include <sdkconfig.h>
#if defined(CONFIG_IDF_TARGET_ESP32)

#include "platform.h"
#include <SD.h>

// The light sensor reads 0 (shown as the converter's ~142 mV floor) in
// anything but near-darkness, and rises only in the dark: about 1.67 V on the
// 2432S028 and ESP32-035, 0.64 V on the ESP32-024 (bench, 2026-10-18 and
// 2026-10-23).  300 mV splits them.  The default 11 dB attenuation spans both.
// The ESP32-035's sensor sees the board's own backlight: in a closed box with
// the screen lit it read only ~0.2 V.
static const uint32_t DARK_MV = 300;

// Every board has a speaker amplifier; the power switch is GPIO4 (LOW = on)
// where there is one.  The light sensor is on GPIO34; on the ESP32-32E boards
// GPIO34 is the battery reading instead (half the cell voltage).
BoardFacts platform_facts(cydr_model_t m, bool *known) {
    *known = true;
    switch (m) {
        case CYDR_MODEL_2432S028:  return {false, false, true,  false};
        case CYDR_MODEL_32E_28:
        case CYDR_MODEL_32E_32:
        case CYDR_MODEL_32E_35_40: return {true,  true,  false, true};
        case CYDR_MODEL_024:
        case CYDR_MODEL_035:
        case CYDR_MODEL_2432S032:
        case CYDR_MODEL_2432S032C: return {true,  false, true,  false};
        // Battery with an SW1 power key, GPIO4 the red LED (bench, 2026-10-31).
        // The light sensor is taken from GPIO34's reading, not yet a dark test.
        case CYDR_MODEL_2432S024C:
        case CYDR_MODEL_JC2432W328: return {true,  false, true,  false};
        // Micro-USB power only; a photoresistor is fitted (bench, 2026-11-02).
        case CYDR_MODEL_3248S035C:  return {false, false, true,  false};
        default: *known = false;   return {false, false, false, cydr_redLedPin() == 22};
    }
}

static const char *cardTypeName(uint8_t t) {
    switch (t) {
        case CARD_MMC:  return "MMC";
        case CARD_SD:   return "SD";
        case CARD_SDHC: return "SDHC";
        default:        return "unknown";
    }
}

String platform_sd_text() {
    // The card slot has the same pins on every board: VSPI (the default SPI)
    // with CS on GPIO5.  See README-sdcard-ESP32.md.
    if (!SD.begin(cydr_sdCsPin())) return "none / not FAT";
    String s = String(cardTypeName(SD.cardType())) + ", " +
               String((uint32_t)(SD.cardSize() >> 20)) + " MB";
    SD.end();
    return s;
}

// GPIO34 is ADC1, so these reads work with WiFi on.
float platform_battery_volts() { return analogReadMilliVolts(34) * 2 / 1000.0f; }
uint32_t platform_light_mv()   { return analogReadMilliVolts(34); }
bool platform_is_dark()        { return analogReadMilliVolts(34) > DARK_MV; }
const char *platform_amp_switch_pin() { return "GPIO4"; }

#endif  // CONFIG_IDF_TARGET_ESP32
