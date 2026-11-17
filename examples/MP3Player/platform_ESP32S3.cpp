/*
 * platform_ESP32S3.cpp - THIS FILE ONLY APPLIES TO THE ESP32-S3 (ES3C28P /
 * ES3N28P).  On any other chip it compiles to nothing; see platform.h.
 *
 * The card: a 4-bit SDMMC slot (CLK 38, CMD 40, D0 39, D1 41, D2 48, D3 47;
 * 10k pull-ups on the board), on the S3's SDMMC host -- its own pins, nothing
 * shared with the display -- mounted with Arduino's SD_MMC.  See
 * README-sdcard-ESP32-S3.md.
 *
 * The output: AudioOutputES8311S3, I2S into the ES8311 codec, then the
 * SC8002B amplifier, whose SHUTDOWN pin is GPIO1 (pulled up: off until
 * driven LOW).  The music reaches the codec at its full 16 bits.
 */

#include <sdkconfig.h>
#if defined(CONFIG_IDF_TARGET_ESP32S3)

#include "platform.h"
#include "AudioOutputES8311S3.h"
#include <SD_MMC.h>

void platform_audio_idle() {
    // Amplifier off (the board's pull-up has it off already; this makes it
    // explicit).  The player turns it on once the codec is running.
    pinMode(AudioOutputES8311S3::kAmpPin, OUTPUT);
    digitalWrite(AudioOutputES8311S3::kAmpPin, HIGH);
}

bool platform_card_mount() {
    SD_MMC.setPins(38, 40, 39, 41, 48, 47);   // CLK, CMD, D0, D1, D2, D3
    if (SD_MMC.begin("/sdcard", false)) return true;          // 4-bit
    Serial.println("player: SD_MMC 4-bit mount failed, retrying 1-bit");
    SD_MMC.end();
    return SD_MMC.begin("/sdcard", true);
}

fs::FS &platform_card() { return SD_MMC; }

CydAudioOut *platform_new_output() { return new AudioOutputES8311S3(); }

int platform_output_bits() { return 16; }

// Core 0 keeps LVGL's core (1) free, as on the classic ESP32; a sketch with
// WiFi should move it to core 1 (see platform_ESP32.cpp).
int platform_audio_core() { return 0; }

#endif  // CONFIG_IDF_TARGET_ESP32S3
