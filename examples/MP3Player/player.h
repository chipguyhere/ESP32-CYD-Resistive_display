/*
 * player.h - MP3 playlist player for the CYD.
 *
 * Scans the SD card root for *.mp3, then decodes them (ESP8266Audio, libmad)
 * in a FreeRTOS task on core 0 so LVGL on core 1 never waits on the decoder.
 *
 * The UI talks to the player only through these functions.  Commands go
 * through a queue; status comes back as a snapshot copied under a spinlock.
 * Nothing here touches LVGL, and the task never calls back into the UI.
 */

#pragma once

#include <Arduino.h>
#include <vector>

enum PlayerState : uint8_t {
    PLAYER_STOPPED,
    PLAYER_PLAYING,
    PLAYER_PAUSED,
};

struct PlayerStatus {
    PlayerState state;
    int track;            // index into player_tracks(), or -1
    uint32_t pos, size;   // file position / size, for a progress bar
    int rate;             // decoded sample rate, 0 until the first frame
    char title[64];       // copied from the track's TrackInfo when it starts
    char artist[64];
};

// Mount the SD card and build the playlist.  Returns false if the card
// didn't mount.  Call once from setup(); safe to call before or after
// lv_setup.begin() — the SD card is on VSPI, the display on HSPI.
bool player_begin();

struct TrackInfo {
    String file;      // bare filename in the card's root, e.g. "song.mp3"
    String title;     // from the ID3 tag, or "" if none
    String artist;
    // "Title - Artist", or whichever half exists, or the filename.
    String label() const {
        if (title.length() && artist.length()) return title + " - " + artist;
        if (title.length()) return title;
        if (artist.length()) return artist;
        return file;
    }
};

// The playlist, scanned (with ID3 title/artist) at player_begin().
const std::vector<TrackInfo> &player_tracks();

void player_play(int track);          // start a track from the beginning
void player_toggle_pause();           // play/pause the current track
void player_stop();
void player_next();
void player_prev();
void player_set_volume(uint8_t pct);  // 0..100
uint8_t player_volume();

// TPDF dither + first-order noise shaping on the 8-bit quantizer.  Off is
// plain rounding.  Undithered, quiet passages crackle: each 1-LSB step is a
// click of fixed size, so it gets relatively louder as the volume goes down.
void player_set_dither(bool on);
bool player_dither();


PlayerStatus player_status();
