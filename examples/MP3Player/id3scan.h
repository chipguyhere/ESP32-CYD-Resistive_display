/*
 * id3scan.h - Read just the title and artist out of an MP3's ID3 tag.
 *
 * ESP8266Audio's AudioFileSourceID3 gets the same information, but it reads
 * every byte of every frame one File::read() at a time, album art included,
 * which is far too slow for scanning a whole card at boot.  This walks the
 * ID3v2 frame headers and seek()s past everything except TIT2 / TPE1, then
 * falls back to the 128-byte ID3v1 trailer.  Strings come back as UTF-8.
 */

#pragma once

#include <Arduino.h>
#include <FS.h>

// Returns true if at least one of title / artist was found.  Either string
// may be left empty.
// fs: the card (Arduino's SD on the classic ESP32, the C5's shared-bus mount).
bool id3_read(fs::FS &fs, const char *path, String &title, String &artist);
