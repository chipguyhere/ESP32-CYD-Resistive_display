/*
 * platform_ESP32C5.cpp - THIS FILE ONLY APPLIES TO THE ESP32-C5 (NM-CYD-C5).
 * On any other chip it compiles to nothing.  See platform.h.
 *
 * The board runs from USB only (no battery connector), has no light sensor,
 * and its amplifier is always on (SHUTDOWN tied to GND).  The card shares
 * the display's SPI bus, and the C5 has only that one general-purpose SPI
 * controller, which the display driver owns -- so the card is opened with
 * ESP-IDF's SD-over-SPI driver as one more device on SPI2_HOST, never with
 * Arduino's SD library.  See README-sdcard-ESP32-C5.md.
 */

#include <sdkconfig.h>
#if defined(CONFIG_IDF_TARGET_ESP32C5)

#include "platform.h"
#include <esp_vfs_fat.h>
#include <driver/sdspi_host.h>

BoardFacts platform_facts(cydr_model_t m, bool *known) {
    *known = m == CYDR_MODEL_NM_CYD_C5;
    return {false, false, false, false};
}

String platform_sd_text() {
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;                  // the display's bus, already initialized
    host.max_freq_khz = 20000;
    sdspi_device_config_t dev = SDSPI_DEVICE_CONFIG_DEFAULT();
    dev.gpio_cs = (gpio_num_t)cydr_sdCsPin();
    dev.host_id = SPI2_HOST;
    esp_vfs_fat_sdmmc_mount_config_t mc = {};
    mc.format_if_mount_failed = false;
    mc.max_files = 1;
    sdmmc_card_t *card = nullptr;
    if (esp_vfs_fat_sdspi_mount("/sd", &host, &dev, &mc, &card) != ESP_OK)
        return "none / not FAT";
    String s = String("SD, ") +
               String((uint32_t)((uint64_t)card->csd.capacity * card->csd.sector_size >> 20)) +
               " MB (display bus)";
    esp_vfs_fat_sdcard_unmount("/sd", card);
    return s;
}

float platform_battery_volts()  { return 0; }
uint32_t platform_light_mv()    { return 0; }
bool platform_is_dark()         { return false; }
const char *platform_amp_switch_pin() { return ""; }

#endif  // CONFIG_IDF_TARGET_ESP32C5
