/*
 * player.cpp - MP3 playlist player: SD scan + decoder task.  See player.h.
 */

#include "player.h"

#include <algorithm>

#include <SPI.h>
#include <SD.h>
#include <AudioFileSourceSD.h>
#include <AudioGeneratorMP3.h>
#include "AudioOutputCydDac.h"
#include "id3scan.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

// microSD slot: CS on GPIO5, data on the default VSPI pins (18/19/23).
// Same wiring on every board this library supports (2432S028, 32E 2.8" /
// 3.2" / 3.5" / 4.0", ESP32-024), so this mount needs no board check.
// SD.begin() defaults to a 4 MHz clock; the on-board slot is a short trace
// and runs fine faster, and slow reads are what cause DAC underruns.
static constexpr int kSdCs = 5;
static constexpr uint32_t kSdHz = 20000000;

// libmad wants a fair amount of stack, and SD/FAT reads happen on top of it.
static constexpr uint32_t kTaskStack = 16 * 1024;
// Core 0 keeps LVGL's core (1) free — right for a sketch with no WiFi.  A
// sketch that uses WiFi should move this to core 1 instead: WiFi and LwIP
// own core 0 at priorities 23 and 18, and this task at priority 3 would sit
// under them.  On core 1 it only outranks loopTask (1), which is the point.
// See README-sound.md in chipguy_ESP32-CYD-Resistive_display, rule 3.
static constexpr int kTaskCore = 0;
static constexpr int kTaskPriority = 3;

enum Cmd : uint8_t { CMD_PLAY, CMD_TOGGLE_PAUSE, CMD_STOP, CMD_NEXT, CMD_PREV };
struct CmdMsg { Cmd cmd; int arg; };

static std::vector<TrackInfo> tracks;
static QueueHandle_t cmdQueue;
static portMUX_TYPE statusMux = portMUX_INITIALIZER_UNLOCKED;
static PlayerStatus status = { PLAYER_STOPPED, -1, 0, 0, 0, "", "" };

// Decoder objects live on the audio task only.
static AudioOutputCydDac *out;
static AudioGeneratorMP3 *mp3;
static AudioFileSourceSD *file;

// ---------------------------------------------------------------------------
// Playlist

static bool isMp3Name(const char *name) {
    // Skip dot-files: macOS drops "._song.mp3" AppleDouble metadata next to
    // every file it copies, and those are not MP3 content.
    if (name[0] == '.') return false;
    size_t n = strlen(name);
    return n > 4 && strcasecmp(name + n - 4, ".mp3") == 0;
}

static void scanRoot() {
    tracks.clear();
    File root = SD.open("/");
    if (!root) return;
    for (File f = root.openNextFile(); f; f = root.openNextFile()) {
        if (!f.isDirectory() && isMp3Name(f.name()))
            tracks.push_back({ String(f.name()), String(), String() });
        f.close();
    }
    root.close();
    std::sort(tracks.begin(), tracks.end(),
              [](const TrackInfo &a, const TrackInfo &b) { return strcasecmp(a.file.c_str(), b.file.c_str()) < 0; });

    // Tags: a few sector reads per file, see id3scan.h.
    for (auto &t : tracks) {
        String path = "/" + t.file;
        id3_read(path.c_str(), t.title, t.artist);
    }
}

// ---------------------------------------------------------------------------
// Status (audio task writes, UI reads)

static void setStatus(PlayerState st, int track) {
    portENTER_CRITICAL(&statusMux);
    status.state = st;
    status.track = track;
    if (st == PLAYER_STOPPED) {
        status.pos = status.size = 0;
        status.rate = 0;
        status.title[0] = status.artist[0] = 0;
    }
    portEXIT_CRITICAL(&statusMux);
}

static void updateProgress() {
    uint32_t pos = file ? file->getPos() : 0;
    uint32_t size = file ? file->getSize() : 0;
    int rate = (mp3 && mp3->isRunning() && out) ? out->dacRate() : 0;
    portENTER_CRITICAL(&statusMux);
    status.pos = pos;
    status.size = size;
    status.rate = rate;
    portEXIT_CRITICAL(&statusMux);
}

// ---------------------------------------------------------------------------
// Audio task

static int currentTrack = -1;
static bool paused = false;

static void stopTrack() {
    if (mp3 && mp3->isRunning()) mp3->stop();   // also flushes the output to mid-scale
    delete file; file = nullptr;
    paused = false;
}

