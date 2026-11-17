/*
 * platform_ESP32S3.cpp - THIS FILE ONLY APPLIES TO THE ESP32-S3 BOARD
 * (ES3C28P, and the ES3N28P without touch).  On any other chip it compiles
 * to nothing.  See platform.h.
 *
 * The card: a 4-bit SDMMC slot (CLK 38, CMD 40, D0 39, D1 41, D2 48,
 * D3 47; 10k pull-ups on the board), mounted with SD_MMC.  The battery:
 * GPIO9 (ADC1) through a 200k / 200k divider, so half the cell voltage.  The
 * amplifier: its SHUTDOWN pin on GPIO1, pulled up -- off until GPIO1 is
 * driven LOW.  No light sensor.  See README-board-ESP32-S3.md.
 */

#include <sdkconfig.h>
#if defined(CONFIG_IDF_TARGET_ESP32S3)

#include "platform.h"
#include <SD_MMC.h>

BoardFacts platform_facts(cydr_model_t m, bool *known) {
    *known = m == CYDR_MODEL_ES3C28P || m == CYDR_MODEL_ES3N28P;
    return {true, true, false, true};
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
    SD_MMC.setPins(38, 40, 39, 41, 48, 47);   // CLK, CMD, D0, D1, D2, D3
    if (!SD_MMC.begin("/sdcard", false)) return "none / not FAT";   // 4-bit
    String s = String(cardTypeName(SD_MMC.cardType())) + ", " +
               String((uint32_t)(SD_MMC.cardSize() >> 20)) + " MB (SDMMC)";
    SD_MMC.end();
    return s;
}

float platform_battery_volts()  { return analogReadMilliVolts(9) * 2 / 1000.0f; }
uint32_t platform_light_mv()    { return 0; }
bool platform_is_dark()         { return false; }
const char *platform_amp_switch_pin() { return "GPIO1 (LOW = on)"; }

#endif  // CONFIG_IDF_TARGET_ESP32S3
