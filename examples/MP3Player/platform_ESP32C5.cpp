/*
 * platform_ESP32C5.cpp - THIS FILE ONLY APPLIES TO THE ESP32-C5 (NM-CYD-C5).
 * On any other chip it compiles to nothing; the classic ESP32's version is
 * platform_ESP32.cpp.  See platform.h.
 *
 * The card: it shares the display's SPI bus (SCK 6, MOSI 7, MISO 2; card CS
 * on GPIO10), and the ESP32-C5 has only that one general-purpose SPI
 * controller, which the display driver already owns.  So the card is mounted
 * through ESP-IDF's SD-over-SPI driver as one more device on SPI2_HOST --
 * never with Arduino's SD library, whose SPIClass would set up the same
 * controller underneath the display.  The FAT volume is then wrapped in an
 * fs::FS, so the shared player code reads it exactly as it reads Arduino's SD
 * on the classic boards.  See README-sdcard-ESP32-C5.md.
 *
 * The output: AudioOutputC5Pdm, the I2S peripheral's PCM-to-PDM converter on
 * GPIO26.  The amplifier on this board is always on.
 */

#include <sdkconfig.h>
#if defined(CONFIG_IDF_TARGET_ESP32C5)

#include "platform.h"
#include "AudioOutputC5Pdm.h"
#include <esp_vfs_fat.h>
#include <driver/sdspi_host.h>
#include <vfs_api.h>
#include <memory>

#include <chipguy_CYD_resistive_display.h>   // cydr_sdCsPin()
static constexpr int kSdKhz = 20000;    // tested on the shared bus; 40 MHz untried
static const char *const kMount = "/sd";

void platform_audio_idle() {
    // No amplifier switch on this board.  Park the speaker input at 0 V: the
    // output class ramps it up to mid-scale when it starts.
    pinMode(26, OUTPUT);
    digitalWrite(26, LOW);
}

// The FAT volume ESP-IDF mounts at /sd, seen through Arduino's fs::FS --
// what Arduino's SD library itself does under the hood.
static std::shared_ptr<VFSImpl> s_vfs = std::make_shared<VFSImpl>();
static fs::FS s_card(s_vfs);
static sdmmc_card_t *s_sd = nullptr;

bool platform_card_mount() {
    if (s_sd) return true;
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;                  // the display's bus, already initialized
    host.max_freq_khz = kSdKhz;
    sdspi_device_config_t dev = SDSPI_DEVICE_CONFIG_DEFAULT();
    dev.gpio_cs = (gpio_num_t)cydr_sdCsPin();
    dev.host_id = SPI2_HOST;
    esp_vfs_fat_sdmmc_mount_config_t mc = {};
    mc.format_if_mount_failed = false;
    mc.max_files = 4;
    mc.allocation_unit_size = 16 * 1024;
    // The SPI master driver takes turns between the panel, touch and the
    // card one transaction at a time, so this is safe while LVGL draws.
    esp_err_t err = esp_vfs_fat_sdspi_mount(kMount, &host, &dev, &mc, &s_sd);
    if (err != ESP_OK) {
        Serial.printf("player: card mount failed: %s\n", esp_err_to_name(err));
        s_sd = nullptr;
        return false;
    }
    s_vfs->mountpoint(kMount);
    Serial.printf("player: card %s, %llu MB, %u kHz\n", s_sd->cid.name,
                  (unsigned long long)s_sd->csd.capacity * s_sd->csd.sector_size / 1048576,
                  (unsigned)s_sd->real_freq_khz);
    return true;
}

fs::FS &platform_card() { return s_card; }

CydAudioOut *platform_new_output() { return new AudioOutputC5Pdm(); }

int platform_output_bits() { return 1; }

// One core.  The decoder task (priority 3) outranks loop() and LVGL (1), so
// rendering never starves it; WiFi (23) and LwIP (18) still outrank it.
int platform_audio_core() { return 0; }

#endif  // CONFIG_IDF_TARGET_ESP32C5