// Where the audio starts: past an ID3v2 tag, if the file opens with one.
// The MP3 decoder doesn't know tags; it hunts through one for a frame header,
// and a tag with album art in it can be hundreds of KB.  (ESP8266Audio's
// AudioFileSourceID3 skips it too, but a byte at a time, inside the decoder's
// first loop() call.)  Title and artist come from id3scan instead.
static uint32_t audioStart(AudioFileSource *f) {
    uint8_t h[10];
    if (f->read(h, 10) != 10 || memcmp(h, "ID3", 3) != 0) return 0;
    const uint32_t size = (uint32_t)(h[6] & 0x7F) << 21 | (uint32_t)(h[7] & 0x7F) << 14
                        | (uint32_t)(h[8] & 0x7F) << 7  | (h[9] & 0x7F);
    return 10 + size + ((h[5] & 0x10) ? 10 : 0);   // header + tag (+ footer)
}

// Called by the decoder on every decoding error.  AudioGeneratorMP3::loop()
// retries a bad frame at once, inside the same loop() call, so on a damaged
// file -- or anything else that isn't audio -- it can go seconds without
// returning to audioTask(), and the task watchdog fires.  Sleeping here paces
// that search.  The first few errors of each track go to Serial: a file that
// keeps producing them is damaged, or isn't really MP3.  Not logged:
// MAD_ERROR_BUFLEN (0x1), which is how every track ends -- the decoder asks
// for another frame and the file has none.
static uint16_t decodeErrors = 0;     // this track's; reset when one starts

static void onDecodeError(void *, int code, const char *text) {
    if (code != 0x1 && decodeErrors < 20 && currentTrack >= 0)
        Serial.printf("player: %s: %s (code 0x%x)\n", tracks[currentTrack].file.c_str(), text, code);
    if (decodeErrors < 0xFFFF) decodeErrors++;
    vTaskDelay(1);
}

static bool startTrack(int idx) {
    stopTrack();
    out->setAmpEnabled(false);            // stays off until the decoder is running
    if (idx < 0 || idx >= (int)tracks.size()) {
        currentTrack = -1;
        setStatus(PLAYER_STOPPED, -1);
        return false;
    }

    String path = "/" + tracks[idx].file;
    file = new AudioFileSourceSD(path.c_str());
    if (!file->isOpen()) {
        Serial.printf("player: can't open %s\n", path.c_str());
        delete file; file = nullptr;
        currentTrack = -1;
        setStatus(PLAYER_STOPPED, -1);
        return false;
    }
    file->seek(audioStart(file), SEEK_SET);
    decodeErrors = 0;

    currentTrack = idx;
    setStatus(PLAYER_STOPPED, idx);       // clears stale title/artist
    portENTER_CRITICAL(&statusMux);
    strlcpy(status.title, tracks[idx].title.c_str(), sizeof(status.title));
    strlcpy(status.artist, tracks[idx].artist.c_str(), sizeof(status.artist));
    portEXIT_CRITICAL(&statusMux);
    if (!mp3->begin(file, out)) {
        Serial.printf("player: decoder failed on %s\n", path.c_str());
        stopTrack();
        currentTrack = -1;
        setStatus(PLAYER_STOPPED, -1);
        return false;
    }
    Serial.printf("player: playing %s\n", path.c_str());
    out->setAmpEnabled(true);             // no-op on the first track (ensureRunning did it)
    setStatus(PLAYER_PLAYING, idx);
    return true;
}

static void handleCommand(const CmdMsg &m) {
    switch (m.cmd) {
    case CMD_PLAY:
        startTrack(m.arg);
        break;
    case CMD_TOGGLE_PAUSE:
        if (mp3->isRunning()) {
            paused = !paused;
            if (paused) out->flush();           // park the DAC at mid-scale
            out->setAmpEnabled(!paused);        // and mute the amp while parked
            setStatus(paused ? PLAYER_PAUSED : PLAYER_PLAYING, currentTrack);
        } else if (currentTrack >= 0) {
            startTrack(currentTrack);
        } else if (!tracks.empty()) {
            startTrack(0);
        }
        break;
    case CMD_STOP:
        stopTrack();
        out->setAmpEnabled(false);          // DAC stays parked at 128; amp off
        setStatus(PLAYER_STOPPED, currentTrack);
        break;
    case CMD_NEXT:
        if (!tracks.empty()) startTrack((currentTrack + 1) % tracks.size());
        break;
    case CMD_PREV:
        if (!tracks.empty()) startTrack((currentTrack + (int)tracks.size() - 1) % tracks.size());
        break;
    }
}

static bool requestedDither = true;
static uint8_t requestedVolume = 70;

static void audioTask(void *) {
    out = new AudioOutputCydDac();
    out->setVolume(requestedVolume);
    out->setDither(requestedDither);
    mp3 = new AudioGeneratorMP3();
    mp3->RegisterStatusCB(onDecodeError, nullptr);

    uint32_t lastProgress = 0;
    uint32_t lastUnderruns = 0;
    for (;;) {
        bool active = mp3->isRunning() && !paused;

        CmdMsg m;
        // While decoding, just poll the queue; otherwise block on it.
        if (xQueueReceive(cmdQueue, &m, active ? 0 : pdMS_TO_TICKS(50)))
            handleCommand(m);

        if (mp3->isRunning() && !paused) {
            if (!mp3->loop()) {
                // End of file (or a decode error): advance, wrapping to the
                // start of the list.
                Serial.println("player: track finished");
                int next = (currentTrack + 1) % tracks.size();
                if (!startTrack(next)) setStatus(PLAYER_STOPPED, -1);
            }
            // Every pass, not only when the DMA write blocked.  If a file ever
            // decodes slower than real time, the DMA never fills, the write
            // never blocks, and loop() -> refuse -> loop() would run with no
            // sleep at all and trip the task watchdog.  One tick per chunk
            // (tens of ms) is nothing against a 12-chunk DMA queue.
            vTaskDelay(1);
        }

        if (millis() - lastProgress >= 200) {
            lastProgress = millis();
            updateProgress();
            if (out->underruns() != lastUnderruns) {
                lastUnderruns = out->underruns();
                Serial.printf("player: DAC underruns: %lu\n", (unsigned long)lastUnderruns);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Public API

bool player_begin() {
    // Amplifier off first (GPIO4, active LOW; the board's 10 k pull-up has it
    // off already, this just makes it explicit).  The output class turns it
    // on once the DAC is running and parked at mid-scale.  Not on the
    // ESP32-2432S028 or ESP32-024, where GPIO4 is the red LED.
    if (AudioOutputCydDac::hasAmpPin()) {
        pinMode(AudioOutputCydDac::kAmpEnablePin, OUTPUT);
        digitalWrite(AudioOutputCydDac::kAmpEnablePin, HIGH);
    }

    // Park the speaker input at 0 V now.  GPIO26 boots as a floating input,
    // and the DAC's first enable would otherwise be a float-to-0 V step
    // through the amplifier; from a driven low it's 0 V to 0 V, then the
    // output class ramps up smoothly.
    pinMode(26, OUTPUT);
    digitalWrite(26, LOW);

    if (!SD.begin(kSdCs, SPI, kSdHz)) {
        Serial.println("player: SD mount failed at 20 MHz, retrying at 4 MHz");
        SD.end();
        if (!SD.begin(kSdCs, SPI, 4000000)) {
            Serial.println("player: SD mount failed");
            return false;
        }
    }
    scanRoot();
    Serial.printf("player: %u MP3 file(s) in root\n", (unsigned)tracks.size());
    for (auto &t : tracks) Serial.printf("  %-14s %s\n", t.file.c_str(), t.label().c_str());

    cmdQueue = xQueueCreate(8, sizeof(CmdMsg));
    xTaskCreatePinnedToCore(audioTask, "audio", kTaskStack, nullptr,
                            kTaskPriority, nullptr, kTaskCore);
    return true;
}

const std::vector<TrackInfo> &player_tracks() { return tracks; }

static void send(Cmd c, int arg = 0) {
    if (!cmdQueue) return;
    CmdMsg m = { c, arg };
    xQueueSend(cmdQueue, &m, 0);
}

void player_play(int track)   { send(CMD_PLAY, track); }
void player_toggle_pause()    { send(CMD_TOGGLE_PAUSE); }
void player_stop()            { send(CMD_STOP); }
void player_next()            { send(CMD_NEXT); }
void player_prev()            { send(CMD_PREV); }

// Volume is a single byte read per-sample by the output; no queue needed.
void player_set_volume(uint8_t pct) {
    requestedVolume = pct > 100 ? 100 : pct;
    if (out) out->setVolume(requestedVolume);
}
uint8_t player_volume() { return out ? out->volume() : requestedVolume; }

void player_set_dither(bool on) {
    requestedDither = on;
    if (out) out->setDither(on);
    Serial.printf("player: dither %s\n", on ? "ON (TPDF + 1st-order shaping)" : "OFF (round)");
}
bool player_dither() { return requestedDither; }

PlayerStatus player_status() {
    PlayerStatus s;
    portENTER_CRITICAL(&statusMux);
    s = status;
    portEXIT_CRITICAL(&statusMux);
    return s;
}
